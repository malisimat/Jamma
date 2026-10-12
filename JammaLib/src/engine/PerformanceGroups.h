#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine
{
    enum class GroupTargetKind { Station, Take, AudioLoop, MidiLoop };

    // Ids describe ownership as well as identity, never a GUI/chain position.
    struct GroupTargetId
    {
        GroupTargetKind Kind = GroupTargetKind::Station;
        std::string StationId;
        std::string TakeId;
        std::string EntityId;
    };

    struct GroupMuteState
    {
        GroupTargetId Target;
        bool Muted = false;
        bool EffectiveMuted = false;
    };

    struct GroupSelectionState
    {
        GroupTargetId Target;
        bool Selected = false;
    };

    struct GroupPluginState
    {
        GroupTargetId Owner;
        std::string PluginId;
        std::string Path;
        std::string State; // JAM-compatible encoded full state blob.
        bool Bypass = false;
    };

    struct MuteGroupSnapshot
    {
        std::vector<GroupMuteState> Targets;
        std::vector<GroupPluginState> Plugins;
    };

    struct SelectionGroupSnapshot
    {
        GroupTargetKind Depth = GroupTargetKind::Take;
        // Parent aggregate flags and leaf flags preserve selection depth.
        std::vector<GroupSelectionState> Targets;
    };

    enum class GroupCommand { RecallOrStore, Store, Clear, Add };

    // Owned and accessed exclusively by the Scene job/UI owner. No callback
    // reader: only owner-applied/published target state crosses into audio.
    // Public slot numbers are 1..9, including in serialized records.
    template<class Snapshot>
    class PerformanceGroupRow
    {
    public:
        static constexpr std::size_t SlotCount = 9u;

        const Snapshot* Get(std::size_t slot) const noexcept
        {
            return IsValidSlot(slot) && _slots[slot - 1u]
                ? &*_slots[slot - 1u] : nullptr;
        }

        std::optional<std::size_t> ActiveSlot() const noexcept { return _active; }
        const std::array<std::optional<Snapshot>, SlotCount>& Slots() const noexcept { return _slots; }
        void Restore(std::array<std::optional<Snapshot>, SlotCount> slots, std::optional<std::size_t> active)
        {
            _slots = std::move(slots);
            _active = active && Get(*active) ? active : std::nullopt;
        }

        std::size_t VisibleSlots() const noexcept
        {
            for (std::size_t i = SlotCount; i != 0u; --i)
                if (_slots[i - 1u]) return i;
            return 0u;
        }

        std::optional<std::size_t> LowestEmptySlot() const noexcept
        {
            for (std::size_t i = 0u; i < SlotCount; ++i)
                if (!_slots[i]) return i + 1u;
            return std::nullopt;
        }

        bool Store(std::size_t slot, Snapshot snapshot)
        {
            if (!IsValidSlot(slot)) return false;
            _slots[slot - 1u] = std::move(snapshot);
            _active = slot;
            return true;
        }

        bool Clear(std::size_t slot) noexcept
        {
            if (!IsValidSlot(slot)) return false;
            _slots[slot - 1u].reset();
            if (_active == slot) _active.reset();
            return true;
        }

        // Capture completes before replacement. Failure retains the existing
        // complete slot and last-applied indication. Recall never mutates it.
        template<class Capture, class Apply>
        bool Execute(GroupCommand command, std::size_t slot, Capture&& capture, Apply&& apply)
        {
            if (command == GroupCommand::Add)
            {
                const auto empty = LowestEmptySlot();
                if (!empty) return false;
                slot = *empty;
                command = GroupCommand::Store;
            }
            if (!IsValidSlot(slot)) return false;
            if (command == GroupCommand::Clear) return Clear(slot);
            if (command == GroupCommand::RecallOrStore && Get(slot))
            {
                if (!apply(*Get(slot))) return false;
                _active = slot;
                return true;
            }
            auto snapshot = capture();
            return snapshot ? Store(slot, std::move(*snapshot)) : false;
        }

    private:
        static bool IsValidSlot(std::size_t slot) noexcept { return slot >= 1u && slot <= SlotCount; }
        std::array<std::optional<Snapshot>, SlotCount> _slots;
        std::optional<std::size_t> _active;
    };

    struct PerformanceGroups
    {
        PerformanceGroupRow<MuteGroupSnapshot> Mute;
        PerformanceGroupRow<SelectionGroupSnapshot> Selection;
    };
}
