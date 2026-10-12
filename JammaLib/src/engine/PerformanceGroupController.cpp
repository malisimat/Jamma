#include "PerformanceGroupController.h"
#include "Station.h"

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
    const bool number = action.KeyChar >= 49u && action.KeyChar <= 57u;
    const bool backspace = action.KeyChar == 8u;
    if (!number && !backspace) return false;
    if (_held[action.KeyChar]) return _consumed[action.KeyChar];
    _held[action.KeyChar] = true;
    const auto modifiers = action.Modifiers;
    if (backspace)
    {
        if (modifiers != base::Action::MODIFIER_NONE) return false;
        auto application = PerformanceGroupService::PrepareSelectedTakeInversion(stations, ++_version);
        if (!application) { _error = "Mute group target limit exceeded"; _consumed[action.KeyChar] = true; return true; }
        if (!application->Takes.empty())
        {
            _capture.reset(); _recall.reset();
            _error.clear();
            // A second physical press can precede the audio receipt. Invert
            // the latest desired flag in that case, never the stale live flag.
            auto revised = std::make_shared<GroupMuteApplication>(*application);
            if (_pendingMute)
                for (auto& state : revised->Takes)
                    for (const auto& pending : _pendingMute->Takes)
                        if (pending.Take == state.Take)
                        {
                            state.Muted = !pending.Muted;
                            for (auto& leaf : revised->Loops)
                                for (const auto& loop : state.Take->GetLoops())
                                    if (leaf.AudioLoop == loop) leaf.Muted = state.Muted;
                            break;
                        }
            _pendingMute = revised;
            publish(revised);
        }
    }
    else
    {
        GroupCommand command;
        if (modifiers == base::Action::MODIFIER_NONE) command = GroupCommand::RecallOrStore;
        else if (modifiers == base::Action::MODIFIER_SHIFT) command = GroupCommand::Store;
        else if (modifiers == base::Action::MODIFIER_ALT) command = GroupCommand::Clear;
        else return false;
        Execute(false, command, action.KeyChar - 48u, stations, depth, publish);
    }
    _consumed[action.KeyChar] = true;
    return true;
}

bool PerformanceGroupController::CapturePlugins(MuteGroupSnapshot& snapshot,
    const PerformanceGroupService::Stations& stations)
{
    for (const auto& station : stations)
    {
        if (station->IsRemote()) continue;
        std::vector<io::JamFile::VstEntry> entries;
        if (!station->TryGroupVstEntries(entries)) return false;
        for (const auto& entry : entries)
            snapshot.Plugins.push_back({ { GroupTargetKind::Station, station->Id(), {}, {} }, entry.Id, entry.Path, entry.State, entry.Bypass });
        for (const auto& take : station->GetLoopTakes())
        {
            entries.clear();
            if (!take->TryGroupVstEntries(entries)) return false;
            for (const auto& entry : entries)
                snapshot.Plugins.push_back({ { GroupTargetKind::Take, station->Id(), take->Id(), {} }, entry.Id, entry.Path, entry.State, entry.Bypass });
        }
    }
    return true;
}

bool PerformanceGroupController::Execute(bool selection, GroupCommand command, std::size_t slot,
    const PerformanceGroupService::Stations& stations, GroupTargetKind depth, const Publish& publish)
{
    _error.clear();
    if (selection)
        return _groups.Selection.Execute(command, slot,
            [&] { return std::optional<SelectionGroupSnapshot>(PerformanceGroupService::CaptureSelectionConfiguration(stations, depth)); },
            [&](const auto& snapshot) { PerformanceGroupService::RestoreSelection(snapshot, stations); return true; });
    _capture.reset(); _recall.reset();
    if (command == GroupCommand::Add)
    {
        auto empty = _groups.Mute.LowestEmptySlot();
        if (!empty) return false;
        slot = *empty;
        command = GroupCommand::Store;
    }
    if (slot < 1u || slot > 9u) return false;
    if (command == GroupCommand::Clear) return _groups.Mute.Clear(slot);
    if (command == GroupCommand::RecallOrStore && _groups.Mute.Get(slot))
    {
        const auto& snapshot = *_groups.Mute.Get(slot);
        auto application = PerformanceGroupService::PrepareMuteApplication(snapshot, stations, ++_version);
        if (!application) { _error = "Mute group target limit exceeded"; return false; }
        _pendingMute = application;
        publish(std::move(application));
        _recall = PendingRecall{ snapshot.Plugins };
        return _groups.Mute.Execute(command, slot, [] { return std::optional<MuteGroupSnapshot>(); }, [](const auto&) { return true; });
    }
    _capture = PendingCapture{ slot };
    Pump(stations, publish);
    return true;
}

void PerformanceGroupController::Pump(const PerformanceGroupService::Stations& stations, const Publish& publish)
{
    if (_capture)
    {
        auto snapshot = PerformanceGroupService::CaptureMuteConfiguration(stations);
        if (snapshot.Targets.size() > GroupMuteApplication::MaxTargets)
        {
            _error = "Mute group target limit exceeded";
            _capture.reset();
            return;
        }
        if (CapturePlugins(snapshot, stations))
        {
            _groups.Mute.Store(_capture->Slot, std::move(snapshot));
            _capture.reset();
        }
        else if (++_capture->Attempts >= 120u)
        {
            _error = "Unable to capture plugin state for mute group";
            _capture.reset();
        }
    }
    if (_recall)
    {
        while (_recall->Index < _recall->Plugins.size())
        {
            const auto& saved = _recall->Plugins[_recall->Index];
            io::JamFile::VstEntry entry;
            entry.Id = saved.PluginId; entry.Path = saved.Path; entry.State = saved.State; entry.Bypass = saved.Bypass;
            bool done = true;
            if (saved.Owner.Kind == GroupTargetKind::Station)
            {
                if (auto station = PerformanceGroupService::ResolveStation(saved.Owner, stations)) done = station->TryRestoreGroupVstEntry(entry);
            }
            else if (auto take = PerformanceGroupService::ResolveTake(saved.Owner, stations)) done = take->TryRestoreGroupVstEntry(entry);
            if (!done)
            {
                if (++_recall->Attempts >= 120u) { _error = "Unable to restore plugin state for mute group"; _recall.reset(); }
                return;
            }
            ++_recall->Index;
            _recall->Attempts = 0u;
        }
        _recall.reset();
    }
}
