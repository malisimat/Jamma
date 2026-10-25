#include "SceneSettingsControls.h"
#include <algorithm>
#include <cmath>

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
		{ SettingsPage::Timing, "", _ninjamMetronomeToggle, NinjamMetronomeControlIndex }
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
