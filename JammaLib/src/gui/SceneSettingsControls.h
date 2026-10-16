#pragma once

#include <cstdint>
#include <optional>
#include <variant>
#include "GuiMainPanel.h"
#include "GuiButton.h"
#include "GuiNumericInput.h"
#include "../io/JamFile.h"
#include "../midi/MidiQuantisation.h"

namespace gui
{
	// Optional exclusive choice: the radio's unused index represents pass-through.
	class GuiMidiChannelOverride : public GuiRadio
	{
	public:
		explicit GuiMidiChannelOverride(std::uint8_t channel);
		using GuiRadio::OnAction;
		actions::ActionResult OnAction(actions::GuiAction action) override;
		std::uint8_t Channel() const;
		void SetChannel(std::uint8_t channel);
	private:
		static constexpr unsigned int UnchangedIndex = 16u;
		static GuiRadioParams _Params(std::uint8_t channel);
	};

	// Concrete session widgets and value conversion; subsystem effects stay in Scene.
	class SceneSettingsControls
	{
	public:
		struct ViewModeChange { unsigned int Value; };
		struct GlobalQuantisationChange { io::JamFile::GlobalMidiQuantState Value; };
		struct ChannelChange { std::uint8_t Value; };
		struct OffsetChange { double Value; };
		struct MetronomeChange { bool Enabled; };
		struct SubdivisionChange { midi::MidiQuantisationFraction Value; };
		struct TapTempo {};
		using Change = std::variant<ViewModeChange, GlobalQuantisationChange, ChannelChange,
			OffsetChange, MetronomeChange, SubdivisionChange, TapTempo>;

		SceneSettingsControls(std::uint8_t channel, double offset, io::JamFile::GlobalMidiQuantState quantisation);
		std::vector<GuiSettingEntry> SettingsEntries() const;
		std::vector<GuiSettingEntry> SelectionEntries() const;
		// Text notifications include in-progress edits. Invalid text retains the
		// caller's accepted offset, not the widget's possibly uncommitted value.
		std::optional<Change> Decode(const actions::GuiAction& action,
			double acceptedOffset) const;
		const auto& Mode() const { return _modeRadio; }
		const auto& Channel() const { return _midiChannelOverride; }
		const auto& Metronome() const { return _ninjamMetronomeToggle; }
		const auto& Offset() const { return _transportOffsetInput; }
		const auto& GlobalQuantisation() const { return _globalMidiQuantRadio; }
		const auto& Subdivision() const { return _midiSubdivisionRadio; }
		void RefreshChannelPresentation(std::uint8_t channel);

	private:
		static constexpr unsigned int MidiChannelOverrideControlIndex = 7001u;
		static constexpr unsigned int TransportOffsetControlIndex = 7002u;
		static constexpr unsigned int NinjamMetronomeControlIndex = 7003u;
		static constexpr unsigned int TapTempoControlIndex = 7004u;
		static constexpr unsigned int MidiSubdivisionControlIndex = 7005u;
		std::shared_ptr<GuiRadio> _modeRadio;
		std::shared_ptr<GuiMidiChannelOverride> _midiChannelOverride;
		std::shared_ptr<GuiNumericInput> _transportOffsetInput;
		std::shared_ptr<GuiToggle> _ninjamMetronomeToggle;
		std::shared_ptr<GuiRadio> _globalMidiQuantRadio;
		std::shared_ptr<GuiRadio> _midiSubdivisionRadio;
		std::shared_ptr<GuiButton> _tapButton;
	};
}
