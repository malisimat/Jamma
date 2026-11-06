#pragma once

#include "PerformanceGroups.h"
#include <cstdint>
#include <memory>

namespace engine
{
    class Station;
    class LoopTake;
    class Loop;

    struct GroupMuteApplication
    {
        static constexpr std::size_t MaxTargets = 32768u;
        struct StationState { std::shared_ptr<Station> Owner; bool Muted = false; };
        struct TakeState { std::shared_ptr<LoopTake> Take; bool Muted = false; };
        struct LoopState { std::shared_ptr<Loop> AudioLoop; bool Muted = false; std::shared_ptr<LoopTake> Take; };
        std::uint64_t Version = 0u;
        std::uint64_t TopologyRevision = 0u; // Accepted rig membership at publication.
        std::vector<StationState> Stations;
        std::vector<TakeState> Takes;
        std::vector<LoopState> Loops;
        void ApplyAudio() const noexcept;
    };

    // All traversal/identity work is job/UI-only under the caller's Scene lock.
    // Prepared application ownership is retired by AudioHost off the callback.
    class PerformanceGroupService
    {
    public:
        using Stations = std::vector<std::shared_ptr<Station>>;
        static MuteGroupSnapshot CaptureMuteConfiguration(const Stations& stations, const GroupMuteApplication* desired = nullptr);
		// Complete raw user intent; source/parent-effective presentation never becomes a child flag.
		static std::shared_ptr<GroupMuteApplication> PrepareRawMuteApplication(
			const Stations& stations, std::uint64_t version, const GroupMuteApplication* desired = nullptr);
		static void SetTargetMuted(GroupMuteApplication& application, const Stations& stations, const GroupTargetId& target, bool muted, bool explicitIntent = false);
		static std::shared_ptr<GroupMuteApplication> FilterMuteApplicationToCurrent(
			const GroupMuteApplication& desired, const Stations& stations, std::uint64_t version);
		static void RestoreSurvivingRawFlags(GroupMuteApplication& current, const GroupMuteApplication& baseline);
		static bool DesiredMuted(const Stations& stations, const GroupTargetId& target, const GroupMuteApplication* desired);
        static SelectionGroupSnapshot CaptureSelectionConfiguration(const Stations& stations, GroupTargetKind depth);
        static void RestoreSelection(const SelectionGroupSnapshot& snapshot, const Stations& stations);
        static std::shared_ptr<const GroupMuteApplication> PrepareMuteApplication(
            const MuteGroupSnapshot& snapshot, const Stations& stations, std::uint64_t version);
        static std::shared_ptr<const GroupMuteApplication> PrepareSelectedTakeInversion(
            const Stations& stations, std::uint64_t version);
        static std::shared_ptr<const GroupMuteApplication> PrepareAllMuted(
            const Stations& stations, bool muted, std::uint64_t version, const GroupMuteApplication* desired = nullptr);
        static std::shared_ptr<Station> ResolveStation(const GroupTargetId& id, const Stations& stations);
        static std::shared_ptr<LoopTake> ResolveTake(const GroupTargetId& id, const Stations& stations);
    };
}
