#pragma once

#include "RigSnapshot.h"
#include <array>
#include <algorithm>

namespace engine
{
    // Window owner only. Audio receives ordinary bounded input edges and owns no ledger.
    // Weak instances do not extend callback lifetimes; clear after readers stop on teardown.
    class ContextualKeyboardInput
    {
    public:
        void Record(const RigSnapshot& rig, const RigSnapshotTrigger& trigger,
            const KeyboardTriggerReceipt& receipt)
        {
            if (!receipt.Release) return;
            const auto same = [&](const Held& held) { return Same(held, trigger.Instance, receipt); };
            if (std::any_of(_held.begin(), _held.end(), same)) return;
            // Each admitted signature occupies one configured keyboard-binding slot.
            const auto capacity = rig.InputDispatch.KeyboardTriggers.size() * Trigger::MaxBindingCount * 2u;
            if (_held.size() < capacity)
                _held.push_back({ trigger.Id, trigger.Instance, receipt, rig.Revision, false });
        }

        void Retire(const std::shared_ptr<Trigger>& instance, const KeyboardTriggerReceipt& receipt)
        {
            std::erase_if(_held, [&](const Held& held) { return Same(held, instance, receipt); });
        }

        bool Observe(const actions::KeyAction& action, bool contextual, bool admissionOpen = true)
        {
            const auto state = action.KeyActionType == actions::KeyAction::KEY_DOWN ? 1u : 0u;
            const bool previouslySuppressed = action.KeyChar < _suppressed.size() && _suppressed[action.KeyChar];
            // A dialog may have closed before the rig transition reopens admission.
            // Preserve the release even though ordinary input cannot enqueue it yet.
            if (contextual || previouslySuppressed || !admissionOpen)
                for (auto& held : _held)
                    if (held.Receipt.Release && held.Receipt.Release->Test(TRIGGER_KEY, action.KeyChar, state))
                        held.PendingRelease = true;
            if (action.KeyChar >= _suppressed.size()) return false;
            if (contextual && state) _suppressed[action.KeyChar] = true;
            if (!state) _suppressed[action.KeyChar] = false;
            return previouslySuppressed;
        }

        void FocusLost()
        {
            for (auto& held : _held)
            {
                held.PendingRelease = true;
                if (held.Receipt.Down.Value < _suppressed.size())
                    _suppressed[held.Receipt.Down.Value] = true;
            }
        }

        // Call only after the existing UI input gate admits this accepted revision.
        void Flush(const RigSnapshot& rig, const base::Action& action)
        {
            std::erase_if(_held, [&](Held& held)
            {
                const auto instance = held.Instance.lock();
                const auto current = std::find_if(rig.Triggers.begin(), rig.Triggers.end(),
                    [&](const auto& trigger) { return trigger.Id == held.Id && trigger.Instance == instance; });
                // Replacement/deletion can be accepted only after existing idle/reset barriers.
                if (!instance || current == rig.Triggers.end()) return true;
                held.Revision = rig.Revision;
                if (!held.PendingRelease) return false;
                instance->QueueAdmittedKeyboardRelease(held.Receipt, held.Revision, action);
                return true;
            });
        }

        void ClearAfterReadersStopped() { _held.clear(); _suppressed.fill(false); }
        std::size_t Outstanding() const noexcept { return _held.size(); }

    private:
        struct Held
        {
            std::string Id;
            std::weak_ptr<Trigger> Instance;
            KeyboardTriggerReceipt Receipt;
            std::uint64_t Revision = 0u;
            bool PendingRelease = false;
        };
        static bool Same(const Held& held, const std::shared_ptr<Trigger>& instance,
            const KeyboardTriggerReceipt& receipt)
        {
            return held.Instance.lock() == instance && held.Receipt.Control == receipt.Control &&
                held.Receipt.Down == receipt.Down && held.Receipt.Release == receipt.Release;
        }
        std::vector<Held> _held;
        std::array<bool, 256u> _suppressed{};
    };
}
