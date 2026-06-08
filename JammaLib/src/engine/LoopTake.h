#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <memory>
#include "Loop.h"
#include "Quantisation.h"
#include "../midi/MidiLoop.h"
#include "../midi/MidiOverdub.h"
#include "Jammable.h"
#include "ActionUndo.h"
#include "Trigger.h"
#include "../audio/AudioMixer.h"
#include "../audio/AudioBuffer.h"
#include "../gui/GuiRack.h"
#include "../vst/VstChain.h"

using base::Audible;

namespace engine
{
	class LoopTakeParams :
		public base::JammableParams
	{
	public:
		LoopTakeParams() :
			base::JammableParams(
				base::GuiElementParams(0, DrawableParams{ "" },
					MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
					SizeableParams{ 1,1 },
					"",
					"",
					"",
					{})
				),
			Id(""),
			FadeSamps(constants::DefaultFadeSamps),
			Loops({})
		{
		}

		LoopTakeParams(base::JammableParams params,
			std::vector<LoopParams> loops) :
			base::JammableParams(params),
			Id(""),
			FadeSamps(constants::DefaultFadeSamps),
			Loops(loops)
		{
		}

	public:
		std::string Id;
		unsigned int FadeSamps;
		std::vector<LoopParams> Loops;
	};

	class LoopTake :
		public base::Jammable,
		public base::MultiAudioSink
	{
	public:
		enum LoopTakeSource
		{
			SOURCE_ADC,
			SOURCE_STATION,
			SOURCE_LOOPTAKE
		};

		enum LoopTakeState
		{
			STATE_INACTIVE,
			STATE_RECORDING,
			STATE_PLAYINGRECORDING,
			STATE_PLAYING,
			STATE_OVERDUBBING,
			STATE_PUNCHEDIN,
			STATE_OVERDUBBINGRECORDING
		};

	public:
		LoopTake(LoopTakeParams params,
			audio::AudioMixerParams mixerParams);
		~LoopTake();

		// Copy
		LoopTake(const LoopTake&) = delete;
		LoopTake& operator=(const LoopTake&) = delete;

	public:
		static std::optional<std::shared_ptr<LoopTake>> FromFile(LoopTakeParams takeParams,
			io::JamFile::LoopTake takeStruct,
			std::wstring dir);
		static audio::AudioMixerParams GetMixerParams(utils::Size2d loopSize,
			audio::BehaviourParams behaviour);

		virtual std::string ClassName() const override { return "LoopTake"; }
		virtual MultiAudioPlugType MultiAudioPlug() const override { return MULTIAUDIOPLUG_BOTH; }
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		virtual unsigned int NumInputChannels(Audible::AudioSourceType) const override;
		virtual unsigned int NumOutputChannels(Audible::AudioSourceType) const override;
		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source) override;
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<Trigger> trigger,
			int indexOffset,
			unsigned int numSamps);
		virtual void EndMultiPlay(unsigned int numSamps) override;
		virtual bool IsArmed() const override;
		virtual void EndMultiWrite(unsigned int numSamps,
			bool updateIndex,
			Audible::AudioSourceType source) override;
		virtual void SetSelectDepth(base::SelectDepth depth) override;
		actions::ActionResult BeginMidiQuantisationGesture(actions::TouchAction action);
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		virtual actions::ActionResult OnAction(actions::GuiAction action) override;
		virtual actions::ActionResult OnAction(actions::JobAction action) override;
		virtual bool Select() override;
		virtual bool DeSelect() override;
		virtual bool Mute() override;
		virtual bool UnMute() override;
		virtual void SetPickingFromState(EditMode mode, bool flipState) override;
		virtual void SetStateFromPicking(EditMode mode, bool flipState) override;

		std::string Id() const;
		std::string SourceId() const;
		LoopTakeSource TakeSourceType() const;
		const std::vector<std::shared_ptr<Loop>>& GetLoops() const { return _loops; }
		LoopTakeState TakeState() const;
		unsigned long NumRecordedSamps() const;
		unsigned long VisualLoopLengthSamps() const noexcept;
		double LoopIndexFrac() const noexcept;
		float VisualRadius() const noexcept;
		std::optional<QuantisationLoopTakeVisual> QuantisationVisual() const noexcept;
		static std::vector<QuantisationLoopTakeVisual> QuantisationVisualsFor(
			const std::vector<std::shared_ptr<LoopTake>>& takes);
		std::shared_ptr<Loop> AddLoop(unsigned int chan, std::string stationName);
		void AddLoop(std::shared_ptr<Loop> loop);
		void SetMixerLevel(unsigned int chan, double level);
		void SetParentVisualScale(float scale) noexcept;
		void SetupBuffers(unsigned int bufSize);
		void SetNumBusChannels(unsigned int chans);
		unsigned int NumBusChannels() const;
		// Staging only — actual load/unload happens on the job thread after
		// CommitChanges() queues the appropriate JOB_LOADVST / JOB_UNLOADVST job.
		void LoadVstPlugin(std::wstring path);
		void UnloadVstPlugin(size_t index);
		void SetSampleRate(float sampleRate);
		float GetSampleRate() const noexcept { return _sampleRate; }
		unsigned int GetLastBufSize() const noexcept { return _lastBufSize; }
		std::shared_ptr<vst::IVstPlugin> GetVstPlugin(size_t index) const;
		std::vector<io::JamFile::VstEntry> VstEntries() const;

		void Record(std::vector<unsigned int> channels,
			std::string stationName,
			std::vector<unsigned int> midiChannels = {},
			std::vector<std::string> midiDevices = {},
			// Snapshot of currently held live MIDI, keyed by device name.
			// Empty device name means "device-agnostic / any source".
			std::vector<std::pair<std::string, midi::MidiNoteSnapshot>> heldAtStart = {});
		void Play(unsigned long index,
			unsigned long loopLength,
			unsigned int endRecordSamps,
			int midiQuantisationErrorSamps = 0);
		void EndRecording();
		void Ditch();
		void Overdub(std::vector<unsigned int> channels,
			std::string stationName,
			std::vector<unsigned int> midiChannels = {},
			std::vector<std::string> midiDevices = {},
			std::shared_ptr<LoopTake> sourceTake = nullptr);
		void PunchIn(bool applyAudio = true, bool applyMidi = true);
		void PunchOut(bool applyAudio = true, bool applyMidi = true);
		bool IsPunchInActive() const noexcept { return _isPunchInActive.load(std::memory_order_relaxed); }

		bool RecordMidiEvent(const midi::MidiEvent& ev, std::uint32_t globalSampleNow) noexcept;
		bool RecordMidiEvent(const midi::MidiEvent& ev,
			const std::string& device,
			std::uint32_t globalSampleNow) noexcept;
		std::vector<midi::MidiEvent> BuildMidiPunchInLiveTransitionEvents(std::uint32_t punchSample) const;
		std::vector<midi::MidiEvent> BuildMidiPunchOutLiveTransitionEvents(std::uint32_t punchSample) const;
		unsigned int ReadMidiBlock(std::uint32_t globalSample,
			std::uint32_t numSamples,
			midi::IMidiOutputSink& sink,
			unsigned int firstOutputIndex = 0u) noexcept;
		// Read-only view of MIDI loops; used by Station to flush held notes on ditch.
		const std::vector<std::shared_ptr<midi::MidiLoop>>& GetMidiLoops() const noexcept { return _midiLoops; }
		const std::vector<unsigned int>& MidiLoopChannels() const noexcept { return _midiLoopChannels; }
		const std::vector<std::string>& MidiLoopDevices() const noexcept { return _midiLoopDevices; }
		static std::uint32_t ResolveMidiRecordSample(std::uint32_t eventGlobalSample,
			std::uint32_t globalSampleNow,
			std::uint32_t recordedSampleCount) noexcept;

		// Per-LoopTake non-destructive MIDI start-time quantisation. Propagated to
		// every owned midi::MidiLoop. Underlying recorded events are never modified;
		// disabling restores original timing exactly.
		void SetMidiQuantisation(const midi::MidiQuantisationSettings& settings) noexcept;
		midi::MidiQuantisationSettings MidiQuantisation() const noexcept;
		void SetRackVisibility(bool visible);
		gui::GuiRackParams::RackState GetRackState() const;
		void CollapseRackToMaster();
		void CollapseRouterToChannels();

	protected:
		static unsigned int _CalcLoopHeight(unsigned int takeHeight, unsigned int numLoops);

		virtual void _InitReceivers() override;
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual std::vector<actions::JobAction> _CommitChanges() override;
		virtual const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
			base::AudioSource::AudioSourceType source) override;
		virtual void _ArrangeChildren() override;

		struct AudioState
		{
			// weak_ptr: AudioState destruction on any thread won't trigger GL destructors
			std::vector<std::weak_ptr<Loop>> Loops;
			std::vector<std::shared_ptr<audio::AudioMixer>> AudioMixers;
			std::vector<std::shared_ptr<audio::AudioBuffer>> AudioBuffers;
			std::vector<float> VstBlockScratch;
			std::vector<float*> VstBlockPtrs;
		};

		gui::GuiRackParams _GetRackParams(utils::Size2d size);
		void _UpdateLoops();
		void _UpdateMidiModels(bool force = false);
		void _UpdateMidiModelRotation();
		void _RemoveMidiModelChildren();
		void _WireVuSliders();
		void _PublishAudioState();
		std::shared_ptr<const AudioState> _AudioStateSnapshot() const;
		void _ResizeVstScratch(unsigned int channelCount);
		void _LogMidiQuantisationFractionChange(midi::MidiQuantisationFraction previous,
			midi::MidiQuantisationFraction updated,
			const char* source) const;
		void _ResetMidiOverdubSession() noexcept;
		void _InitMidiOverdubSession(std::shared_ptr<LoopTake> sourceTake);
		std::size_t _BuildMidiOverdubMergedEvents(std::size_t loopIndex,
			std::uint32_t targetLoopLength,
			bool includeOpenPunchWindow) noexcept;
		void _RefreshMidiOverdubPreview(std::uint32_t displayLength) noexcept;
		void _OpenMidiPunchWindow(std::uint32_t punchSample) noexcept;
		void _CloseMidiPunchWindow(std::uint32_t punchSample, bool synthNoteOffs) noexcept;
		bool _AppendMidiOverdubLiveEvent(std::size_t loopIndex, const midi::MidiEvent& ev) noexcept;
		void _FinalizeMidiOverdubLoop(std::size_t loopIndex, std::uint32_t targetLoopLength);
		void _ApplyMidiQuantisationGesture(midi::MidiQuantisationGesture gesture,
			midi::MidiQuantisationFraction fraction,
			const char* source) noexcept;
		midi::MidiQuantisationGrainCandidates _MidiQuantisationGrainCandidates() const noexcept;
		midi::MidiNoteSnapshot _SnapshotSourceMidiAtSample(std::size_t loopIndex, std::uint32_t targetSample) const noexcept;
		midi::MidiNoteSnapshot _SnapshotLiveMidiState(std::size_t loopIndex) const noexcept;
		void _SnapshotPunchBoundaryMidi(std::uint32_t punchSample,
			midi::MidiNoteSnapshot& sourceSnapshot,
			midi::MidiNoteSnapshot& liveSnapshot) const noexcept;
		std::vector<midi::MidiEvent> _BuildMidiLiveTransitionEvents(std::uint32_t punchSample,
			bool isPunchInTransition) const;
		std::size_t _PruneSharedPunchStartTransitions(std::size_t loopIndex,
			const std::array<midi::MidiPunchWindow, 128u>& windows,
			std::size_t windowCount,
			std::uint32_t targetLoopLength,
			midi::MidiEvent* events,
			std::size_t eventCount) const noexcept;

	protected:
		static const utils::Size2d _Gap;
		static const utils::Size2d _ToggleSize;
		static const utils::Size2d _ToggleGap;

		bool _flipLoopBuffer;
		bool _loopsNeedUpdating;
		bool _endRecordingCompleted;
		std::atomic<LoopTakeState> _state;
		std::string _id;
		std::string _sourceId;
		unsigned int _lastBufSize;
		unsigned int _fadeSamps;
		LoopTakeSource _sourceType;
		std::atomic<unsigned long> _recordedSampCount;
		unsigned int _endRecordSampCount;
		unsigned int _endRecordSamps;
		unsigned long _midiVisualPlayIndex;
		unsigned long _midiVisualLoopLength;
		std::atomic<bool> _isPunchInActive;
		std::atomic<bool> _isMidiPunchInActive;
		std::shared_ptr<gui::GuiRack> _guiRack;
		std::shared_ptr<audio::AudioMixer> _masterMixer;
		float _parentVisualScale = 1.0f;
		std::vector<std::shared_ptr<Loop>> _loops;
		std::vector<std::shared_ptr<Loop>> _backLoops;
		std::vector<std::shared_ptr<midi::MidiLoop>> _midiLoops;
		std::vector<unsigned int> _midiLoopChannels;
		std::vector<std::string> _midiLoopDevices;
		std::vector<midi::MidiNoteSnapshot> _midiRecordHeld;
		midi::MidiOverdubSession _midiOverdubSession;
		std::atomic<std::uint64_t> _midiQuantisationPacked;
		bool _midiQuantisationUpdatePending;
		std::vector<std::shared_ptr<audio::AudioMixer>> _audioMixers;
		std::vector<std::shared_ptr<audio::AudioMixer>> _backAudioMixers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _audioBuffers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _backAudioBuffers;
		std::atomic<std::shared_ptr<const AudioState>> _audioState;
		// Live VST chain published atomically for lock-free audio-thread reads.
		std::atomic<std::shared_ptr<vst::VstChain>> _vstChain;
		std::shared_ptr<vst::VstChain> _backVstChain;
		std::atomic<bool> _flipVstChain{ false };
		std::vector<std::wstring> _pendingVstLoads;
		std::vector<size_t> _pendingVstUnloads;
		float _sampleRate{ static_cast<float>(constants::DefaultSampleRate) };
		// _vstPluginPaths: written on job thread (OnAction), read on main thread (VstEntries).
		// Access is guarded by _vstPathsMutex in both directions.
		mutable std::mutex _vstPathsMutex;
		std::vector<std::wstring> _vstPluginPaths;
		std::vector<float> _vstBlockScratch;
		std::vector<float*> _vstBlockPtrs;
	};
}
