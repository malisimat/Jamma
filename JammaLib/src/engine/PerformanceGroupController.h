#pragma once

#include "PerformanceGroupService.h"
#include "../actions/KeyAction.h"
#include <array>
#include <functional>
#include <span>

namespace engine
{
    // Scene serializes all access with its non-audio mutex. Key state belongs
    // to the window thread; stores capture mute flags only on that thread.
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
        bool SetAllMuted(bool muted, const PerformanceGroupService::Stations& stations, const Publish& publish);
		bool BeginMuteMarquee(const PerformanceGroupService::Stations& stations);
		bool PreviewMuteMarquee(std::span<const GroupTargetId> targets, const PerformanceGroupService::Stations& stations, const Publish& publish);
		void CommitMuteMarquee() { _marqueeBaseline.reset(); }
		bool CancelMuteMarquee(const PerformanceGroupService::Stations& stations, const Publish& publish);
		bool MuteMarqueeActive() const noexcept { return _marqueeBaseline != nullptr; }
		bool SetTargetMuted(const GroupTargetId& target, bool muted, const PerformanceGroupService::Stations& stations, const Publish& publish);
		bool DesiredMuted(const GroupTargetId& target, const PerformanceGroupService::Stations& stations) const;
		void PrepareForTopology(const PerformanceGroupService::Stations& stations) { CancelMuteMarquee(stations, {}); }
		void RepublishForTopology(const PerformanceGroupService::Stations& stations, const Publish& publish);
        const std::string& Error() const noexcept { return _error; }
        void Acknowledge(std::uint64_t version) { if (_pendingMute && _pendingMute->Version <= version) _pendingMute.reset(); }
        void CancelPending() { _marqueeBaseline.reset(); _pendingMute.reset(); }
    private:
        PerformanceGroups _groups;
        std::array<bool, 256> _held{};
        std::array<bool, 256> _consumed{};
        std::uint64_t _version = 0u;
        std::string _error;
        std::shared_ptr<const GroupMuteApplication> _pendingMute;
		std::shared_ptr<const GroupMuteApplication> _marqueeBaseline;
		bool _PublishDesired(std::shared_ptr<GroupMuteApplication> application, const Publish& publish);
		static bool _SameRawState(const GroupMuteApplication& left, const GroupMuteApplication& right);
    };
}
