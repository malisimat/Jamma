#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>
#include "LoopTake.h"
#include "Quantisation.h"
#include "../graphics/QuantisationModel.h"
#include "../graphics/StationModel.h"
#include "Trigger.h"
#include "AudioSink.h"
#include "../audio/AudioMixer.h"
#include "../audio/AudioBuffer.h"
#include "../base/Jammable.h"
#include "../gui/GuiRack.h"
#include "../io/InitFile.h"
#include "../midi/MidiQueue.h"
#include "../midi/MidiVstOutputSink.h"
#include "../vst/VstChain.h"

namespace engine
{
	class StationParams :
		public base::JammableParams
	{
	public:
		StationParams() :
			base::JammableParams(
				base::GuiElementParams(0, DrawableParams{ "" },
					MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
					SizeableParams{ 1,1 },
					"",
					"",
					"",
					{})
				),
			Name(""),
			FadeSamps(constants::DefaultFadeSamps),
			NumBusChannels(0),
			StationLevelFallRate(0.01f)
		{
		}

	public:
		std::string Name;
		unsigned int FadeSamps;
		unsigned int NumBusChannels;
		float StationLevelFallRate;
	};
	
	class Station :
		public base::Tickable,
		public base::Jammable,
		public base::MultiAudioSink
	{
	public:
		enum StationPanelType
		{
			STATIONPANEL_MASTER,
			STATIONPANEL_MIXER,
			STATIONPANEL_ROUTER
		};

		static constexpr unsigned int LiveMidiOutputIndex = midi::LiveMidiOutputIndex;

	public:
		Station(StationParams params,
			audio::AudioMixerParams mixerParams);
		~Station();

		// Copy
		Station(const Station&) = delete;
		Station& operator=(const Station&) = delete;

	public:
		static std::optional<std::shared_ptr<Station>> FromFile(StationParams stationParams,
			audio::AudioMixerParams mixerParams,
			io::JamFile::Station stationStruct,
			std::wstring dir);
		static audio::AudioMixerParams GetMixerParams(utils::Size2d stationSize,
			audio::BehaviourParams behaviour);

		virtual std::string ClassName() const override { return "Station"; }
		virtual MultiAudioPlugType MultiAudioPlug() const override { return MULTIAUDIOPLUG_BOTH; }
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		virtual	utils::Position2d Position() const override;
		virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override;
		virtual unsigned int NumOutputChannels(base::Audible::AudioSourceType source) const override;
		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source) override;
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<Trigger> trigger,
			int indexOffset,
			unsigned int numSamps,
			std::uint32_t blockStartSample = 0u);
		virtual void EndMultiPlay(unsigned int numSamps) override;
		virtual void OnBlockWriteChannel(unsigned int channel,
			const base::AudioWriteRequest& request,
			int writeOffset) override;
		virtual void EndMultiWrite(unsigned int numSamps,
			bool updateIndex,
			Audible::AudioSourceType source) override;
		virtual void SetSelectDepth(base::SelectDepth depth) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		actions::ActionResult OnTriggerEvent(TriggerSource source,
			unsigned int value,
			unsigned int state,
			const base::Action& action,
			const std::string& device = "");
		virtual actions::ActionResult OnAction(actions::GuiAction action) override;
		virtual actions::ActionResult OnAction(actions::TriggerAction action) override;
		virtual void OnTick(Time curTime,
			unsigned int samps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params) override;
		virtual void Reset() override;
		
		const std::vector<std::shared_ptr<LoopTake>>& GetLoopTakes() const
		{
			return (_changesMade && _flipTakeBuffer) ? _backLoopTakes : _loopTakes;
		}
		// Returns true if this station receives audio from a remote ninjam user.
		// Overriding this instead of dynamic_cast keeps the audio callback path safe.
		virtual bool IsRemote() const noexcept { return false; }
		std::shared_ptr<LoopTake> AddTake();
		void AddTake(std::shared_ptr<LoopTake> take);
		void AddTrigger(std::shared_ptr<Trigger> trigger);
		unsigned int NumTakes() const;
		std::string Name() const;
		void SetName(std::string name);
		void SetClock(std::shared_ptr<Timer> clock);
		void SetQuantisationParams(std::optional<QuantisationParams> params, bool confirm = false);
		void ClearQuantisationParams();
		void SetQuantisationOverlayAlpha(float alpha) noexcept;
		void RefreshQuantisationOverlayFromClock();
		void SetupBuffers(unsigned int bufSize);
		void SetSampleRate(float sampleRate);
		void SetLogging(const io::LoggingConfig& config) noexcept;
		void SetNumBusChannels(unsigned int chans);
		void SetNumAdcChannels(unsigned int chans);
		void SetNumDacChannels(unsigned int chans);
		unsigned int NumBusChannels() const;
		void OnBounce(unsigned int numSamps,
			io::UserConfig config,
			std::optional<audio::AudioStreamParams> params = std::nullopt);
		void SetRackVisibility(bool showStationRack, bool showLoopTakeRacks);
		std::vector<io::JamFile::VstEntry> VstEntries() const;
		// Returns true if the named device is allowed to drive this station's live
		// VST playback. Any unrestricted trigger keeps the station open to all
		// devices; otherwise the device must match a trigger's MidiInputDevices list.
		bool AcceptsLiveMidiFromDevice(const std::string& deviceName) const noexcept;
		// For synthetic live MIDI events, like punch-in NoteOn/NoteOff pairs,
		// without associated deviceName.
		void EnqueueLiveMidiEvent(const midi::MidiEvent& event);
		// For real live MIDI input, with associated deviceName.
		void EnqueueLiveMidiEvent(const midi::MidiEvent& event, const std::string& deviceName);
		// Replacement semantics: one MIDI output routes to at most one plugin.
		void SetMidiVstRoute(unsigned int midiOutputIndex, size_t vstIndex);
		void ClearMidiVstRoutes();

		// VST chain management (non-RT, queued through the job thread).
		// LoadVstPlugin queues an async load; once the load completes the plugin
		// is inserted at the end of the station's effect chain.
		// initialState — optional VST2 state blob (from IVstPlugin::GetState) to
		// restore immediately after the plugin finishes loading.  Pass {} for
		// normal interactive loads where no state needs to be restored.
		void LoadVstPlugin(std::wstring path,
			std::vector<std::uint8_t> initialState = {});
		void UnloadVstPlugin(size_t index);

		// Non-RT accessor to retrieve a loaded plugin instance (or nullptr).
		std::shared_ptr<vst::IVstPlugin> GetVstPlugin(size_t index) const;

		// Called on the job thread to actually perform the load / unload.
		virtual actions::ActionResult OnAction(actions::JobAction action) override;

	protected:
		static constexpr utils::Size2d _Gap = { 5, 5 };
		static constexpr unsigned int _DefaultNumBusChannels = 8;

		static constexpr unsigned int _HiddenSeedSamps = 1u;
		static constexpr float _StationModelHeight = 470.0f;
		static constexpr float _StationModelYOffset = _StationModelHeight * 0.5f;

		static unsigned int _CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes);
		
		static void _DrainVstChain(std::shared_ptr<vst::VstChain> chain);
		static unsigned int _ResolveSampleRate(std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params);
		static void _TrySeedClockFromFirstLoop(const std::shared_ptr<engine::Timer>& clock,
			unsigned long loopLengthSamps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params);

		virtual void _InitReceivers() override;
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		virtual std::vector<actions::JobAction> _CommitChanges() override;
		virtual const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
			Audible::AudioSourceType source) override;
		virtual void _ArrangeChildren() override;

		struct AudioState
		{
			// weak_ptr: AudioState destruction on any thread won't trigger GL destructors
			std::vector<std::weak_ptr<LoopTake>> LoopTakes;
			std::vector<std::shared_ptr<audio::AudioMixer>> AudioMixers;
			std::vector<std::shared_ptr<audio::AudioBuffer>> AudioBuffers;
			std::vector<float> VstBlockScratch;
			std::vector<float*> VstBlockPtrs;
		};

		void _CollapseOtherTakeRouters();
		void _CollapseOtherTakeRoutersToChannels();
		void _PublishAudioState();
		std::shared_ptr<const AudioState> _AudioStateSnapshot() const;

		gui::GuiRackParams _GetRackParams(utils::Size2d size);
		std::optional<std::shared_ptr<LoopTake>> _TryGetTake(std::string id);
		void _WireVuSliders();
		using MidiVstRoutingSnapshot = midi::MidiVstRoutingSnapshot;

		// --- WriteBlock helpers (audio thread) ---

		// Copy each audio buffer into the corresponding VST scratch pointer.
		void _PrepareVstScratch(const AudioState& state, unsigned int sampsToRead) noexcept;

		// Run one VST block: update host time (derived from the clock and current
		// tempo), open the MIDI block, drain live MIDI, read MIDI from loop takes,
		// then process the audio block.
		// When vstActive is false only live MIDI is drained (to prevent queue back-log).
		void _RunVstBlock(vst::VstChain* chain,
			const MidiVstRoutingSnapshot* routes,
			const AudioState& state,
			bool vstActive,
			unsigned int channelCount,
			unsigned int sampsToRead,
			std::uint32_t blockStartSample) noexcept;

		// Enqueue NoteOffs for any held MIDI notes then call Ditch().
		// Must be called from the action thread; NoteOffs are delivered via EnqueueLiveMidiEvent.
		void _DitchLoopTake(std::shared_ptr<LoopTake>& take) noexcept;

		bool _flipTakeBuffer;
		bool _flipAudioBuffer;
		std::string _name;
		unsigned int _fadeSamps;
		unsigned int _lastBufSize;
		std::shared_ptr<Timer> _clock;
		std::shared_ptr<QuantisationModel> _quantisationModel;
		std::shared_ptr<graphics::StationModel> _stationModel;
		std::shared_ptr<gui::GuiRack> _guiRack;
		std::shared_ptr<audio::AudioMixer> _masterMixer;
		std::shared_ptr<gui::GuiToggle> _mixerToggle;
		std::shared_ptr<gui::GuiToggle> _routerToggle;
		std::shared_ptr<gui::GuiRouter> _router;
		std::vector<std::shared_ptr<LoopTake>> _loopTakes;
		std::vector<std::shared_ptr<Trigger>> _triggers;
		std::vector<std::shared_ptr<LoopTake>> _backLoopTakes;
		std::vector<std::shared_ptr<audio::AudioMixer>> _audioMixers;
		std::vector<std::shared_ptr<audio::AudioMixer>> _backAudioMixers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _audioBuffers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _backAudioBuffers;
		std::atomic<std::shared_ptr<const AudioState>> _audioState;

		// VST insert chain applied after all LoopTakes are mixed down,
		// just before each channel is sent to the output AudioMixer.
		// Published atomically for lock-free audio-thread reads.
		std::atomic<std::shared_ptr<vst::VstChain>> _vstChain;
		std::shared_ptr<vst::VstChain> _backVstChain;
		std::atomic<bool> _flipVstChain{ false };

		// Pending load/unload requests staged by LoadVstPlugin / UnloadVstPlugin.
		// Read only on the job thread (from OnAction(JobAction)).
		// Each entry carries the plugin path and an optional VST2 initial-state blob.
		std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>> _pendingVstLoads;
		std::vector<size_t> _pendingVstUnloads;
		// _vstPluginPaths: written on job thread (OnAction), read on main thread (VstEntries).
		// Access is guarded by _vstPathsMutex in both directions.
		mutable std::mutex _vstPathsMutex;
		std::vector<std::wstring> _vstPluginPaths;
		midi::MidiQueue<1024> _liveMidiIngress;
		mutable std::mutex _liveHeldMidiMutex;
		std::vector<std::pair<std::string, midi::MidiNoteSnapshot>> _liveHeldMidi;
		// Route snapshots are published off the audio thread.
		// The callback reads an immutable snapshot pointer for O(1) lookups.
		std::atomic<const MidiVstRoutingSnapshot*> _midiVstRoutes;
		std::vector<std::unique_ptr<MidiVstRoutingSnapshot>> _retainedMidiVstRoutes;

		// Sample rate and block size captured at SetupBuffers time; needed to
		// initialise a newly loaded plugin (IVstPlugin: VST2 or VST3).
		float _sampleRate = 44100.0f;
		unsigned int _blockSize = 512u;
		std::vector<float> _vstBlockScratch;
		std::vector<float*> _vstBlockPtrs;
		std::optional<QuantisationParams> _pendingQuantisationParams;
		bool _pendingQuantisationConfirm = false;
		float _quantisationOverlayAlpha = 0.0f;
	};
}
