#include "VstRackControls.h"
#include "../utils/PathUtils.h"
#include <algorithm>
#include <utility>

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
		if (!_busy && !_intent && !_confirmation) _intent = Intent{std::move(owner), modifiers};
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
			const auto rolledBack = !target.Take && !target.Loop && !target.PluginId.empty()
				? target.Station->TakeFailedInteractiveVstLoad(target.PluginId) : std::nullopt;
			if (rolledBack || completed.Failed)
				failure = L"Could not load VST:\n" + (rolledBack ? *rolledBack : completed.Path) +
					L"\n\nCheck that the file is an installed 64-bit VST2 or VST3 plugin compatible with this item's channels." +
					(rolledBack ? L"\nThe rack has been restored." : L"\nClick the red VST button to choose a replacement.");
			return true;
		});
	}
	if (!failure.empty())
	{
		_ShowError(failure);
	}
	if (_confirmation) { _CompleteConfirmation(); return; }
	if (!_intent) return;
	const auto intent = std::exchange(_intent, std::nullopt).value();
	if (intent.RackCommand) { _PumpStation(intent); return; }
	vst::VstRackSlot slot;
	std::uint64_t rackRevision = 0u;
	{
		std::scoped_lock lock(_host.Mutex);
		if (!_AvailableLocked(intent.Owner)) return;
		slot = _SlotLocked(intent.Owner);
		rackRevision = intent.Owner.Station->RackRevision();
		if (slot.Pending) return;
	}
	const bool remove = (intent.Modifiers & base::Action::MODIFIER_ALT) != 0;
	if (remove)
	{
		if (slot.Identity.empty()) return;
		_RequestConfirmation(intent,slot,rackRevision,"Remove VST",
			"Remove this VST slot? The other plugins stay in their chain.","Remove VST");
		return;
	}
	if (slot.Plugin && !slot.Failed)
	{
		if (!_host.Editors.ToggleVstEditorForPlugin(slot.Plugin))
		{
			const std::wstring error = L"The plugin is loaded, but its editor could not be opened.\nAudio processing is unchanged. You can try again or Alt-click to remove it.";
			_ShowError(error);
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
	if (_confirmation) { *_confirmation->Decision = false; _confirmation.reset(); }
	_intent.reset();
	_loads.clear();
}

void VstRackControls::_ShowError(const std::wstring& message) const
{
	if (_host.ShowError) _host.ShowError(message);
	else std::cerr << "[VST] " << utils::EncodeUtf8(message) << std::endl;
}

void VstRackControls::_RequestConfirmation(const Intent& intent, vst::VstRackSlot slot,
	std::uint64_t revision, std::string title, std::string message, std::string accept)
{
	if (!_host.Confirm) { _ShowError(utils::DecodeUtf8(message)); return; }
	auto decision = _host.Confirm(std::move(title),std::move(message),std::move(accept));
	if (decision) _confirmation = Confirmation{intent,std::move(slot),revision,std::move(decision)};
}

void VstRackControls::_CompleteConfirmation()
{
	if (!_confirmation->Decision->has_value()) return;
	auto confirmation = std::move(*_confirmation); _confirmation.reset();
	if (!confirmation.Decision->value()) return;
	const auto& intent = confirmation.Captured;
	const auto& station = intent.Owner.Station;
	const bool clear = intent.RackCommand && intent.RackCommand->Operation == GuiVstRack::Command::Kind::ClearInactiveRoutes;
	const auto valid = [&] {
		if (!_AvailableLocked(intent.Owner) || station->RackRevision() != confirmation.RackRevision) return false;
		if (clear) return station->RackInactive();
		if (intent.RackCommand && station->RackInactive()) return true; // Ordinal is protected by rack revision.
		const auto current = _SlotLocked(intent.Owner);
		return !current.Pending && current.Identity == confirmation.Slot.Identity && current.Plugin == confirmation.Slot.Plugin;
	};
	{
		std::scoped_lock lock(_host.Mutex);
		if (!valid()) return;
	}
	// Native editor teardown can pump messages. Keep it on UI and outside Scene's
	// lock, then validate the captured identity/revision again before mutation.
	if (!clear) _host.Editors.CloseVstEditorForPlugin(confirmation.Slot.Plugin);
	bool changed = false;
	{
		std::scoped_lock lock(_host.Mutex);
		if (!valid()) return;
		if (clear) changed = station->ClearInactiveRackRoutes(confirmation.RackRevision);
		else if (intent.RackCommand && station->RackInactive())
			changed = station->RemoveInactiveRackSlot(intent.RackCommand->Ordinal,confirmation.RackRevision);
		else changed = _UnloadLocked(intent.Owner,confirmation.Slot.Identity);
	}
	if (changed) _host.Commit();
}

void VstRackControls::_PresentStationLocked(const std::shared_ptr<engine::Station>& station)
{
	const auto rack = station->GetGuiRack();
	const auto control = rack ? rack->VstRack() : nullptr;
	if (!control) return;
	GuiVstRack::Model model;
	model.OwnerName = station->Name();
	model.TotalSlots = station->RackSlotCount(); model.Revision = station->RackRevision();
	model.Editable = _AvailableLocked({station, {}, {}}) && !_busy;
	model.Inactive = station->RackInactive(); model.Suspended = station->RackLatencySuspended();
	model.Diagnostic = station->RackDiagnostic();
	if (!model.Inactive) model.ChannelRoutes = station->LiveVstChannelRoutes();
	for (auto slot : station->GetVstRackSlots(control->PageOffset()))
	{
		const bool open = slot.Plugin && _host.Editors.IsVstEditorOpen(slot.Plugin);
		model.Slots.push_back({std::move(slot), open});
	}
	control->Update(std::move(model), [this, life = std::weak_ptr<int>(_lifetime), weak = std::weak_ptr<engine::Station>(station)](GuiVstRack::Command command) {
		if (life.expired() || _busy || _intent || _confirmation) return;
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
	const auto report = [&](const std::string& message) { if (rack) rack->SetFeedback(message); _ShowError(utils::DecodeUtf8(message)); };
	const auto valid = [&] { return _AvailableLocked(intent.Owner) && station->RackRevision() == command.Revision; };
	const auto show = [&](const std::string& message) {
		const auto text = utils::DecodeUtf8(message);
		_ShowError(text);
	};
	vst::VstRackSlot slot;
	bool inactive = false;
	{
		std::scoped_lock lock(_host.Mutex);
		if (!valid()) { report("Rack changed; retry the gesture."); return; }
		inactive = station->RackInactive();
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
	if (command.Operation == Kind::Append && (command.Modifiers & base::Action::MODIFIER_ALT)) return;
	if (command.Operation == Kind::ClearInactiveRoutes)
	{
		_RequestConfirmation(intent,slot,command.Revision,"Station rack",
			"Clear saved live MIDI cables? Channels return to Broadcast. Plugin entries, state and recorded MIDI routes are preserved.","Clear MIDI");
		return;
	}
	if (command.Operation == Kind::Routes ||
		command.Operation == Kind::Recover || command.Operation == Kind::ClearInactiveRoutes)
	{
		bool changed = false; std::string failure;
		{
			std::scoped_lock lock(_host.Mutex);
			if (!valid()) { report("Rack changed; retry the gesture."); return; }
			switch (command.Operation)
			{
			case Kind::Routes: changed = station->SetLiveVstChannelRoutes(command.ChannelRoutes, command.Revision); break;
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
		_RequestConfirmation(intent,slot,command.Revision,"Remove VST",
			"Remove this VST slot? The other plugins stay in their chain.","Remove VST");
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
		changed = station->LoadManagedVstSlot(path, command.Operation == Kind::Slot ? command.Identity : std::string{},
			command.Operation == Kind::Append);
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
