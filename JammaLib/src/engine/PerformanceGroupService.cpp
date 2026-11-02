#include "PerformanceGroupService.h"
#include "Station.h"
#include "Loop.h"
#include <map>
#include <string_view>
#include <tuple>

using namespace engine;

void GroupMuteApplication::ApplyAudio() const noexcept
{
    for (const auto& state : Stations) if (!state.Owner->MembershipRemoving()) state.Owner->SetRetainedMuted(state.Muted);
    for (const auto& state : Takes) if (!state.Take->IsCaptureCancelled()) state.Take->SetGroupMutedAudio(state.Muted);
    for (const auto& state : Loops) if (!state.Take || !state.Take->IsCaptureCancelled()) state.AudioLoop->SetGroupMutedAudio(state.Muted);
}

std::shared_ptr<Station> PerformanceGroupService::ResolveStation(const GroupTargetId& id, const Stations& stations)
{
    for (const auto& station : stations)
        if (!station->IsRemote() && station->Id() == id.StationId) return station;
    return {};
}

std::shared_ptr<LoopTake> PerformanceGroupService::ResolveTake(const GroupTargetId& id, const Stations& stations)
{
    if (auto station = ResolveStation(id, stations))
        for (const auto& take : station->GetLoopTakes())
            if (take->Id() == id.TakeId) return take;
    return {};
}

MuteGroupSnapshot PerformanceGroupService::CaptureMuteConfiguration(const Stations& stations, const GroupMuteApplication* desired)
{
    std::map<const Station*, bool> stationFlags;
    std::map<const LoopTake*, bool> takeFlags;
    std::map<const Loop*, bool> loopFlags;
    if (desired)
    {
        for (const auto& state : desired->Stations) stationFlags[state.Owner.get()] = state.Muted;
        for (const auto& state : desired->Takes) takeFlags[state.Take.get()] = state.Muted;
        for (const auto& state : desired->Loops) loopFlags[state.AudioLoop.get()] = state.Muted;
    }
    MuteGroupSnapshot snapshot;
    for (const auto& station : stations)
    {
        if (station->IsRemote()) continue;
        const auto pendingStation = stationFlags.find(station.get());
        const bool retained = pendingStation == stationFlags.end() ? station->RetainedMuted() : pendingStation->second;
        const auto stationIndex = snapshot.Targets.size();
        snapshot.Targets.push_back({{GroupTargetKind::Station, station->Id(), {}, {}}, retained, retained});
        bool allTakesMuted = !station->GetLoopTakes().empty();
        for (const auto& take : station->GetLoopTakes())
        {
            const auto pendingTake = takeFlags.find(take.get());
            const bool raw = pendingTake == takeFlags.end() ? static_cast<bool>(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED) : pendingTake->second;
            // Source suppression remains recallable, but parent intent is saved
            // exactly once on Station and must never become child raw flags.
            const bool effective = raw || take->IsSourceMuted();
            snapshot.Targets.push_back({{GroupTargetKind::Take, station->Id(), take->Id(), {}}, raw, effective});
            allTakesMuted &= effective;
            for (const auto& loop : take->GetLoops())
            {
                const auto pendingLoop = loopFlags.find(loop.get());
                const bool leafRaw = pendingLoop == loopFlags.end() ? static_cast<bool>(loop->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED) : pendingLoop->second;
                snapshot.Targets.push_back({{GroupTargetKind::AudioLoop, station->Id(), take->Id(), loop->Id()},
                    leafRaw, leafRaw || loop->Mixer()->IsCaptureSourceMuted()});
            }
        }
        snapshot.Targets[stationIndex].EffectiveMuted = retained || allTakesMuted;
    }
    return snapshot;
}

SelectionGroupSnapshot PerformanceGroupService::CaptureSelectionConfiguration(const Stations& stations, GroupTargetKind depth)
{
    SelectionGroupSnapshot snapshot;
    snapshot.Depth = depth;
    for (const auto& station : stations)
    {
        if (station->IsRemote()) continue;
        snapshot.Targets.push_back({ { GroupTargetKind::Station, station->Id(), {}, {} }, station->IsSelected() });
        for (const auto& take : station->GetLoopTakes())
        {
            snapshot.Targets.push_back({ { GroupTargetKind::Take, station->Id(), take->Id(), {} }, take->IsSelected() });
            for (const auto& loop : take->GetLoops())
                snapshot.Targets.push_back({ { GroupTargetKind::AudioLoop, station->Id(), take->Id(), loop->Id() }, loop->IsSelected() });
            for (const auto& loop : take->GetMidiLoops())
                if (auto model = loop->Model())
                    snapshot.Targets.push_back({ { GroupTargetKind::MidiLoop, station->Id(), take->Id(), loop->Id() }, model->IsSelected() });
        }
    }
    return snapshot;
}

void PerformanceGroupService::RestoreSelection(const SelectionGroupSnapshot& snapshot, const Stations& stations)
{
    for (const auto& state : snapshot.Targets)
    {
        std::shared_ptr<base::GuiElement> target;
        if (state.Target.Kind == GroupTargetKind::Station) target = ResolveStation(state.Target, stations);
        else if (auto take = ResolveTake(state.Target, stations))
        {
            if (state.Target.Kind == GroupTargetKind::Take)
            {
                take->SetGroupSelected(state.Selected);
                continue;
            }
            if (state.Target.Kind == GroupTargetKind::AudioLoop)
                for (const auto& loop : take->GetLoops())
                    if (loop->Id() == state.Target.EntityId) { target = loop; break; }
            if (state.Target.Kind == GroupTargetKind::MidiLoop)
                for (const auto& loop : take->GetMidiLoops())
                    if (loop->Id() == state.Target.EntityId) { target = loop->Model(); break; }
        }
        if (target)
        {
            if (state.Selected) target->base::GuiElement::Select();
            else target->base::GuiElement::DeSelect();
        }
    }
}

std::shared_ptr<const GroupMuteApplication> PerformanceGroupService::PrepareMuteApplication(
    const MuteGroupSnapshot& snapshot, const Stations& stations, std::uint64_t version)
{
    auto application = std::make_shared<GroupMuteApplication>();
    application->Version = version;
    // This lookup is temporary UI/job-owned preparation; views never escape
    // the snapshot. Unknown material was inaudible when the group was stored.
    using TargetKey = std::tuple<GroupTargetKind, std::string_view, std::string_view, std::string_view>;
    std::map<TargetKey, bool> savedMute;
    std::map<std::string_view, bool> savedStationMute;
    for (const auto& state : snapshot.Targets)
    {
        const auto& id = state.Target;
        if (id.Kind == GroupTargetKind::Station) savedStationMute[id.StationId] = state.Muted;
        else savedMute[{ id.Kind, id.StationId, id.TakeId, id.EntityId }] = state.Muted || state.EffectiveMuted;
    }
    for (const auto& station : stations)
    {
        if (station->IsRemote()) continue;
        const auto savedStation = savedStationMute.find(station->Id());
        // Station raw intent is independent of its aggregate display state.
        application->Stations.push_back({station, savedStation == savedStationMute.end() || savedStation->second});
        if (application->Stations.size() > GroupMuteApplication::MaxTargets) return {};
        for (const auto& take : station->GetLoopTakes())
        {
            const auto takeId = take->Id();
            const auto savedTake = savedMute.find({ GroupTargetKind::Take, station->Id(), takeId, {} });
            application->Takes.push_back({ take, savedTake == savedMute.end() || savedTake->second });
            if (application->Stations.size() + application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
            for (const auto& loop : take->GetLoops())
            {
                const auto savedLoop = savedMute.find({ GroupTargetKind::AudioLoop, station->Id(), takeId, loop->Id() });
                application->Loops.push_back({ loop, savedLoop == savedMute.end() || savedLoop->second, take });
                if (application->Stations.size() + application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
            }
        }
    }
    return application;
}

std::shared_ptr<const GroupMuteApplication> PerformanceGroupService::PrepareAllMuted(
    const Stations& stations, bool muted, std::uint64_t version, const GroupMuteApplication* desired)
{
    auto application = std::make_shared<GroupMuteApplication>();
    application->Version = version;
    std::map<const Station*, bool> desiredStations;
    if (desired) for (const auto& state : desired->Stations) desiredStations[state.Owner.get()] = state.Muted;
    for (const auto& station : stations)
    {
        if (station->IsRemote()) continue;
        const auto pending = desiredStations.find(station.get());
        // Global mute changes existing children, not explicit retained parent intent.
        // Global unmute deliberately clears both levels, including empty stations.
        const bool retained = muted && (pending == desiredStations.end() ? station->RetainedMuted() : pending->second);
        application->Stations.push_back({station, retained});
        if (application->Stations.size() > GroupMuteApplication::MaxTargets) return {};
        for (const auto& take : station->GetLoopTakes())
        {
            application->Takes.push_back({ take, muted });
            if (application->Stations.size() + application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
            for (const auto& loop : take->GetLoops())
            {
                application->Loops.push_back({ loop, muted, take });
                if (application->Stations.size() + application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
            }
        }
    }
    return application;
}

std::shared_ptr<const GroupMuteApplication> PerformanceGroupService::PrepareSelectedTakeInversion(
    const Stations& stations, std::uint64_t version)
{
    auto application = std::make_shared<GroupMuteApplication>();
    application->Version = version;
    for (const auto& station : stations)
    {
        if (station->IsRemote()) continue;
        for (const auto& take : station->GetLoopTakes())
        {
            bool selected = station->IsSelected() || take->IsSelected();
            for (const auto& loop : take->GetLoops()) selected |= loop->IsSelected();
            for (const auto& loop : take->GetMidiLoops()) if (auto model = loop->Model()) selected |= model->IsSelected();
            if (!selected) continue;
            // Invert persistent user mute independently; temporary suppression
            // is retained and cannot be released by this operation.
            const bool muted = !(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
            application->Takes.push_back({ take, muted });
            for (const auto& loop : take->GetLoops()) application->Loops.push_back({ loop, muted, take });
            if (application->Stations.size() + application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
        }
    }
    return application;
}
