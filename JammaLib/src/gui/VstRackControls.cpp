#include "VstRackControls.h"
#include "../utils/PathUtils.h"
#include <algorithm>
#include <utility>
#include <Windows.h>

using namespace gui;

bool VstRackControls::_AvailableLocked(const Target& target) const
{
	if (!_host.EditsEnabled() || !target.Station || target.Station->IsRemote() ||
		target.Station->MembershipRemoving() ||
		std::find(_host.Stations.begin(), _host.Stations.end(), target.Station) == _host.Stations.end()) return false;
	if (!target.Take) return !target.Loop;
	const auto& takes = target.Station->GetLoopTakes();
	if (std::find(takes.begin(), takes.end(), target.Take) == takes.end()) return false;
	if (!target.Loop) return true;
	const auto& loops = target.Take->GetLoops();
	return std::find(loops.begin(), loops.end(), target.Loop) != loops.end();
}

vst::VstRackSlot VstRackControls::_SlotLocked(const Target& target) const
{
	if (target.Loop) return target.Loop->GetVstRackSlot();
	if (target.Take) return target.Take->GetVstRackSlot();
	return target.Station->GetVstRackSlot();
}

bool VstRackControls::_LoadLocked(const Target& target, const std::wstring& path)
{
	if (target.Loop) return target.Loop->LoadManagedVstPlugin(path);
	if (target.Take) return target.Take->LoadManagedVstPlugin(path);
	return target.Station->LoadManagedVstPlugin(path);
}

bool VstRackControls::_UnloadLocked(const Target& target, const std::string& identity)
{
	if (target.Loop) return target.Loop->UnloadManagedVstPlugin(identity);
	if (target.Take) return target.Take->UnloadManagedVstPlugin(identity);
	return target.Station->UnloadManagedVstPlugin(identity);
}

void VstRackControls::_PresentLocked(const std::shared_ptr<GuiRack>& rack, Target target, const std::string& label)
{
	if (!rack) return;
	const bool available = _AvailableLocked(target);
	const auto slot = _SlotLocked(target);
	const auto state = slot.Pending ? GuiRack::VstControlState::Loading :
		slot.Failed ? GuiRack::VstControlState::Failed :
		slot.Plugin ? (_host.Editors.IsVstEditorOpen(slot.Plugin) ? GuiRack::VstControlState::Open : GuiRack::VstControlState::Closed) :
		GuiRack::VstControlState::Empty;
	rack->SetVstControlState(state, available && !_busy);
	rack->SetVstControlText(slot.Pending ? "VST..." : label);
	// The hierarchy owns its racks. Capturing strong owners here would form a
	// cycle; pin them only when a gesture becomes an intent.
	rack->SetVstControlCallback([this, lifetime = std::weak_ptr<int>(_lifetime),
		station = std::weak_ptr<engine::Station>(target.Station),
		take = std::weak_ptr<engine::LoopTake>(target.Take),
		loop = std::weak_ptr<engine::Loop>(target.Loop),
		hasTake = bool(target.Take), hasLoop = bool(target.Loop)](base::Action::Modifiers modifiers) {
		if (lifetime.expired()) return;
		Target owner{station.lock(), take.lock(), loop.lock()};
		if (!owner.Station || (hasTake && !owner.Take) || (hasLoop && !owner.Loop)) return;
		if (!_busy && !_intent) _intent = Intent{std::move(owner), modifiers};
	});
}

void VstRackControls::UpdateLocked(base::SelectDepth depth)
{
	for (const auto& station : _host.Stations)
	{
		_PresentLocked(station->GetGuiRack(), {station, {}, {}}, "VST");
		for (const auto& take : station->GetLoopTakes())
		{
			auto rack = take->GetGuiRack();
			if (depth != base::DEPTH_LOOP)
			{
				_PresentLocked(rack, {station, take, {}}, "VST");
				continue;
			}
			std::shared_ptr<engine::Loop> selected;
			size_t index = 0, selectedIndex = 0, count = 0;
			for (const auto& loop : take->GetLoops())
			{
				if (loop->IsSelected()) { selected = loop; selectedIndex = index; ++count; }
				++index;
			}
			if (count == 1)
				_PresentLocked(rack, {station, take, selected}, "VST L" + std::to_string(selectedIndex + 1));
			else if (rack)
			{
				rack->SetVstControlState(GuiRack::VstControlState::Empty, false);
				rack->SetVstControlText("VST L?");
				rack->SetVstControlCallback({});
			}
		}
	}
}

void VstRackControls::Pump()
{
	if (_busy) return;
	_busy = true;
	struct BusyRelease { bool& Busy; ~BusyRelease() { Busy = false; } } release{_busy};
	std::wstring failure;
	{
		std::scoped_lock lock(_host.Mutex);
		std::erase_if(_loads, [&](const Target& target) {
			if (!_AvailableLocked(target)) return true;
			const auto completed = _SlotLocked(target);
			if (completed.Pending) return false;
			if (completed.Failed) failure = L"Could not load VST:\n" + completed.Path +
				L"\n\nCheck that the file is an installed 64-bit VST2 or VST3 plugin compatible with this item's channels.\nClick the red VST button to choose a replacement.";
			return true;
		});
	}
	if (!failure.empty())
	{
		if (_host.ShowError) _host.ShowError(failure);
		else MessageBoxW(GetActiveWindow(), failure.c_str(), L"VST load failed", MB_OK | MB_ICONWARNING);
	}
	if (!_intent) return;
	const auto intent = std::exchange(_intent, std::nullopt).value();
	vst::VstRackSlot slot;
	{
		std::scoped_lock lock(_host.Mutex);
		if (!_AvailableLocked(intent.Owner)) return;
		slot = _SlotLocked(intent.Owner);
		if (slot.Pending) return;
	}
	const bool remove = (intent.Modifiers & base::Action::MODIFIER_ALT) != 0;
	if (remove)
	{
		if (slot.Identity.empty()) return;
		const bool confirmed = _host.ConfirmRemoval ? _host.ConfirmRemoval() :
			MessageBoxW(GetActiveWindow(), L"Remove the first VST from this item?\nThe other plugins stay in their chain.",
				L"Remove VST", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
		if (!confirmed) return;
		// The confirmation can pump messages. Close only the captured processor,
		// then validate membership and identity again before enqueueing removal.
		_host.Editors.CloseVstEditorForPlugin(slot.Plugin);
		bool changed = false;
		{
			std::scoped_lock lock(_host.Mutex);
			if (_AvailableLocked(intent.Owner)) changed = _UnloadLocked(intent.Owner, slot.Identity);
		}
		if (changed) _host.Commit();
		return;
	}
	if (slot.Plugin && !slot.Failed)
	{
		if (!_host.Editors.ToggleVstEditorForPlugin(slot.Plugin))
		{
			const std::wstring error = L"The plugin is loaded, but its editor could not be opened.\nAudio processing is unchanged. You can try again or Alt-click to remove it.";
			if (_host.ShowError) _host.ShowError(error);
			else MessageBoxW(GetActiveWindow(), error.c_str(), L"VST editor", MB_OK | MB_ICONINFORMATION);
		}
		return;
	}
	const auto path = _host.PickPlugin ? _host.PickPlugin() : utils::PickFile(
		slot.Failed ? L"Replace failed VST plugin" : L"Choose VST plugin");
	if (path.empty()) return;
	bool changed = false;
	{
		std::scoped_lock lock(_host.Mutex);
		if (_AvailableLocked(intent.Owner))
		{
			const auto current = _SlotLocked(intent.Owner);
			if (current.Identity == slot.Identity && !current.Pending)
				changed = _LoadLocked(intent.Owner, path);
		}
	}
	if (changed)
	{
		_loads.push_back(intent.Owner);
		_host.Commit();
	}
}

void VstRackControls::Reset()
{
	_intent.reset();
	_loads.clear();
}
