#include "SceneSettingsControls.h"
#include "../io/RigFile.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "GuiStackPanel.h"

using namespace gui;
using actions::GuiAction;

GuiRadioParams GuiMidiChannelOverride::_Params(std::uint8_t channel)
{
	GuiRadioParams params;
	params.Size = { 448u, 38u };
	params.InitValue = channel == 0u ? UnchangedIndex : std::min<unsigned int>(channel, 16u) - 1u;
	for (unsigned int index = 0u; index < 16u; ++index)
	{
		auto toggle = GuiToggleParams::PanelPrimary();
		toggle.Text = std::to_string(index + 1u);
		toggle.TextPadding = 2u;
		toggle.TextFontSize = graphics::FontOptions::FONT_SMALL;
		toggle.Position = { static_cast<int>(index * 28u), 0 };
		toggle.Size = { 28u, 38u };
		toggle.MinSize = toggle.Size;
		params.ToggleParams.push_back(toggle);
	}
	return params;
}

GuiMidiChannelOverride::GuiMidiChannelOverride(std::uint8_t channel) : GuiRadio(_Params(channel)) {}

std::uint8_t GuiMidiChannelOverride::Channel() const
{
	return CurrentValue() < UnchangedIndex ? static_cast<std::uint8_t>(CurrentValue() + 1u) : 0u;
}

void GuiMidiChannelOverride::SetChannel(std::uint8_t channel)
{
	SetCurrentValue(channel == 0u ? UnchangedIndex : std::min<unsigned int>(channel, 16u) - 1u, true);
}

actions::ActionResult GuiMidiChannelOverride::OnAction(GuiAction action)
{
	if (action.ElementType != GuiAction::ACTIONELEMENT_TOGGLE || action.Index >= UnchangedIndex)
		return actions::ActionResult::NoAction();
	SetCurrentValue(CurrentValue() == action.Index ? UnchangedIndex : action.Index, true);
	if (_receiver)
	{
		GuiAction change;
		change.ElementType = GuiAction::ACTIONELEMENT_RADIO;
		change.Index = _index;
		change.Data = GuiAction::GuiInt(Channel());
		_receiver->OnAction(change);
	}
	return { true, std::to_string(_index), "", actions::ACTIONRESULT_RADIO, nullptr,
		std::static_pointer_cast<base::GuiElement>(shared_from_this()) };
}

SceneSettingsControls::SceneSettingsControls(std::uint8_t channel, double offset,
	io::JamFile::GlobalMidiQuantState quantisation)
{
	GuiRadioParams modeRadioParams;
	modeRadioParams.Index = 100u;
	modeRadioParams.Size = { 384, 64 };
	std::vector<GuiToggleParams> radioToggleParams;

	for (auto i = 0u; i < 3; i++)
	{
		GuiToggleParams toggleParams;
		toggleParams.Position = { (int)i * 128, 0 };
		toggleParams.Size = { 128, 64 };

		switch (i)
		{
		case 0:
			toggleParams.Texture = "stationmode";
			toggleParams.OverTexture = "stationmode_over";
			toggleParams.DownTexture = "stationmode_down";
			toggleParams.ToggledTexture = "stationmode_toggled";
			toggleParams.ToggledOverTexture = "stationmode_toggled_over";
			toggleParams.ToggledDownTexture = "stationmode_toggled_down";
			break;
		case 1:
			toggleParams.Texture = "takemode";
			toggleParams.OverTexture = "takemode_over";
			toggleParams.DownTexture = "takemode_down";
			toggleParams.ToggledTexture = "takemode_toggled";
			toggleParams.ToggledOverTexture = "takemode_toggled_over";
			toggleParams.ToggledDownTexture = "takemode_toggled_down";
			break;
		case 2:
			toggleParams.Texture = "loopmode";
			toggleParams.OverTexture = "loopmode_over";
			toggleParams.DownTexture = "loopmode_down";
			toggleParams.ToggledTexture = "loopmode_toggled";
			toggleParams.ToggledOverTexture = "loopmode_toggled_over";
			toggleParams.ToggledDownTexture = "loopmode_toggled_down";
			break;
		}

		radioToggleParams.push_back(toggleParams);
	}

	modeRadioParams.ToggleParams = radioToggleParams;
	_modeRadio = std::make_shared<GuiRadio>(modeRadioParams);

	_midiChannelOverride = std::make_shared<GuiMidiChannelOverride>(channel);


	GuiNumericInputParams transportOffsetParams = GuiNumericInputParams::PanelInput(88u);
	transportOffsetParams.Index = TransportOffsetControlIndex;
	transportOffsetParams.Size = { 96, GuiNumericInputParams::DefaultHeight };
	transportOffsetParams.Min = -1.0;
	transportOffsetParams.Max = 1.0;
	transportOffsetParams.Step = 0.005;
	transportOffsetParams.Decimals = 3;
	transportOffsetParams.InitValue = offset;
	_transportOffsetInput = std::make_shared<GuiNumericInput>(transportOffsetParams);


	GuiToggleParams metronomeToggleParams = GuiToggleParams::PanelPrimary();
	metronomeToggleParams.Index = NinjamMetronomeControlIndex;
	metronomeToggleParams.ToggleIndex = NinjamMetronomeControlIndex;
	metronomeToggleParams.Text = "Metronome";
	metronomeToggleParams.Size = { 120, 40u };
	metronomeToggleParams.MinSize = { 120, 40u };
	metronomeToggleParams.TextFontSize = graphics::FontOptions::FONT_SMALL;
	metronomeToggleParams.InitState = GuiToggleParams::TOGGLE_ON;
	_ninjamMetronomeToggle = std::make_shared<GuiToggle>(metronomeToggleParams);


	GuiRadioParams globalMidiQuantRadioParams;
	globalMidiQuantRadioParams.Index = 101u;
	globalMidiQuantRadioParams.InitValue = static_cast<unsigned int>(quantisation);
	globalMidiQuantRadioParams.Size = { 228, 40 };

	std::vector<GuiToggleParams> globalQuantToggleParams;
	for (auto i = 0u; i < 3; ++i)
	{
		GuiToggleParams toggleParams;
		toggleParams.Text = i == 0 ? "Off" : i == 1 ? "Mixed" : "All";
		toggleParams.TextureShader = "texture_tinted";
		toggleParams.Position = { static_cast<int>(i * 76), 0 };
		toggleParams.Size = { 72, 40 };
		toggleParams.Texture = "rounded_but";
		toggleParams.OverTexture = "rounded_but_over";
		toggleParams.DownTexture = "rounded_but_down";
		toggleParams.ToggledTexture = "rounded_but_on";
		toggleParams.ToggledOverTexture = "rounded_but_on_over";
		toggleParams.ToggledDownTexture = "rounded_but_on_down";
		switch (i)
		{
		case 0:
			toggleParams.TintColor = glm::vec3(1.0f, 0.34f, 0.30f);
			break;
		case 1:
			toggleParams.TintColor = glm::vec3(1.0f, 0.75f, 0.20f);
			break;
		case 2:
		default:
			toggleParams.TintColor = glm::vec3(0.33f, 0.92f, 0.45f);
			break;
		}

		toggleParams.TextFontSize = graphics::FontOptions::FONT_SMALL;
		globalQuantToggleParams.push_back(toggleParams);
	}
	globalMidiQuantRadioParams.ToggleParams = globalQuantToggleParams;
	_globalMidiQuantRadio = std::make_shared<GuiRadio>(globalMidiQuantRadioParams);

	auto tapParams = GuiButtonParams::PanelButton(180u);
	tapParams.Index = TapTempoControlIndex;
	tapParams.Text = "Tap tempo (Space)";
	tapParams.Size.Height = 40u; tapParams.MinSize.Height = 40u;
	tapParams.TextFontSize = graphics::FontOptions::FONT_SMALL;
	_tapButton = std::make_shared<GuiButton>(tapParams);
	GuiRadioParams subdivisionParams;
	subdivisionParams.Index = MidiSubdivisionControlIndex;
	subdivisionParams.InitValue = midi::MidiQuantisation::FractionDisplayIndex(midi::MidiQuantisationFraction::Quarter);
	subdivisionParams.Size = { 450u, 40u };
	for (int index = 0; index < midi::MidiQuantisationFractionCount; ++index)
	{
		auto toggle = GuiToggleParams::PanelPrimary();
		toggle.Text = midi::MidiQuantisation::FractionLabel(midi::MidiQuantisation::ClampFractionDisplayIndex(index));
		toggle.Position = { index * 50, 0 };
		toggle.Size = { 48u, 40u };
		toggle.TextFontSize = graphics::FontOptions::FONT_SMALL;
		subdivisionParams.ToggleParams.push_back(toggle);
	}
	_midiSubdivisionRadio = std::make_shared<GuiRadio>(subdivisionParams);

	_deviceStatus = _StatusLabel("Ready", 66u);
	_activeAudioStatus = _StatusLabel("Audio stopped", 44u);
	_fileStatus = _StatusLabel("Choose a folder on first Save", 44u);
	_saveButton = _SessionButton("Save");
	_saveDefaultJamButton = _SessionButton("Set as Default");
	_loadButton = _SessionButton("Load");
	_saveAsJamButton = _SessionButton("Save As...");

}

std::vector<GuiSettingEntry> SceneSettingsControls::SettingsEntries() const
{
	return {
		{ SettingsPage::Timing, "Global MIDI quantisation", _globalMidiQuantRadio, 101u },
		{ SettingsPage::Timing, "Local phase offset (loops)", _transportOffsetInput, TransportOffsetControlIndex },
		{ SettingsPage::Timing, "Subdivision: local grain / remote beat", _midiSubdivisionRadio, MidiSubdivisionControlIndex },
		{ SettingsPage::Timing, "", _tapButton, TapTempoControlIndex },
		{ SettingsPage::Midi, "MIDI channel override", _midiChannelOverride, MidiChannelOverrideControlIndex },
		{ SettingsPage::Midi, "", std::make_shared<GuiLabel>(GuiLabelParams::PanelHeader("All off = unchanged", 300u)), 0u },
		{ SettingsPage::Timing, "", _ninjamMetronomeToggle, NinjamMetronomeControlIndex },
		{ SettingsPage::Session, "", _saveButton, SaveSessionControlIndex },
		{ SettingsPage::Session, "", _saveDefaultJamButton, DefaultSessionControlIndex },
		{ SettingsPage::Session, "", _loadButton, LoadSessionControlIndex },
		{ SettingsPage::Session, "", _saveAsJamButton, SaveSessionAsControlIndex }
	};
}

std::vector<GuiSettingEntry> SceneSettingsControls::SelectionEntries() const
{
	return { { SettingsPage::Selection, "", _modeRadio, 100u } };
}

std::optional<SceneSettingsControls::Change> SceneSettingsControls::Decode(const GuiAction& action,
	double acceptedOffset) const
{
	if (action.ElementType == GuiAction::ACTIONELEMENT_BUTTON && action.Index == TapTempoControlIndex)
		return TapTempo{};
	if (auto value = std::get_if<GuiAction::GuiInt>(&action.Data))
	{
		if (action.ElementType == GuiAction::ACTIONELEMENT_RADIO)
		{
			if (action.Index == MidiChannelOverrideControlIndex)
				return ChannelChange{ static_cast<std::uint8_t>(std::clamp(value->Value, 0, 16)) };
			if (action.Index == 100u) return ViewModeChange{ static_cast<unsigned int>(std::clamp(value->Value, 0, 2)) };
			if (action.Index == 101u)
			{
				using State = io::JamFile::GlobalMidiQuantState;
				const auto state = value->Value >= static_cast<int>(State::Off) && value->Value <= static_cast<int>(State::All)
					? static_cast<State>(value->Value) : State::Mixed;
				return GlobalQuantisationChange{ state };
			}
			if (action.Index == MidiSubdivisionControlIndex)
				return SubdivisionChange{ midi::MidiQuantisation::ClampFractionDisplayIndex(value->Value) };
		}
		if (action.ElementType == GuiAction::ACTIONELEMENT_TOGGLE && action.Index == NinjamMetronomeControlIndex)
			return MetronomeChange{ value->Value == GuiToggleParams::TOGGLE_ON };
	}
	if (action.ElementType != GuiAction::ACTIONELEMENT_RACK) return std::nullopt;
	if (action.Index == TransportOffsetControlIndex)
	{
		double offset = acceptedOffset;
		if (auto text = std::get_if<GuiAction::GuiString>(&action.Data))
		{
			try
			{
				size_t consumed = 0;
				const double parsed = std::stod(text->Value, &consumed);
				if (consumed == text->Value.size() && std::isfinite(parsed)) offset = parsed;
			}
			catch (...) { /* Retain the accepted offset during invalid edits. */ }
		}
		else if (auto number = std::get_if<GuiAction::GuiDouble>(&action.Data)) offset = number->Value;
		else return std::nullopt;
		return OffsetChange{ offset }; // Scene retains engine-side validation and clamping.
	}
	return std::nullopt;
}

void SceneSettingsControls::RefreshChannelPresentation(std::uint8_t channel)
{
	if (_midiChannelOverride->Channel() != channel)
		_midiChannelOverride->SetChannel(channel);
}

std::shared_ptr<GuiLabel> SceneSettingsControls::_StatusLabel(const std::string& text, unsigned int height)
{
	auto params = GuiLabelParams::PanelHeader(text, 320, height);
	params.FontSize = graphics::FontOptions::FONT_SMALL;
	return std::make_shared<GuiLabel>(params);
}

std::shared_ptr<GuiButton> SceneSettingsControls::_SessionButton(const std::string& text)
{
	auto params = GuiButtonParams::PanelButton(240u);
	params.Text = text;
	params.DispatchCommand = true;
	params.Size.Height = 40u;
	params.MinSize.Height = 40u;
	params.TextFontSize = graphics::FontOptions::FONT_SMALL;
	return std::make_shared<GuiButton>(params);
}

void SceneSettingsControls::SetSessionEnabled(bool enabled, bool defaultEnabled)
{
	_saveButton->SetEnabled(enabled);
	_saveAsJamButton->SetEnabled(enabled);
	_loadButton->SetEnabled(enabled);
	_saveDefaultJamButton->SetEnabled(defaultEnabled);
}

bool SceneSettingsControls::IsOperationControl(unsigned int index)
{
	return index >= AudioDeviceControlIndex && index < MidiDeviceControlLimit;
}

std::optional<SceneSettingsControls::DeviceChange> SceneSettingsControls::DecodeDeviceChange(const GuiAction& action) const
{
	switch (action.Index)
	{
	case SaveSessionControlIndex: return SaveSession{ (action.InputModifiers & base::Action::MODIFIER_ALT) != 0, false };
	case SaveSessionAsControlIndex: return SaveSession{ true, false };
	case DefaultSessionControlIndex: return SaveSession{ false, true };
	case LoadSessionControlIndex: return LoadSession{};
	case RefreshMidiControlIndex: return RefreshMidi{};
	case AudioDeviceControlIndex:
		if (const auto value = std::get_if<GuiAction::GuiInt>(&action.Data);
			value && value->Value >= 0 && static_cast<std::size_t>(value->Value) < _audioChoiceNames.size())
			return AudioDeviceChange{ _audioChoiceNames[value->Value] };
		return std::nullopt;
	case AudioInputControlIndex + static_cast<unsigned int>(AudioField::SampleRate):
		// A device choice can arrive before its page is rebuilt. Never decode an
		// index from the previous device as a rate for the new draft.
		if (_audioRateDeviceName != _audioDraft.Name) return std::nullopt;
		if (const auto value = std::get_if<GuiAction::GuiInt>(&action.Data);
			value && value->Value >= 0 && static_cast<std::size_t>(value->Value) < _audioRateChoices.size()
			&& _audioRateChoices[value->Value] != 0u)
			return AudioValueChange{ AudioField::SampleRate, _audioRateChoices[value->Value] };
		return std::nullopt;
	default: break;
	}
	if (action.Index >= AudioInputControlIndex && action.Index < AudioInputControlIndex + _audioInputs.size())
	{
		const auto index = action.Index - AudioInputControlIndex;
		if (_audioInputs[index])
			return AudioValueChange{ static_cast<AudioField>(index), static_cast<unsigned int>(_audioInputs[index]->Value()) };
		return std::nullopt;
	}
	if (action.Index >= MidiDeviceControlIndex && action.Index < MidiDeviceControlLimit)
	{
		const auto index = action.Index - MidiDeviceControlIndex;
		if (index < _midiChoices.size())
		{
			const auto& choice = _midiChoices[index];
			return MidiDeviceChange{ index, choice.HardwareName, choice.RegistryId,
				choice.Toggle->GetToggleState() == GuiToggleParams::TOGGLE_ON };
		}
	}
	return std::nullopt;
}

void SceneSettingsControls::SetMidiChoiceRegistryId(std::size_t choice, const std::string& id)
{
	_midiChoices.at(choice).RegistryId = id;
}

SceneSettingsControls::DevicePages SceneSettingsControls::BuildDevicePages(
	const audio::AsioInventory& inventory, const io::UserConfig::AudioSettings& audioDraft,
	const std::unordered_map<std::string, std::vector<unsigned int>>& acceptedBufferSizes,
	const midi::MidiConnectionResult& midi, const std::vector<io::UserConfig::MidiSettings>& midiDraft,
	GuiPopupManager& popupManager, const std::shared_ptr<base::ActionReceiver>& receiver)
{
	std::vector<GuiSettingEntry> entries;
	std::vector<GuiSettingEntry> midiEntries;
	for (auto entry : SettingsEntries())
		if (entry.Page == SettingsPage::Midi) midiEntries.push_back(std::move(entry));
	_audioChoiceNames.clear();
	auto dropdown = GuiDropDownParams::PanelInput(300u);
	for (const auto& device : inventory.Devices)
	{
		_audioChoiceNames.push_back(device.Name);
		dropdown.Items.push_back(device.Name + (device.Probed ? "" : " (unavailable)"));
	}
	if (std::find(_audioChoiceNames.begin(), _audioChoiceNames.end(), audioDraft.Name) == _audioChoiceNames.end())
	{
		_audioChoiceNames.push_back(audioDraft.Name); dropdown.Items.push_back(audioDraft.Name + " (missing)");
	}
	dropdown.InitIndex = static_cast<unsigned int>(std::find(_audioChoiceNames.begin(), _audioChoiceNames.end(), audioDraft.Name) - _audioChoiceNames.begin());
	_audioDeviceChoice = std::make_shared<GuiDropDown>(dropdown);
	_audioDeviceChoice->SetPopupManager(&popupManager);
	entries.push_back({SettingsPage::Audio, "ASIO device", _audioDeviceChoice, AudioDeviceControlIndex});
	const auto selected = std::find_if(inventory.Devices.begin(), inventory.Devices.end(), [&audioDraft](const auto& device){return device.Name == audioDraft.Name;});
	if (selected != inventory.Devices.end())
	{
		const std::string details = "Limits: " + std::to_string(selected->InputChannels) + " in / " + std::to_string(selected->OutputChannels) + " out";
		auto detailsParams = GuiLabelParams::PanelHeader(details, 320);
		detailsParams.FontSize = graphics::FontOptions::FONT_SMALL;
		entries.push_back({SettingsPage::Audio,"",std::make_shared<GuiLabel>(detailsParams),0u});
	}
	const std::array<unsigned int,4> values{audioDraft.NumChannelsIn, audioDraft.NumChannelsOut, audioDraft.SampleRate, audioDraft.BufSize};
	std::array<std::string,4> labels{"First N inputs (settles for 3 seconds)", "First N outputs (settles for 3 seconds)", "Sample rate (settles for 3 seconds; no takes only)", "Buffer size (settles for 3 seconds; negotiated on open)"};
	if (const auto found = acceptedBufferSizes.find(audioDraft.Name); found != acceptedBufferSizes.end())
	{
		labels[3] += " Accepted:"; for (const auto size : found->second) labels[3] += " " + std::to_string(size);
	}
	for (unsigned int i = 0; i < 4; ++i)
	{
		if (i == static_cast<unsigned int>(AudioField::SampleRate))
		{
			_audioRateDeviceName = audioDraft.Name;
			_audioRateChoices = selected != inventory.Devices.end() && selected->Probed
				? selected->SampleRates : std::vector<unsigned int>{};
			auto rates = GuiDropDownParams::PanelInput(140u);
			for (const auto rate : _audioRateChoices) rates.Items.push_back(std::to_string(rate) + " Hz");
			const auto current = std::find(_audioRateChoices.begin(), _audioRateChoices.end(), audioDraft.SampleRate);
			if (current == _audioRateChoices.end() && audioDraft.SampleRate != 0u)
			{
				// Preserve an existing rate when a device cannot offer it (for
				// example while takes prevent changing the running sample rate).
				rates.InitIndex = static_cast<unsigned int>(_audioRateChoices.size());
				_audioRateChoices.push_back(0u);
				rates.Items.push_back(std::to_string(audioDraft.SampleRate) + " Hz (unavailable)");
			}
			else if (current != _audioRateChoices.end())
				rates.InitIndex = static_cast<unsigned int>(current - _audioRateChoices.begin());
			_audioSampleRateChoice = std::make_shared<GuiDropDown>(rates);
			_audioSampleRateChoice->SetPopupManager(&popupManager);
			entries.push_back({SettingsPage::Audio, labels[i], _audioSampleRateChoice, AudioInputControlIndex+i});
			continue;
		}
		auto params = GuiNumericInputParams::PanelInput(140u);
		params.IntegerOnly = true; params.RejectOutOfRange = true;
		params.OnInvalidValue = [this, i]() { _deviceStatus->SetString(i < 2u
			? "Enter a whole number within the selected device's channel limit"
			: "Enter a valid whole number; value exceeds allowed integer limits"); };
		params.Min = i < 2u ? 0.0 : 1.0;
		params.Max = i < 2u && selected != inventory.Devices.end() && selected->Probed
			? static_cast<double>(i == 0u ? selected->InputChannels : selected->OutputChannels)
			: static_cast<double>((std::numeric_limits<unsigned int>::max)());
		params.Step = 1.0; params.Decimals = 0; params.InitValue = values[i];
		_audioInputs[i] = std::make_shared<GuiNumericInput>(params);
		if (i < 2u && (selected == inventory.Devices.end() || !selected->Probed)) _audioInputs[i]->SetEnabled(false);
		entries.push_back({SettingsPage::Audio, labels[i], _audioInputs[i], AudioInputControlIndex+i});
	}
	entries.push_back({SettingsPage::Audio, "Active stream", _activeAudioStatus, 0u});
	entries.push_back({SettingsPage::Audio, "Status", _deviceStatus, 0u});
	auto button = [&](const std::string& text, unsigned int command){ auto params = GuiButtonParams::PanelButton(240u); params.Text = text; params.DispatchCommand = true; params.Size.Height = 40u; params.MinSize.Height = 40u; params.TextFontSize = graphics::FontOptions::FONT_SMALL; entries.push_back({SettingsPage::Audio,"",std::make_shared<GuiButton>(params),command}); };
	_midiChoices.clear(); _midiChoiceBindings.clear();
	auto midiTitle = GuiLabelParams::PanelHeader(midi.Inventory.Error.empty() ? "MIDI input selection" : "MIDI inventory: " + midi.Inventory.Error, 320, 66u);
	midiTitle.FontSize = graphics::FontOptions::FONT_SMALL;
	midiEntries.push_back({SettingsPage::Midi, "", std::make_shared<GuiLabel>(midiTitle), 0u});
	// Existing registry entries remain independent even when hardware selectors
	// coincide. Inventory-only choices acquire their ID when first selected.
	auto choices = midiDraft;
	for (const auto& device : midi.Inventory.Devices)
		if (std::none_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.Name == device.Name; }))
			choices.push_back({device.Name, false, {}});
	std::shared_ptr<GuiStackPanel> midiGroup;
	for (const auto& choice : choices)
	{
		if (_midiChoices.size() >= 1000u) break;
		if (_midiChoices.size() % 128u == 0u)
		{
			GuiStackPanelParams groupParams; groupParams.Size = {300u, 8u}; groupParams.Spacing = 8u;
			midiGroup = std::make_shared<GuiStackPanel>(groupParams);
			midiEntries.push_back({SettingsPage::Midi,"",midiGroup,0u});
		}
		auto params = GuiToggleParams::PanelPrimary(); params.Size = {300u,36u}; params.HorizSizing = base::LayoutSizing::Fill;
		params.InitState = choice.Enabled ? GuiToggleParams::TOGGLE_ON : GuiToggleParams::TOGGLE_OFF;
		const auto attempt = std::find_if(midi.Attempts.begin(), midi.Attempts.end(),
			[&choice](const auto& a){return !choice.Id.empty() && a.RegistryId == choice.Id;});
		std::string status = "disabled";
		if (attempt != midi.Attempts.end()) switch (attempt->Status)
		{
		case midi::MidiConnectionStatus::Connected: status = "connected"; break;
		case midi::MidiConnectionStatus::Missing: status = "missing"; break;
		case midi::MidiConnectionStatus::Ambiguous: status = "ambiguous"; break;
		case midi::MidiConnectionStatus::Failed: status = "failed"; break;
		case midi::MidiConnectionStatus::CapacityExceeded: status = "capacity exceeded"; break;
		default: break;
		}
		params.Text = choice.Name + (choice.Id.empty() ? "" : " [" + choice.Id + "]") + " (" + status + ")";
		params.TextFontSize = graphics::FontOptions::FONT_SMALL;
		params.TextEllipsize = true;
		auto control = std::make_shared<GuiToggle>(params);
		auto binding = std::make_shared<GuiCommandReceiver>(MidiDeviceControlIndex+static_cast<unsigned int>(_midiChoices.size()), receiver);
		control->SetReceiver(binding); _midiChoiceBindings.push_back(binding);
		midiGroup->AddChild(control); midiGroup->SetSize({300u, static_cast<unsigned int>((_midiChoices.size() % 128u + 1u) * 44u - 8u)}); midiGroup->ComputeLayout();
		_midiChoices.push_back({choice.Name, choice.Id, control});
	}
	auto refreshMidi = GuiButtonParams::PanelButton(240u); refreshMidi.Text = "Refresh MIDI devices"; refreshMidi.DispatchCommand = true;
	refreshMidi.Size.Height = 40u; refreshMidi.MinSize.Height = 40u; refreshMidi.TextFontSize = graphics::FontOptions::FONT_SMALL;
	midiEntries.push_back({SettingsPage::Midi, "", std::make_shared<GuiButton>(refreshMidi), RefreshMidiControlIndex});
	return { std::move(entries), std::move(midiEntries) };
}

SceneSettingsControls::DraftChange SceneSettingsControls::EditDeviceDraft(const DeviceChange& command,
	const audio::AsioInventory& inventory, const std::vector<io::UserConfig::SerialSettings>& serialDevices,
	bool allowRateFallback, unsigned int activeSampleRate)
{
	if (const auto value = std::get_if<AudioDeviceChange>(&command))
	{
		_audioDraft.Name = value->Name;
		// A pending rate edit belongs to the old device. Start the new request
		// from the running rate before choosing a supported fallback.
		if (activeSampleRate != 0u) _audioDraft.SampleRate = activeSampleRate;
		const auto found = std::find_if(inventory.Devices.begin(), inventory.Devices.end(), [this](const auto& d){ return d.Name == _audioDraft.Name; });
		if (found != inventory.Devices.end() && found->Probed)
		{
			_audioDraft.NumChannelsIn = std::min(_audioDraft.NumChannelsIn, found->InputChannels);
			_audioDraft.NumChannelsOut = std::min(_audioDraft.NumChannelsOut, found->OutputChannels);
			if (!_audioDraft.NumChannelsIn && !_audioDraft.NumChannelsOut) { _audioDraft.NumChannelsOut = std::min(2u, found->OutputChannels); if (!_audioDraft.NumChannelsOut) _audioDraft.NumChannelsIn = std::min(2u, found->InputChannels); }
			if (allowRateFallback && std::find(found->SampleRates.begin(), found->SampleRates.end(), _audioDraft.SampleRate) == found->SampleRates.end() && !found->SampleRates.empty())
				_audioDraft.SampleRate = std::find(found->SampleRates.begin(), found->SampleRates.end(), found->PreferredSampleRate) != found->SampleRates.end()
					? found->PreferredSampleRate : found->SampleRates.front();
		}
		++_audioDraftRevision;
		return DraftChange::Device;
	}
	if (std::holds_alternative<AudioValueChange>(command))
	{
		const auto& change = std::get<AudioValueChange>(command);
		using Field = SceneSettingsControls::AudioField;
		if (change.Field == Field::SampleRate)
		{
			if (change.Value == 0u || _audioRateDeviceName != _audioDraft.Name ||
				std::find(_audioRateChoices.begin(), _audioRateChoices.end(), change.Value) == _audioRateChoices.end())
				return DraftChange::None;
		}
		if (change.Field == Field::Inputs) _audioDraft.NumChannelsIn = change.Value;
		if (change.Field == Field::Outputs) _audioDraft.NumChannelsOut = change.Value;
		if (change.Field == Field::SampleRate) _audioDraft.SampleRate = change.Value;
		if (change.Field == Field::BufferSize) _audioDraft.BufSize = change.Value;
		++_audioDraftRevision;
		return change.Field == AudioField::Inputs || change.Field == AudioField::Outputs ? DraftChange::Channels : DraftChange::Format;
	}
	if (std::holds_alternative<MidiDeviceChange>(command))
	{
		const auto& choice = std::get<MidiDeviceChange>(command);
		const bool enabled = choice.Enabled;
		auto found = std::find_if(_midiDraft.begin(), _midiDraft.end(), [&choice](const auto& d){ return d.Id == choice.RegistryId; });
		if (found == _midiDraft.end())
		{
			std::string id;

			for (size_t suffix = 1u;; ++suffix)
			{
				id = "midi-" + std::to_string(suffix);
				const bool midiCollision = std::any_of(_midiDraft.begin(), _midiDraft.end(), [&](const auto& device) { return device.Id == id; });
				const bool serialCollision = std::any_of(serialDevices.begin(), serialDevices.end(),
					[&](const auto& device) { return device.Name == id; });
				if (id != io::RigFile::KeyboardDevice && !midiCollision && !serialCollision) break;
			}
			SetMidiChoiceRegistryId(choice.Choice, id);
			_midiDraft.push_back({choice.Name, enabled, std::move(id)});
		}
		else found->Enabled = enabled;
		++_midiDraftRevision;
		return DraftChange::Midi;
	}
	return DraftChange::None;
}

void SceneSettingsControls::SetAudioDraftFromStream(const audio::AudioStreamParams& params)
{
	auto draft = AudioDraft();
	// Startup may resolve a legacy/default request or negotiate stream geometry.
	// Show the running configuration without rewriting the accepted rig on disk.
	draft.Name = params.Name;
	draft.SampleRate = params.SampleRate;
	draft.NumChannelsIn = params.NumInputChannels;
	draft.NumChannelsOut = params.NumOutputChannels;
	draft.BufSize = params.BufSize;
	draft.NumBuffers = params.NumBuffers;
	SetAudioDraft(std::move(draft));
}
