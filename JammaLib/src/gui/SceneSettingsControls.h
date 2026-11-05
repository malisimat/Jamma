#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <optional>
#include <variant>
#include "GuiMainPanel.h"
#include "GuiButton.h"
#include "GuiNumericInput.h"
#include "GuiSlider.h"
#include "../io/JamFile.h"
#include "../engine/SessionControls.h"
#include "../audio/AudioDevice.h"
#include "../midi/MidiDevice.h"
#include "GuiDropDown.h"
#include "../midi/MidiQuantisation.h"

namespace ninjam { enum class NinjamLocalTempoState : std::uint8_t; }

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
		struct PlaybackRateChange { double Value; };
		struct RequestServerTempo {};
		struct MetronomeChange { bool Enabled; };
		struct MetronomeLevelChange { float Value; };
		struct SubdivisionChange { midi::MidiQuantisationFraction Value; };
		struct TapTempo {};
		struct Reclock {};
        struct UseMaster {};
		struct SyncToServer {};
		struct ResetAll { engine::SessionResetMode Mode=engine::SessionResetMode::PreserveStationVsts; };
		struct SaveSession { bool ChooseFolder; bool AsDefault; };
		struct LoadSession {};
		struct RefreshMidi {};
		struct RefreshAudio {};
		struct AudioDeviceChange { std::string Name; };
		enum class AudioField { Inputs, Outputs, SampleRate, BufferSize };
		struct AudioValueChange { AudioField Field; unsigned int Value; };
		struct MidiDeviceChange { std::size_t Choice; std::string Name, RegistryId; bool Enabled; };
		using DeviceChange = std::variant<SaveSession, LoadSession, RefreshMidi, RefreshAudio,
			AudioDeviceChange, AudioValueChange, MidiDeviceChange>;
		struct DevicePages { std::vector<GuiSettingEntry> Audio, Midi; };
		using Change = std::variant<ViewModeChange, GlobalQuantisationChange, ChannelChange,
			OffsetChange, PlaybackRateChange, RequestServerTempo, MetronomeChange, MetronomeLevelChange, SubdivisionChange, TapTempo, Reclock, UseMaster, SyncToServer, ResetAll>;

		enum class DraftChange { None, Device, Channels, Format, Midi };
		DraftChange EditDeviceDraft(const DeviceChange& command, const audio::AsioInventory& inventory,
			const std::vector<io::UserConfig::SerialSettings>& serialDevices, bool allowRateFallback,
			unsigned int activeSampleRate);
		std::uint64_t AudioDraftRevision() const noexcept { return _audioDraftRevision; }
		std::uint64_t MidiDraftRevision() const noexcept { return _midiDraftRevision; }
		void AcceptAudioDraft(io::UserConfig::AudioSettings value, std::uint64_t revision) { if (revision == _audioDraftRevision) _audioDraft = std::move(value); }
		void AcceptMidiDraft(std::vector<io::UserConfig::MidiSettings> value, std::uint64_t revision) { if (revision == _midiDraftRevision) _midiDraft = std::move(value); }
		const io::UserConfig::AudioSettings& AudioDraft() const noexcept { return _audioDraft; }
		const std::vector<io::UserConfig::MidiSettings>& MidiDraft() const noexcept { return _midiDraft; }
		void SetAudioDraftFromStream(const audio::AudioStreamParams& params);
		void SetAudioDraft(io::UserConfig::AudioSettings value) { _audioDraft = std::move(value); }
		void SetMidiDraft(std::vector<io::UserConfig::MidiSettings> value) { _midiDraft = std::move(value); }
		SceneSettingsControls(std::uint8_t channel, double offset, io::JamFile::GlobalMidiQuantState quantisation);
		std::vector<GuiSettingEntry> SettingsEntries() const;
		std::vector<GuiSettingEntry> SelectionEntries() const;
		// Decode control candidates; Scene retains domain acceptance.
		std::optional<Change> Decode(const actions::GuiAction& action,
			double acceptedOffset) const;
		const auto& Mode() const { return _modeRadio; }
		const auto& Channel() const { return _midiChannelOverride; }
		const auto& MetronomeLevel() const { return _metronomeLevel; }
		const auto& Metronome() const { return _ninjamMetronomeToggle; }
		const auto& Offset() const { return _transportOffsetSlider; }
        const auto& MasterButton() const { return _useMasterButton; }
		const auto& GlobalQuantisation() const { return _globalMidiQuantRadio; }
		const auto& Subdivision() const { return _midiSubdivisionRadio; }
		void RefreshNinjamStatus(ninjam::NinjamLocalTempoState state, bool connected, bool following, bool aligning = false);
		void RefreshChannelPresentation(std::uint8_t channel);
		void RefreshOffsetPresentation(double acceptedOffset);
		void RefreshPlaybackRate(double target, double applied, bool following, bool canRequest, double bpm, unsigned int bpi, bool unsupportedRemoteRate = false, bool aligning = false, bool controlPending = false);
        void RefreshMasterPresentation(const std::string& target,const std::string& status,const std::string& current);
		static std::string FormatOffset(double acceptedOffset);
		DevicePages BuildDevicePages(const audio::AsioInventory& inventory,
			const io::UserConfig::AudioSettings& audioDraft,
			const std::unordered_map<std::string, std::vector<unsigned int>>& acceptedBufferSizes,
			const midi::MidiConnectionResult& midi,
			const std::vector<io::UserConfig::MidiSettings>& midiDraft,
			GuiPopupManager& popupManager, const std::shared_ptr<base::ActionReceiver>& receiver);
		std::optional<DeviceChange> DecodeDeviceChange(const actions::GuiAction& action) const;
		static bool IsOperationControl(unsigned int index);
		void SetMidiChoiceRegistryId(std::size_t choice, const std::string& id);
		void SetSessionEnabled(bool enabled, bool defaultEnabled);
		void SetDeviceStatus(const std::string& text) { _deviceStatus->SetString(text); }
		void SetActiveAudioStatus(const std::string& text) { _activeAudioStatus->SetString(text); }
		void SetFileStatus(const std::string& text) { _fileStatus->SetString(text); }
		const auto& MidiDevice(std::size_t index) const { return _midiChoices.at(index).Toggle; }
		const auto& AudioDevice() const { return _audioDeviceChoice; }
		const auto& AudioSampleRate() const { return _audioSampleRateChoice; }
		const auto& AudioInput(std::size_t index) const { return _audioInputs.at(index); }

	private:
		std::uint64_t _audioDraftRevision = 0u, _midiDraftRevision = 0u;
		io::UserConfig::AudioSettings _audioDraft{};
		std::vector<io::UserConfig::MidiSettings> _midiDraft;
		static constexpr unsigned int AudioDeviceControlIndex = 7200u;
		static constexpr unsigned int AudioInputControlIndex = 7201u;
		static constexpr unsigned int RefreshMidiControlIndex = 7206u;
		static constexpr unsigned int RefreshAudioControlIndex = 7207u;
		static constexpr unsigned int SaveSessionControlIndex = 7210u;
		static constexpr unsigned int LoadSessionControlIndex = 7211u;
		static constexpr unsigned int DefaultSessionControlIndex = 7212u;
		static constexpr unsigned int SaveSessionAsControlIndex = 7213u;
		static constexpr unsigned int ResetAllControlIndex = 7214u;
        static constexpr unsigned int ResetWithVstsControlIndex=7215u;
		static constexpr unsigned int ReclockControlIndex = 7006u;
        static constexpr unsigned int UseMasterControlIndex = 7010u;
		static constexpr unsigned int SyncToServerControlIndex = 7007u;
		static constexpr unsigned int PlaybackRateControlIndex = 7011u;
		static constexpr unsigned int RequestServerTempoControlIndex = 7012u;
		static constexpr unsigned int MidiDeviceControlIndex = 7300u;
		static constexpr unsigned int MidiDeviceControlLimit = 8300u;
		static constexpr unsigned int MidiChannelOverrideControlIndex = 7001u;
		static constexpr unsigned int TransportOffsetControlIndex = 7002u;
		static constexpr unsigned int MetronomeLevelControlIndex = 7008u;
		static constexpr unsigned int NinjamMetronomeControlIndex = 7003u;
		static constexpr unsigned int TapTempoControlIndex = 7004u;
		static constexpr unsigned int MidiSubdivisionControlIndex = 7005u;
		std::shared_ptr<GuiRadio> _modeRadio;
		std::shared_ptr<GuiMidiChannelOverride> _midiChannelOverride;
		std::shared_ptr<GuiSlider> _transportOffsetSlider, _playbackRateSlider;
		std::shared_ptr<GuiLabel> _playbackRateLabel, _serverTempoLabel;
		std::shared_ptr<GuiButton> _requestServerTempoButton;
		std::shared_ptr<GuiLabel> _transportOffsetLabel;
		std::shared_ptr<GuiToggle> _ninjamMetronomeToggle;
		std::shared_ptr<GuiSlider> _metronomeLevel;
		std::shared_ptr<GuiRadio> _globalMidiQuantRadio;
		std::shared_ptr<GuiRadio> _midiSubdivisionRadio;
		std::shared_ptr<GuiButton> _tapButton, _reclockButton, _useMasterButton, _syncToServerButton, _resetAllButton, _resetWithVstsButton;
		std::shared_ptr<GuiButton> _saveButton, _saveDefaultJamButton, _loadButton, _saveAsJamButton;
		std::shared_ptr<GuiLabel> _deviceStatus, _activeAudioStatus, _fileStatus, _ninjamStatus, _masterStatus, _masterTarget;
		std::vector<std::string> _audioChoiceNames;
		std::shared_ptr<GuiDropDown> _audioDeviceChoice;
		std::shared_ptr<GuiDropDown> _audioSampleRateChoice;
		std::vector<unsigned int> _audioRateChoices;
		std::string _audioRateDeviceName;
		std::array<std::shared_ptr<GuiNumericInput>, 4> _audioInputs;
		struct MidiDeviceChoice
		{
			std::string HardwareName, RegistryId;
			std::shared_ptr<GuiToggle> Toggle;
		};
		std::vector<MidiDeviceChoice> _midiChoices;
		std::vector<std::shared_ptr<GuiCommandReceiver>> _midiChoiceBindings;
		static std::shared_ptr<GuiButton> _SessionButton(const std::string& text);
		static std::shared_ptr<GuiLabel> _StatusLabel(const std::string& text, unsigned int height);
	};
}
