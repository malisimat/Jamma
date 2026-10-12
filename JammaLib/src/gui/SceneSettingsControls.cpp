#include "SceneSettingsControls.h"
#include <algorithm>
#include <cmath>

using namespace gui;
using actions::GuiAction;

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
			toggleParams.ToggledOverTexture = "stationmode_over";
			toggleParams.ToggledDownTexture = "stationmode_down";
			break;
		case 1:
			toggleParams.Texture = "takemode";
			toggleParams.OverTexture = "takemode_over";
			toggleParams.DownTexture = "takemode_down";
			toggleParams.ToggledTexture = "takemode_toggled";
			toggleParams.ToggledOverTexture = "takemode_over";
			toggleParams.ToggledDownTexture = "takemode_down";
			break;
		case 2:
			toggleParams.Texture = "loopmode";
			toggleParams.OverTexture = "loopmode_over";
			toggleParams.DownTexture = "loopmode_down";
			toggleParams.ToggledTexture = "loopmode_toggled";
			toggleParams.ToggledOverTexture = "loopmode_over";
			toggleParams.ToggledDownTexture = "loopmode_down";
			break;
		}

		radioToggleParams.push_back(toggleParams);
	}

	modeRadioParams.ToggleParams = radioToggleParams;
	_modeRadio = std::make_shared<GuiRadio>(modeRadioParams);

	GuiNumericInputParams midiChannelOverrideParams = GuiNumericInputParams::PanelInput(72u);
	midiChannelOverrideParams.Index = MidiChannelOverrideControlIndex;
	midiChannelOverrideParams.Size = { 80, GuiNumericInputParams::DefaultHeight };
	midiChannelOverrideParams.Min = 0.0;
	midiChannelOverrideParams.Max = 16.0;
	midiChannelOverrideParams.Step = 0.1;
	midiChannelOverrideParams.Decimals = 0;
	midiChannelOverrideParams.InitValue = static_cast<double>(channel);
	_midiChannelOverrideInput = std::make_shared<GuiNumericInput>(midiChannelOverrideParams);


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
	metronomeToggleParams.Size = { 120, GuiToggleParams::DefaultHeight };
	metronomeToggleParams.MinSize = { 120, GuiToggleParams::DefaultHeight };
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

		globalQuantToggleParams.push_back(toggleParams);
	}
	globalMidiQuantRadioParams.ToggleParams = globalQuantToggleParams;
	_globalMidiQuantRadio = std::make_shared<GuiRadio>(globalMidiQuantRadioParams);

	auto tapParams = GuiButtonParams::PanelButton(180u);
	tapParams.Index = TapTempoControlIndex;
	tapParams.Text = "Tap tempo (Space)";
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
		subdivisionParams.ToggleParams.push_back(toggle);
	}
	_midiSubdivisionRadio = std::make_shared<GuiRadio>(subdivisionParams);

}

std::vector<GuiSettingEntry> SceneSettingsControls::SettingsEntries() const
{
	return {
		{ SettingsPage::Timing, "Global MIDI quantisation", _globalMidiQuantRadio, 101u },
		{ SettingsPage::Timing, "Local phase offset (loops)", _transportOffsetInput, TransportOffsetControlIndex },
		{ SettingsPage::Timing, "Subdivision: local grain / remote beat", _midiSubdivisionRadio, MidiSubdivisionControlIndex },
		{ SettingsPage::Timing, "", _tapButton, TapTempoControlIndex },
		{ SettingsPage::Midi, "MIDI channel (0 = unchanged)", _midiChannelOverrideInput, MidiChannelOverrideControlIndex },
		{ SettingsPage::Timing, "", _ninjamMetronomeToggle, NinjamMetronomeControlIndex }
	};
}

std::vector<GuiSettingEntry> SceneSettingsControls::SelectionEntries() const
{
	return { { SettingsPage::Selection, "", _modeRadio, 100u } };
}

std::optional<SceneSettingsControls::Change> SceneSettingsControls::Decode(const GuiAction& action,
	std::uint8_t acceptedChannel, double acceptedOffset) const
{
	if (action.ElementType == GuiAction::ACTIONELEMENT_BUTTON && action.Index == TapTempoControlIndex)
		return TapTempo{};
	if (auto value = std::get_if<GuiAction::GuiInt>(&action.Data))
	{
		if (action.ElementType == GuiAction::ACTIONELEMENT_RADIO)
		{
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
	if (action.Index == MidiChannelOverrideControlIndex)
	{
		int channel = acceptedChannel;
		if (auto text = std::get_if<GuiAction::GuiString>(&action.Data))
		{
			try
			{
				size_t consumed = 0;
				const int parsed = std::stoi(text->Value, &consumed);
				if (consumed == text->Value.size()) channel = std::clamp(parsed, 0, 16);
			}
			catch (...) { /* Retain the accepted channel during invalid edits. */ }
		}
		else if (auto number = std::get_if<GuiAction::GuiDouble>(&action.Data))
		{
			// Clamp in floating point before conversion; nonfinite values retain the owner value.
			if (std::isfinite(number->Value)) channel = static_cast<int>(std::clamp(number->Value + 0.5, 0.0, 16.0));
		}
		else return std::nullopt;
		return ChannelChange{ static_cast<std::uint8_t>(channel) };
	}
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
	if (!_midiChannelOverrideInput->HasFocus() && _midiChannelOverrideInput->Value() != channel)
		_midiChannelOverrideInput->SetValue(channel, false);
}
