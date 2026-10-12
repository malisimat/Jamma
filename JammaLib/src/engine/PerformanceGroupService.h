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
        struct TakeState { std::shared_ptr<LoopTake> Take; bool Muted = false; };
        struct LoopState { std::shared_ptr<Loop> AudioLoop; bool Muted = false; };
        std::uint64_t Version = 0u;
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
        static MuteGroupSnapshot CaptureMuteConfiguration(const Stations& stations);
        static SelectionGroupSnapshot CaptureSelectionConfiguration(const Stations& stations, GroupTargetKind depth);
        static void RestoreSelection(const SelectionGroupSnapshot& snapshot, const Stations& stations);
        static std::shared_ptr<const GroupMuteApplication> PrepareMuteApplication(
            const MuteGroupSnapshot& snapshot, const Stations& stations, std::uint64_t version);
        static std::shared_ptr<const GroupMuteApplication> PrepareSelectedTakeInversion(
            const Stations& stations, std::uint64_t version);
        static std::shared_ptr<Station> ResolveStation(const GroupTargetId& id, const Stations& stations);
        static std::shared_ptr<LoopTake> ResolveTake(const GroupTargetId& id, const Stations& stations);
    };
}
