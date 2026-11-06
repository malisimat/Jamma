#include "PerformanceGroupController.h"
#include "Station.h"
#include <algorithm>
#include <set>
#include <map>

using namespace engine;

bool PerformanceGroupController::ConsumeRelease(const actions::KeyAction& action)
{
    if (action.KeyChar >= _held.size() || action.KeyActionType != actions::KeyAction::KEY_UP) return false;
    _held[action.KeyChar] = false;
    const bool consumed = _consumed[action.KeyChar];
    _consumed[action.KeyChar] = false;
    return consumed;
}

bool PerformanceGroupController::HandleKey(const actions::KeyAction& action,
    const PerformanceGroupService::Stations& stations, GroupTargetKind depth, const Publish& publish)
{
    if (action.KeyActionType != actions::KeyAction::KEY_DOWN || action.KeyChar >= _held.size()) return false;
    const bool groupKey = action.KeyChar >= 112u && action.KeyChar <= 120u;
    const bool backspace = action.KeyChar == 8u;
    if (!groupKey && !backspace) return false;
    if (_held[action.KeyChar]) return _consumed[action.KeyChar];
    _held[action.KeyChar] = true;
    _consumed[action.KeyChar] = false;
    const auto modifiers = action.Modifiers;
    if (backspace)
    {
        if (modifiers != base::Action::MODIFIER_NONE) return false;
        if (_marqueeBaseline && !CancelMuteMarquee(stations, publish)) { _consumed[action.KeyChar] = true; return true; }
        const auto selected = PerformanceGroupService::PrepareSelectedTakeInversion(stations, _version);
        auto application = PerformanceGroupService::PrepareRawMuteApplication(stations, _version, _pendingMute.get());
        if (!selected || !application) { _error = "Mute group target limit exceeded"; _consumed[action.KeyChar] = true; return true; }
        if (!selected->Takes.empty())
        {
            std::set<const LoopTake*> selectedTakes;
            for (const auto& state : selected->Takes) selectedTakes.insert(state.Take.get());
            std::map<const LoopTake*, bool> inverted;
            for (auto& state : application->Takes)
                if (selectedTakes.contains(state.Take.get())) { state.Muted = !state.Muted; inverted[state.Take.get()] = state.Muted; }
            for (auto& state : application->Loops)
                if (const auto found = inverted.find(state.Take.get()); found != inverted.end()) state.Muted = found->second;
            _PublishDesired(std::move(application), publish);
        }
    }
    else
    {
        GroupCommand command;
        if (modifiers == base::Action::MODIFIER_NONE) command = GroupCommand::RecallOrStore;
        else if (modifiers == base::Action::MODIFIER_SHIFT) command = GroupCommand::Store;
        else if (modifiers == base::Action::MODIFIER_ALT) command = GroupCommand::Clear;
        else return false;
        const auto slot = action.KeyChar - 111u;
        // Keyboard creation mirrors the visible row plus its next add position.
        // Reject before Execute so a no-op cannot cancel pending work or feedback.
        const bool exists = _groups.Mute.Get(slot) != nullptr;
        if (!exists && (command == GroupCommand::Clear || slot > _groups.Mute.VisibleSlots() + 1u))
        {
            _consumed[action.KeyChar] = true;
            return true;
        }
        Execute(false, command, slot, stations, depth, publish);
    }
    _consumed[action.KeyChar] = true;
    return true;
}

bool PerformanceGroupController::SetAllMuted(bool muted, const PerformanceGroupService::Stations& stations,
    const Publish& publish)
{
    if (_marqueeBaseline && !CancelMuteMarquee(stations, publish)) return false;
    const auto local = PerformanceGroupService::PrepareAllMuted(stations, muted, _version, _pendingMute.get());
    auto application = PerformanceGroupService::PrepareRawMuteApplication(stations, _version, _pendingMute.get());
    if (!local || !application) { _error = "Mute group target limit exceeded"; return false; }
    PerformanceGroupService::RestoreSurvivingRawFlags(*application, *local);
    return _PublishDesired(std::move(application), publish);
}

bool PerformanceGroupController::Execute(bool selection, GroupCommand command, std::size_t slot,
    const PerformanceGroupService::Stations& stations, GroupTargetKind depth, const Publish& publish)
{
    _error.clear();
    if (selection)
        return _groups.Selection.Execute(command, slot,
            [&] { return std::optional<SelectionGroupSnapshot>(PerformanceGroupService::CaptureSelectionConfiguration(stations, depth)); },
            [&](const auto& snapshot) { PerformanceGroupService::RestoreSelection(snapshot, stations); return true; });
    if (command == GroupCommand::Add)
    {
        auto empty = _groups.Mute.LowestEmptySlot();
        if (!empty) return false;
        slot = *empty;
        command = GroupCommand::Store;
    }
    if (slot < 1u || slot > 9u) return false;
    if (command == GroupCommand::Clear) return _groups.Mute.Clear(slot);
    if (_marqueeBaseline && !CancelMuteMarquee(stations, publish)) return false;
    if (command == GroupCommand::RecallOrStore && _groups.Mute.Get(slot))
    {
        const auto& snapshot = *_groups.Mute.Get(slot);
        const auto local = PerformanceGroupService::PrepareMuteApplication(snapshot, stations, _version);
        auto application = PerformanceGroupService::PrepareRawMuteApplication(stations, _version, _pendingMute.get());
        if (!local || !application) { _error = "Mute group target limit exceeded"; return false; }
        PerformanceGroupService::RestoreSurvivingRawFlags(*application, *local);
        if (!_PublishDesired(std::move(application), publish)) return false;
        // Live recall only publishes mute flags. Even in-place plugin state
        // restoration can rebuild vendor caches and exclude audio processing.
        return _groups.Mute.Execute(command, slot, [] { return std::optional<MuteGroupSnapshot>(); }, [](const auto&) { return true; });
    }
    // Vendor state capture excludes processing, even when invoked on the UI
    // thread. Live groups save only mute flags, which is also all recall applies.
    auto snapshot = PerformanceGroupService::CaptureMuteConfiguration(stations, _pendingMute.get());
    if (snapshot.Targets.size() > GroupMuteApplication::MaxTargets)
    {
        _error = "Mute group target limit exceeded";
        return false;
    }
    return _groups.Mute.Store(slot, std::move(snapshot));
}

// All images are prepared on the Scene owner. Only AudioHost applies UI mute flags.
bool PerformanceGroupController::_SameRawState(const GroupMuteApplication& left, const GroupMuteApplication& right)
{
    return left.Stations.size() == right.Stations.size() && left.Takes.size() == right.Takes.size() && left.Loops.size() == right.Loops.size()
        && std::equal(left.Stations.begin(), left.Stations.end(), right.Stations.begin(), [](const auto& a, const auto& b) { return a.Owner == b.Owner && a.Muted == b.Muted; })
        && std::equal(left.Takes.begin(), left.Takes.end(), right.Takes.begin(), [](const auto& a, const auto& b) { return a.Take == b.Take && a.Muted == b.Muted; })
        && std::equal(left.Loops.begin(), left.Loops.end(), right.Loops.begin(), [](const auto& a, const auto& b) { return a.AudioLoop == b.AudioLoop && a.Take == b.Take && a.Muted == b.Muted; });
}

bool PerformanceGroupController::_PublishDesired(std::shared_ptr<GroupMuteApplication> application, const Publish& publish)
{
    if (!application) { _error = "Mute group target limit exceeded"; return false; }
    _error.clear();
    application->Version = ++_version;
    _pendingMute = application;
    if (publish) publish(std::move(application));
    return true;
}

bool PerformanceGroupController::BeginMuteMarquee(const PerformanceGroupService::Stations& stations)
{
    if (_marqueeBaseline) return true;
    _marqueeBaseline = PerformanceGroupService::PrepareRawMuteApplication(stations, _version, _pendingMute.get());
    if (!_marqueeBaseline) _error = "Mute group target limit exceeded";
    return _marqueeBaseline != nullptr;
}

bool PerformanceGroupController::PreviewMuteMarquee(std::span<const GroupTargetId> targets,
    const PerformanceGroupService::Stations& stations, const Publish& publish)
{
    if (!_marqueeBaseline) return false;
    const auto before = PerformanceGroupService::PrepareRawMuteApplication(stations, _version, _pendingMute.get());
    if (!before) return _PublishDesired({}, publish);
    auto application = std::make_shared<GroupMuteApplication>(*before);
    PerformanceGroupService::RestoreSurvivingRawFlags(*application, *_marqueeBaseline);
    for (const auto& target : targets) PerformanceGroupService::SetTargetMuted(*application, stations, target, true);
    if (_SameRawState(*application, *before)) return true;
    return _PublishDesired(std::move(application), publish);
}

bool PerformanceGroupController::CancelMuteMarquee(const PerformanceGroupService::Stations& stations, const Publish& publish)
{
    if (!_marqueeBaseline) return false;
    auto application = PerformanceGroupService::PrepareRawMuteApplication(stations, _version, _pendingMute.get());
    if (!application)
    {
        application = PerformanceGroupService::FilterMuteApplicationToCurrent(*_marqueeBaseline,stations,_version);
        _marqueeBaseline.reset();
        const auto restored = _PublishDesired(std::move(application),publish);
        _error = "Too much material for marquee; previous mute state restored";
        return restored;
    }
    const auto before = *application;
    PerformanceGroupService::RestoreSurvivingRawFlags(*application, *_marqueeBaseline);
    _marqueeBaseline.reset();
    if (_SameRawState(*application,before)) return true;
    return _PublishDesired(std::move(application), publish);
}

bool PerformanceGroupController::SetTargetMuted(const GroupTargetId& target, bool muted,
    const PerformanceGroupService::Stations& stations, const Publish& publish)
{
    if (_marqueeBaseline && !CancelMuteMarquee(stations, publish)) return false;
    auto application = PerformanceGroupService::PrepareRawMuteApplication(stations, _version, _pendingMute.get());
    if (!application) return _PublishDesired({}, publish);
    PerformanceGroupService::SetTargetMuted(*application, stations, target, muted, true);
    return _PublishDesired(std::move(application), publish);
}

bool PerformanceGroupController::DesiredMuted(const GroupTargetId& target, const PerformanceGroupService::Stations& stations) const
{
    return PerformanceGroupService::DesiredMuted(stations, target, _pendingMute.get());
}

void PerformanceGroupController::RepublishForTopology(const PerformanceGroupService::Stations& stations, const Publish& publish)
{
    if (!_pendingMute) return;
    auto application = PerformanceGroupService::PrepareRawMuteApplication(stations,_version,_pendingMute.get());
    if (!application) application = PerformanceGroupService::FilterMuteApplicationToCurrent(*_pendingMute,stations,_version);
    _PublishDesired(std::move(application),publish);
}
