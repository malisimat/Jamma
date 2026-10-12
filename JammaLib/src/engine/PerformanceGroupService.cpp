#include "PerformanceGroupService.h"
#include "Station.h"
#include "Loop.h"
#include <map>
#include <string_view>
#include <tuple>

using namespace engine;

void GroupMuteApplication::ApplyAudio() const noexcept
{
    for (const auto& state : Takes) state.Take->SetGroupMutedAudio(state.Muted);
    for (const auto& state : Loops) state.AudioLoop->SetGroupMutedAudio(state.Muted);
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

MuteGroupSnapshot PerformanceGroupService::CaptureMuteConfiguration(const Stations& stations)
{
    MuteGroupSnapshot snapshot;
    for (const auto& station : stations)
    {
        if (station->IsRemote()) continue;
        // Stations have no independent playback mute: their UI operation fans
        // to takes. Store aggregate status while restoring the actual owners.
        snapshot.Targets.push_back({ { GroupTargetKind::Station, station->Id(), {}, {} }, false, station->AllTakesMuted() });
        for (const auto& take : station->GetLoopTakes())
        {
            snapshot.Targets.push_back({ { GroupTargetKind::Take, station->Id(), take->Id(), {} },
                static_cast<bool>(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED), take->IsMuted() });
            for (const auto& loop : take->GetLoops())
                snapshot.Targets.push_back({ { GroupTargetKind::AudioLoop, station->Id(), take->Id(), loop->Id() },
                    static_cast<bool>(loop->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED), loop->IsMuted() });
        }
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
    for (const auto& state : snapshot.Targets)
    {
        const auto& id = state.Target;
        savedMute[{ id.Kind, id.StationId, id.TakeId, id.EntityId }] = state.Muted || state.EffectiveMuted;
    }
    for (const auto& station : stations)
    {
        if (station->IsRemote()) continue;
        for (const auto& take : station->GetLoopTakes())
        {
            const auto takeId = take->Id();
            const auto savedTake = savedMute.find({ GroupTargetKind::Take, station->Id(), takeId, {} });
            application->Takes.push_back({ take, savedTake == savedMute.end() || savedTake->second });
            if (application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
            for (const auto& loop : take->GetLoops())
            {
                const auto savedLoop = savedMute.find({ GroupTargetKind::AudioLoop, station->Id(), takeId, loop->Id() });
                application->Loops.push_back({ loop, savedLoop == savedMute.end() || savedLoop->second });
                if (application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
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
            for (const auto& loop : take->GetLoops()) application->Loops.push_back({ loop, muted });
            if (application->Takes.size() + application->Loops.size() > GroupMuteApplication::MaxTargets) return {};
        }
    }
    return application;
}
