#pragma once

#include "PerformanceGroupService.h"
#include "../actions/KeyAction.h"
#include <array>
#include <functional>

namespace engine
{
    // Scene serializes all access with its non-audio mutex. Key state belongs
    // to the window thread; pending capture is pumped on that thread.
    class PerformanceGroupController
    {
    public:
        using Publish = std::function<void(std::shared_ptr<const GroupMuteApplication>)>;
        const PerformanceGroups& Groups() const noexcept { return _groups; }
        PerformanceGroups& Groups() noexcept { return _groups; }
        bool ConsumeRelease(const actions::KeyAction& action);
        void BlockContextualPress(const actions::KeyAction& action)
        {
            if (action.KeyChar < _held.size() && action.KeyActionType == actions::KeyAction::KEY_DOWN)
            {
                if (!_held[action.KeyChar]) _consumed[action.KeyChar] = false;
                _held[action.KeyChar] = true;
            }
        }
        // Focus loss can be sent by a recalled plugin before its group key-up.
        // Retain release ownership; a fresh down clears stale consumption.
        void ResetKeys() noexcept { _held.fill(false); }
        bool HandleKey(const actions::KeyAction& action, const PerformanceGroupService::Stations& stations,
            GroupTargetKind depth, const Publish& publish);
        bool Execute(bool selection, GroupCommand command, std::size_t slot,
            const PerformanceGroupService::Stations& stations, GroupTargetKind depth, const Publish& publish);
        // UI frame pump for explicit stores; recall never calls plugin state APIs.
        void Pump(const PerformanceGroupService::Stations& stations, const Publish& publish);
        const std::string& Error() const noexcept { return _error; }
        void Acknowledge(std::uint64_t version) { if (_pendingMute && _pendingMute->Version <= version) _pendingMute.reset(); }
        void CancelPending() { _capture.reset(); _pendingMute.reset(); }
    private:
        struct PendingCapture { std::size_t Slot; unsigned int Attempts = 0u; };
        static bool CapturePlugins(MuteGroupSnapshot& snapshot, const PerformanceGroupService::Stations& stations);
        PerformanceGroups _groups;
        std::array<bool, 256> _held{};
        std::array<bool, 256> _consumed{};
        std::optional<PendingCapture> _capture;
        std::uint64_t _version = 0u;
        std::string _error;
        std::shared_ptr<const GroupMuteApplication> _pendingMute;
    };
}
