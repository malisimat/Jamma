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
	if (!target.PluginId.empty())
		for (const auto& slot : target.Station->GetVstRackSlots()) if (slot.Identity == target.PluginId) return slot;
	if (!target.PluginId.empty()) return {};
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
	rack->SetVstControlText(slot.Failed ? "Retry " + label : slot.Identity.empty() ? "Add " + label : label);
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
		_PresentStationLocked(station);
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
	if (intent.RackCommand) { _PumpStation(intent); return; }
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
			MessageBoxW(GetActiveWindow(), L"Remove this VST slot?\nThe other plugins stay in their chain.",
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
		bool stillOwned = false;
		{
			std::scoped_lock lock(_host.Mutex);
			stillOwned = _AvailableLocked(intent.Owner) && _SlotLocked(intent.Owner).Plugin == slot.Plugin;
		}
		if (!stillOwned) _host.Editors.CloseVstEditorForPlugin(slot.Plugin);
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

void VstRackControls::_PresentStationLocked(const std::shared_ptr<engine::Station>& station)
{
	const auto rack = station->GetGuiRack();
	const auto control = rack ? rack->VstRack() : nullptr;
	if (!control) return;
	GuiVstRack::Model model;
	model.TotalSlots = station->RackSlotCount(); model.Revision = station->RackRevision();
	model.Editable = _AvailableLocked({station, {}, {}}) && !_busy;
	model.Inactive = station->RackInactive(); model.Suspended = station->RackLatencySuspended();
	model.Legacy = station->RackRenderPolicy() == vst::VstChain::RenderPolicy::LegacySerial;
	model.ConversionAvailable = model.Legacy && station->CanConvertRackToAdditive();
	model.Diagnostic = station->RackDiagnostic();
	if (!model.Inactive) model.ChannelRoutes = station->LiveVstChannelRoutes();
	for (auto slot : station->GetVstRackSlots(control->PageOffset()))
	{
		const bool open = slot.Plugin && _host.Editors.IsVstEditorOpen(slot.Plugin);
		model.Slots.push_back({std::move(slot), open});
	}
	control->Update(std::move(model), [this, life = std::weak_ptr<int>(_lifetime), weak = std::weak_ptr<engine::Station>(station)](GuiVstRack::Command command) {
		if (life.expired() || _busy || _intent) return;
		if (auto owner = weak.lock())
		{
			Target target{std::move(owner), {}, {}, command.Identity};
			_intent = Intent{std::move(target), command.Modifiers, std::move(command)};
		}
	});
}

void VstRackControls::_PumpStation(const Intent& intent)
{
	const auto command = *intent.RackCommand;
	const auto station = intent.Owner.Station;
	const auto rack = station->GetGuiRack()->VstRack();
	using Kind = GuiVstRack::Command::Kind;
	const auto report = [&](std::string message) { if (rack) rack->SetFeedback(std::move(message)); };
	const auto valid = [&] { return _AvailableLocked(intent.Owner) && station->RackRevision() == command.Revision; };
	const auto show = [&](const std::string& message) {
		const auto text = utils::DecodeUtf8(message);
		if (_host.ShowError) _host.ShowError(text);
		else MessageBoxW(GetActiveWindow(), text.c_str(), L"Station rack", MB_OK | MB_ICONINFORMATION);
	};
	vst::VstRackSlot slot;
	bool inactive = false;
	bool serial = false;
	{
		std::scoped_lock lock(_host.Mutex);
		if (!valid()) { report("Rack changed; retry the gesture."); return; }
		inactive = station->RackInactive();
		serial = station->RackRenderPolicy() == vst::VstChain::RenderPolicy::LegacySerial;
		if (command.Operation == Kind::Slot && !station->RackInactive())
		{
			slot = _SlotLocked(intent.Owner);
			if (slot.Identity != command.Identity) { report("This slot is no longer available."); return; }
		}
	}
	if (command.Operation == Kind::Diagnostic)
	{
		std::string message;
		{ std::scoped_lock lock(_host.Mutex); message = station->RackDiagnostic(); }
		if (message.empty() && rack) message = rack->Feedback();
		if (!message.empty()) show(message);
		return;
	}
	if (command.Operation == Kind::ModeInfo)
	{
		show(serial ?
			"Serial chain: each VST processes the previous VST's output. This keeps older JAMs sounding as saved. "
			"Conversion is available when loaded plugins have known roles and rack latency fits. "
			"Converting can change the sound and cannot be undone here." :
			"Additive mix: instrument outputs are summed, while effects process the mix according to their order.");
		return;
	}
	if (command.Operation == Kind::Append && (command.Modifiers & base::Action::MODIFIER_ALT)) return;
	if (command.Operation == Kind::Convert || command.Operation == Kind::ClearInactiveRoutes)
	{
		const auto text = command.Operation == Kind::Convert ?
			L"Convert this serial chain to an additive mix?\n\nInstrument A, Instrument B, Reverb: Reverb(A + B).\nInstrument A, Reverb, Instrument B: Reverb(A) + B.\n\nThis can change the sound. Plugin state and order are preserved." :
			L"Clear every saved live MIDI cable in this inactive rack?\n\nChannels return to Broadcast. Plugin entries, state and recorded MIDI routes are preserved.";
		if (MessageBoxW(GetActiveWindow(), text, L"Station rack", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
	}
	if (command.Operation == Kind::Routes || command.Operation == Kind::Convert ||
		command.Operation == Kind::Recover || command.Operation == Kind::ClearInactiveRoutes)
	{
		bool changed = false; std::string failure;
		{
			std::scoped_lock lock(_host.Mutex);
			if (!valid()) { report("Rack changed; retry the gesture."); return; }
			switch (command.Operation)
			{
			case Kind::Routes: changed = station->SetLiveVstChannelRoutes(command.ChannelRoutes, command.Revision); break;
			case Kind::Convert: changed = station->ConvertRackToAdditive(command.Revision); break;
			case Kind::Recover: changed = station->RecoverRack(command.Revision); break;
			case Kind::ClearInactiveRoutes: changed = station->ClearInactiveRackRoutes(command.Revision); break;
			default: break;
			}
			if (!changed) failure = station->RackDiagnostic();
		}
		if (changed) _host.Commit();
		else report(failure.empty() ? "Rack edit rejected; check plugin roles and capacity." : failure);
		return;
	}
	if (command.Operation == Kind::Slot && (command.Modifiers & base::Action::MODIFIER_ALT))
	{
		const bool confirmed = _host.ConfirmRemoval ? _host.ConfirmRemoval() :
			MessageBoxW(GetActiveWindow(), L"Remove this VST slot?\nThe other plugins stay in their chain.",
				L"Remove VST", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
		if (!confirmed) return;
		_host.Editors.CloseVstEditorForPlugin(slot.Plugin);
		bool changed = false;
		{
			std::scoped_lock lock(_host.Mutex);
			if (valid()) changed = station->RackInactive() ? station->RemoveInactiveRackSlot(command.Ordinal, command.Revision) :
				station->UnloadManagedVstPlugin(command.Identity);
		}
		if (changed) _host.Commit(); else report("Rack changed; retry removal.");
		return;
	}
	if (command.Operation == Kind::Slot && (slot.Pending || inactive)) return;
	if (slot.Plugin && !slot.Failed)
	{
		const bool opened = _host.Editors.ToggleVstEditorForPlugin(slot.Plugin);
		bool stillOwned = false;
		{
			std::scoped_lock lock(_host.Mutex);
			stillOwned = valid() && _SlotLocked(intent.Owner).Plugin == slot.Plugin;
		}
		if (!stillOwned) _host.Editors.CloseVstEditorForPlugin(slot.Plugin);
		else if (!opened) report("The plugin editor could not be opened; click Rack info for details.");
		return;
	}
	const auto path = _host.PickPlugin ? _host.PickPlugin() : utils::PickFile(slot.Failed ? L"Retry saved VST slot" : L"Append VST plugin");
	if (path.empty()) return;
	bool changed = false; std::string failure;
	{
		std::scoped_lock lock(_host.Mutex);
		if (!valid()) { report("Rack changed while choosing a plugin; retry."); return; }
		changed = station->LoadManagedVstSlot(path, command.Operation == Kind::Slot ? command.Identity : std::string{});
		if (!changed) failure = station->RackDiagnostic();
		else
		{
			auto target = intent.Owner;
			if (command.Operation == Kind::Append)
			{
				const auto slots = station->GetVstRackSlots();
				if (!slots.empty()) target.PluginId = slots.back().Identity;
			}
			_loads.push_back(std::move(target));
		}
	}
	if (changed) _host.Commit(); else report(failure.empty() ? "Rack is full or still retiring a removed plugin; retry shortly." : failure);
}
