#include "PerformanceGroupService.h"
#include "Station.h"
#include "Loop.h"
#include <map>
#include <set>
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

std::shared_ptr<GroupMuteApplication> PerformanceGroupService::PrepareRawMuteApplication(
    const Stations& stations, std::uint64_t version, const GroupMuteApplication* desired)
{
    auto application = std::make_shared<GroupMuteApplication>(); application->Version = version;
    std::map<const Station*, bool> parents; std::map<const LoopTake*, bool> takes; std::map<const Loop*, bool> loops;
    if (desired)
    {
        for (const auto& state : desired->Stations) parents[state.Owner.get()] = state.Muted;
        for (const auto& state : desired->Takes) takes[state.Take.get()] = state.Muted;
        for (const auto& state : desired->Loops) loops[state.AudioLoop.get()] = state.Muted;
    }
    for (const auto& station : stations)
    {
        if (!station || station->MembershipRemoving()) continue;
        const auto parent = parents.find(station.get());
        application->Stations.push_back({station, parent == parents.end() ? station->RetainedMuted() : parent->second});
        for (const auto& take : station->GetLoopTakes())
        {
            if (!take || take->IsCaptureCancelled()) continue;
            const auto flag = takes.find(take.get());
            application->Takes.push_back({take, flag == takes.end() ? static_cast<bool>(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED) : flag->second});
            for (const auto& loop : take->GetLoops())
            {
                const auto leaf = loops.find(loop.get());
                application->Loops.push_back({loop, leaf == loops.end() ? static_cast<bool>(loop->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED) : leaf->second, take});
                if (application->Stations.size() + application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
            }
            if (application->Stations.size() + application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
        }
        if (application->Stations.size() + application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
    }
    return application;
}

void PerformanceGroupService::SetTargetMuted(GroupMuteApplication& application, const Stations& stations, const GroupTargetId& target, bool muted, bool explicitIntent)
{
    std::shared_ptr<Station> station;
    for (const auto& member : stations) if (member && member->Id() == target.StationId && !member->MembershipRemoving()) { station = member; break; }
    if (!station) return;
    std::set<const LoopTake*> takes;
    for (const auto& take : station->GetLoopTakes())
        if (take && (target.Kind == GroupTargetKind::Station || take->Id() == target.TakeId)) takes.insert(take.get());
    for (auto& state : application.Stations)
        if (target.Kind == GroupTargetKind::Station && state.Owner == station) state.Muted = muted;
    const bool wholeTake = target.Kind != GroupTargetKind::AudioLoop;
    std::set<const LoopTake*> fanout;
    for (auto& state : application.Takes)
    {
        if (!wholeTake || !takes.contains(state.Take.get())) continue;
        const bool rawChanged = state.Muted != muted;
        // Audition at accepted UI admission, never when a queued image is adopted.
        const bool auditionChanged = explicitIntent && !muted && state.Take->AllowReplacementPlayback();
        if (!explicitIntent || rawChanged || auditionChanged) fanout.insert(state.Take.get());
        state.Muted = muted;
    }
    for (auto& state : application.Loops)
    {
        if (!takes.contains(state.Take.get()) ||
            (wholeTake ? !fanout.contains(state.Take.get()) : state.AudioLoop->Id() != target.EntityId)) continue;
        if (explicitIntent && !muted) state.AudioLoop->AllowReplacementPlayback();
        state.Muted = muted;
    }
}

std::shared_ptr<GroupMuteApplication> PerformanceGroupService::FilterMuteApplicationToCurrent(
    const GroupMuteApplication& desired, const Stations& stations, std::uint64_t version)
{
    std::set<const Station*> parents; std::set<const LoopTake*> takes; std::map<const Loop*,const LoopTake*> loops;
    for (const auto& station : stations)
    {
        if (!station || station->MembershipRemoving()) continue; parents.insert(station.get());
        for (const auto& take : station->GetLoopTakes())
        {
            if (!take || take->IsCaptureCancelled()) continue; takes.insert(take.get());
            for (const auto& loop : take->GetLoops()) loops[loop.get()] = take.get();
        }
    }
    auto result = std::make_shared<GroupMuteApplication>(); result->Version = version;
    for (const auto& state : desired.Stations) if (parents.contains(state.Owner.get())) result->Stations.push_back(state);
    for (const auto& state : desired.Takes) if (takes.contains(state.Take.get())) result->Takes.push_back(state);
    for (const auto& state : desired.Loops)
        if (const auto found = loops.find(state.AudioLoop.get()); found != loops.end() && found->second == state.Take.get()) result->Loops.push_back(state);
    return result; // A bounded previous image can only shrink; new owners retain their own flags.
}

void PerformanceGroupService::RestoreSurvivingRawFlags(GroupMuteApplication& current, const GroupMuteApplication& baseline)
{
    // Current membership owns admission. An externally retained removed/reused-ID owner is never restored.
    std::map<const Station*, bool> stations;
    std::map<const LoopTake*, bool> takes;
    std::map<const Loop*, std::pair<const LoopTake*, bool>> loops;
    for (const auto& state : baseline.Stations) stations[state.Owner.get()] = state.Muted;
    for (const auto& state : baseline.Takes) takes[state.Take.get()] = state.Muted;
    for (const auto& state : baseline.Loops) loops[state.AudioLoop.get()] = {state.Take.get(), state.Muted};
    for (auto& state : current.Stations)
        if (const auto found = stations.find(state.Owner.get()); found != stations.end()) state.Muted = found->second;
    for (auto& state : current.Takes)
        if (const auto found = takes.find(state.Take.get()); found != takes.end()) state.Muted = found->second;
    for (auto& state : current.Loops)
        if (const auto found = loops.find(state.AudioLoop.get()); found != loops.end() && found->second.first == state.Take.get()) state.Muted = found->second.second;
}

bool PerformanceGroupService::DesiredMuted(const Stations& stations, const GroupTargetId& target, const GroupMuteApplication* desired)
{
    const auto image = PrepareRawMuteApplication(stations, 0u, desired);
    if (!image) return false;
    std::shared_ptr<Station> station; bool parent = false;
    for (const auto& state : image->Stations)
        if (state.Owner->Id() == target.StationId) { station = state.Owner; parent = state.Muted; break; }
    if (!station) return false;
    std::map<const LoopTake*, bool> takeFlags;
    for (const auto& state : image->Takes) takeFlags[state.Take.get()] = state.Muted;
    if (target.Kind == GroupTargetKind::Station)
        return parent || (!station->GetLoopTakes().empty() && std::all_of(station->GetLoopTakes().begin(), station->GetLoopTakes().end(),
            [&](const auto& take) { const auto flag = takeFlags.find(take.get()); return take && ((flag != takeFlags.end() && flag->second) || take->IsSourceMuted()); }));
    for (const auto& take : station->GetLoopTakes())
        if (take && take->Id() == target.TakeId)
        {
            const auto flag = takeFlags.find(take.get());
            const bool takeMuted = parent || (flag != takeFlags.end() && flag->second) || take->IsSourceMuted();
            if (target.Kind != GroupTargetKind::AudioLoop) return takeMuted;
            for (const auto& state : image->Loops)
                if (state.Take == take && state.AudioLoop->Id() == target.EntityId)
                    return takeMuted || state.Muted || state.AudioLoop->Mixer()->IsCaptureSourceMuted();
        }
    return false;
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
