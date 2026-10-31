#pragma once
#include "../vst/VstRackSlot.h"
#include "../audio/InputHistory.h"

// Recorded performance layer within a Station; owns take state, MIDI material,
// timing anchors, effects, and its Loops without owning remote follow policy.

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include <memory>
#include "Loop.h"
#include "TriggerCaptureTiming.h"
#include "CameraRecordingObservation.h"
#include "../graphics/CameraBounds.h"
#include "../graphics/StationModel.h"
#include "../engine/Quantiser.h"
#include "../midi/MidiLoop.h"
#include "../midi/MidiOverdub.h"
#include "Jammable.h"
#include "ActionUndo.h"
#include "../audio/AudioMixer.h"
#include "../audio/AudioBuffer.h"
#include "../gui/GuiRack.h"
#include "../io/JamFile.h"
#include "../vst/VstChain.h"
#include "../vst/RackMove.h"
#include "../base/TriggerPunchTarget.h"
#include "../base/PublishedAudioSnapshot.h"

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
		public base::MultiAudioSink,
		public base::TriggerPunchTarget
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

		enum class TimingCorrectionReason : std::uint8_t
		{
			TempoReplacement,
			JoinAlignment,
			PhaseDiscipline,
			Invalidation
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
		void InitResources(resources::ResourceLib& resourceLib, bool forceInit = false) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		virtual unsigned int NumInputChannels(Audible::AudioSourceType) const override;
		virtual unsigned int NumOutputChannels(Audible::AudioSourceType) const override;
		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source) override;
		virtual void OnBlockWriteChannel(unsigned int channel,
			const base::AudioWriteRequest& request,
			int writeOffset) override;
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<base::BounceWriter> bounceWriter,
			int indexOffset,
			unsigned int numSamps, const audio::DacRenderContext* dac = nullptr);
		void SetActiveBounce(std::shared_ptr<LoopTake> sourceTake,
			std::shared_ptr<base::BounceWriter> writer) noexcept;
		void ProcessActiveBounce(int sourceOffset, unsigned int numSamps);
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
		// Audio-boundary group application changes persistent state only. It never
		// propagates to new leaves or alters recording-owned source-mute controls.
		void SetGroupMutedAudio(bool muted) noexcept
		{
			if (muted) Tweakable::Mute(); else Tweakable::UnMute();
		}
		void SetGroupSelected(bool selected)
		{
			if (selected) GuiElement::Select(); else GuiElement::DeSelect();
		}
		virtual void SetPickingFromState(EditMode mode, bool flipState) override;
		virtual void SetStateFromPicking(EditMode mode, bool flipState) override;

		std::string Id() const;
		std::string SourceId() const;
		LoopTakeSource TakeSourceType() const;
		const std::vector<std::shared_ptr<Loop>>& GetLoops() const { return _loops; }
		// Owner/job-side restore lookup includes material awaiting ordinary publication.
		std::shared_ptr<Loop> FindStagedAudioLoopById(const std::string& id) const;
		// Owner/job-side restoration admission, before staged audio publication.
		bool HasRestorableLoopMaterial() const noexcept { return !_backLoops.empty() || !_midiLoops.empty(); }
		std::vector<std::vector<unsigned long>> SnapshotAudioRoutesForExport() const;
		double MasterLevelForExport() const;
		std::vector<double> BusLevelsForExport() const;
		bool RestoreMixerLevels(double masterLevel, const std::vector<double>& busLevels);
		bool RestoreAudioRoutes(const std::vector<std::vector<unsigned long>>& routes);
		LoopTakeState TakeState() const;
		unsigned int FrozenAudioSourceAgeSamps() const noexcept override { return _triggerCaptureConfigured ? _triggerCaptureTiming.SourceAgeSamps : 0u; }
		bool HasTriggerAudioCapture() const noexcept override { return !_backLoops.empty(); }
		bool HasPendingTriggerCapture() const noexcept override
		{
			const auto state = _state.load(std::memory_order_acquire);
			return !_captureCancelled.load(std::memory_order_acquire) && ((state != STATE_INACTIVE && state != STATE_PLAYING) || MidiTriggerCapturePending());
		}
		bool IsCaptureCancelled() const noexcept
			{ return _captureCancelled.load(std::memory_order_acquire); }
		bool IsTriggerCaptureInactive() const noexcept override
			{ return _captureCancelled.load(std::memory_order_acquire); }
		void AcquireTriggerSourceMuteAudio() noexcept override
			{ _captureSourceMuteControl->Acquire(); }
		void ReleaseTriggerSourceMuteAudio() noexcept override
			{ _captureSourceMuteControl->Release(); }
		bool IsMuted() const override
			{ return Tweakable::IsMuted() || _captureSourceMuteControl->IsMuted(); }
		// Structural owner only; retains a control handle, never the source take.
		void AcquireReplacementSourceMute(const std::shared_ptr<LoopTake>& source) noexcept;
		unsigned long NumRecordedSamps() const;
		unsigned long VisualLoopLengthSamps() const noexcept;
		unsigned long MidiPlayIndex() const noexcept
			{ return _midiVisualPlayIndex.load(std::memory_order_relaxed); }
		unsigned long MidiLoopLengthSamps() const noexcept
			{ return _midiVisualLoopLength.load(std::memory_order_relaxed); }
		struct MidiMappedRestoreDiagnostic
		{
			unsigned long BeforeSamps = 0ul;
			unsigned long TargetSamps = 0ul;
			std::uint64_t Count = 0u;
			std::uint64_t ChangedCount = 0u;
			unsigned int LargestBackstepSamps = 0u;
		};
		MidiMappedRestoreDiagnostic MappedMidiRestoreDiagnostic() const noexcept
		{
			const auto pair = _mappedMidiRestorePair.load(std::memory_order_relaxed);
			return { static_cast<unsigned long>(pair >> 32u), static_cast<unsigned long>(pair & 0xffffffffu),
				_mappedMidiRestoreCount.load(std::memory_order_relaxed),
				_mappedMidiRestoreChangedCount.load(std::memory_order_relaxed),
				_mappedMidiLargestBackstepSamps.load(std::memory_order_relaxed) };
		}
		// Accumulated signed transport correction for MIDI loop phase anchors.
		std::int32_t MidiAnchorCorrection() const noexcept
			{ return _midiAnchorCorrection.load(std::memory_order_relaxed); }
		const std::atomic<std::int32_t>* MidiAnchorCorrectionPtr() const noexcept
			{ return &_midiAnchorCorrection; }
		double LoopIndexFrac() const noexcept;
		// Callback-only: resolve a promoted master from the published audio loops,
		// falling back to this take's MIDI cursor when it has no audio loop.
		void CompleteLocalMasterQuantisationOnOwner() noexcept;
		void CancelLocalMasterQuantisationOnOwner() noexcept;
		bool PrepareLocalMasterQuantisation(unsigned int grainSamps,
			std::uint64_t rebasedTransportStartSamps) noexcept;
		bool TryClaimPreparedMidiQuantisationAtAudioBoundary() noexcept;
		void ReleasePreparedMidiQuantisationClaimAtAudioBoundary() noexcept;
		void ActivatePreparedMidiQuantisationAtAudioBoundary() noexcept;
		std::optional<std::uint32_t> MasterPhaseAtAudioBoundary(
			const Loop* targetLoop, std::uint32_t masterLength) const noexcept;
		float VisualRadius() const noexcept;
		// Presentation accessors: non-audio callers hold Scene::_sceneMutex.
		bool HasCameraContent() const;
		graphics::CameraBounds CameraBoundsInParent(float parentVisualScale = 1.0f) const;
		std::optional<engine::QuantisationLoopTakeVisual> QuantisationVisual() const noexcept;
		static std::vector<engine::QuantisationLoopTakeVisual> QuantisationVisualsFor(
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
		std::string LoadVstPlugin(std::wstring path,
			std::vector<std::uint8_t> initialState = {});
		// Startup-only synchronous counterpart used by JAM reconstruction.
		bool LoadVstPluginSynchronously(const std::wstring& path,
			const std::vector<std::uint8_t>& initialState = {},
			bool bypass = false, const std::string& stateId = {});
		void UnloadVstPlugin(size_t index);
		vst::VstRackSlot GetVstRackSlot() const;
		bool LoadManagedVstPlugin(std::wstring path, const std::string& identity = {});
		bool UnloadManagedVstPlugin(const std::string& identity);
		void ForceUnloadAllVstPlugins();
		void SetSampleRate(float sampleRate);
		bool ReconfigureVstFormat(float sampleRate, unsigned int blockSize);
		float GetSampleRate() const noexcept { return _sampleRate; }
		unsigned int GetLastBufSize() const noexcept { return _lastBufSize; }
		std::shared_ptr<vst::IVstPlugin> GetVstPlugin(size_t index) const;
		std::vector<vst::VstRackSlot> GetVstRackSlots(std::size_t offset = 0u) const;
		std::uint64_t RackRevision() const noexcept { return _rackRevision.load(std::memory_order_acquire); }
		bool MoveVstSlot(const vst::RackMove& move);
		void SetAutomationTopologyChangedCallback(std::function<void()> changed);
		std::shared_ptr<vst::IVstPlugin> FindVstPlugin(const vst::IVstPlugin* observer,
			const std::string& identity, std::size_t& index) const
		{
			const auto chain = _vstChain.load(std::memory_order_acquire);
			return chain ? chain->FindPlugin(observer, identity, index) : nullptr;
		}
		std::vector<io::JamFile::VstEntry> VstEntries() const;
		bool TryGroupVstEntries(std::vector<io::JamFile::VstEntry>& output) const;
		bool TryRestoreGroupVstEntry(const io::JamFile::VstEntry& entry);

		// True if plugin is hosted by this take's VST chain. Identity comparison
		// only; non-RT (reads the published chain). Used to resolve the owner of
		// an editor-driven automation event.
		bool OwnsPlugin(const vst::IVstPlugin* plugin) const noexcept
		{
			auto chain = _vstChain.load(std::memory_order_acquire);
			return chain && chain->ContainsPlugin(plugin);
		}

		// Read-only, real-time-safe view of this take's aggregate VST
		// processing latency, in samples (sum across the published chain).
		// Plumb-only for now: not yet folded into any playback-position
		// compensation -- see doc/ninjam-live-loop-latency-sync-planC.md §2/§7
		// and IVstPlugin::GetLatencySamples.
		void PollPlaybackPluginChanges() const noexcept;
		unsigned int RecordedPlaybackLatencySamps() const noexcept;
		bool CanConfigureRecordedPlaybackCompensation(unsigned int referenceLatencySamps,
			unsigned int stationLatencySamps) const noexcept;
		bool ConfigureRecordedPlaybackCompensation(unsigned int referenceLatencySamps,
			unsigned int stationLatencySamps);
		int CurrentVstLatencySamps() const noexcept
		{
			auto chain = _vstChain.borrow();
			return chain ? chain->GetLatencySamples() : 0;
		}

		void Record(std::vector<unsigned int> channels,
			std::string stationName,
			std::vector<unsigned int> midiChannels = {},
			std::vector<std::string> midiDevices = {},
			// Snapshot of currently held live MIDI, keyed by device name.
			// Empty device name means "device-agnostic / any source".
			std::vector<std::pair<std::string, midi::MidiNoteSnapshot>> heldAtStart = {},
			std::uint64_t transportStartSamps = 0u,
			std::optional<std::uint32_t> midiRecordStartSample = std::nullopt);
		std::optional<std::uint32_t> MidiRecordStartSample() const noexcept
		{
			return _midiRecordStartSample;
		}
        bool AdmitTriggerCaptureLength(unsigned long length) const noexcept;
        void SetTriggerCaptureStop(std::uint32_t stop) noexcept { _triggerCaptureStop = stop; }
        void CompleteMidiIngressPump(std::uint32_t deviceSample);
        bool MidiTriggerCapturePending() const noexcept { return _midiTriggerCapturePending.load(std::memory_order_acquire); }
		void Play(unsigned long index,
			unsigned long loopLength,
			unsigned int endRecordSamps,
			int midiQuantisationErrorSamps = 0, bool deferFirstMasterActivation = false, unsigned int outputAdvanceSamps = 0);
		bool FirstMasterActivationPending() const noexcept
			{ return _firstMasterActivationPending.load(std::memory_order_acquire); }
		bool ActivateFirstMasterMidiAtAudioBoundary(unsigned long phaseSamps, std::uint32_t recordOrigin) noexcept;
		void QueueTimingCorrection(long long deltaSamps,
			std::uint64_t generation,
			TimingCorrectionReason reason) noexcept;
		void InvalidateTimingCorrections() noexcept;
		// Applies one accepted audio-boundary correction. Session policy has
		// already been interpreted by AudioHost; this engine operation only keeps
		// its generation gate and shifts each entity by the common signed delta.
		void ApplyAcceptedTimingCorrection(long long deltaSamps,
			std::uint64_t generation, bool relocateMidi = false) noexcept;
		void CaptureSceneAnchors(std::uint64_t sceneCoordinateSamps) noexcept;
		void InvalidateSceneAnchors() noexcept;
		void ResetTimingEpoch() noexcept;
		// Capture/restore use an already-mapped common source coordinate. Entity
		// anchors and modulo lengths remain local to the take and its loops.
		void CaptureMappedSourceAnchors(std::int64_t sourceCoordinateSamps) noexcept;
		void RestoreMappedSourceCoordinate(std::int64_t sourceCoordinateSamps) noexcept;
		// Audio-thread absolute setter. The target is persistent so an empty take
		// can reconcile when it first becomes playable.
		void SetLocalTransportOffsetSamps(long long targetSamps) noexcept;
		// Non-audio initialization before this take enters an audio snapshot.
		void SetInitialLocalTransportOffsetSamps(long long targetSamps) noexcept;
		std::uint64_t QueuedExternalPhaseCorrectionCount() const noexcept
			{ return _queuedTimingCorrectionCount.load(std::memory_order_relaxed); }
		std::uint64_t ConsumedExternalPhaseCorrectionCount() const noexcept
			{ return _consumedTimingCorrectionCount.load(std::memory_order_relaxed); }
		enum class PresentationMode : std::uint8_t { Inactive, Record, Overdub, Punch, Tail };
		struct Presentation { PresentationMode Mode; std::uint64_t Serial; };
		Presentation GetPresentation() const noexcept;
		void SetCameraRecordingPublisher(std::weak_ptr<CameraRecordingPublisher> publisher) noexcept
		{
			const auto owner = publisher.lock();
			_cameraRecordingGeneration = owner ? owner->Snapshot()->SceneGeneration : 0u;
			_cameraRecordingPublisher = std::move(publisher);
		}
		void PublishCameraLayerStart();
		void PublishCameraCaptureObservation() { _PublishCameraCapture(); }
		void SetPresentation(PresentationMode mode, std::uint64_t serial) noexcept;
		void EndRecording();
		void ConfigureTriggerCapture(const TriggerCaptureTiming& timing) noexcept;
		const TriggerCaptureTiming& CaptureTiming() const noexcept { return _triggerCaptureTiming; }
        bool HasMidiDevicePhaseObservation() const noexcept { return _hasMidiDevicePhaseObservation.load(std::memory_order_acquire); }
		std::uint32_t CaptureBoundarySample() const noexcept { return _captureBoundarySample.load(std::memory_order_acquire); }
		void BeginCaptureAudioBoundary(std::uint64_t deviceSample, std::uint64_t sceneSample, std::uint64_t absoluteSample) noexcept;
		bool AudioCaptureReady(unsigned int count, int offset = 0) const noexcept;

		// Atomic logical cancellation; destructive Ditch requires stopped/retired readers.
		void CancelCapture(bool sourceLoss = false) noexcept;
		bool IsSourceLossCancelled() const noexcept
			{ return _sourceLossCancelled.load(std::memory_order_acquire); }
		void Ditch();
		void Overdub(std::vector<unsigned int> channels,
			std::string stationName,
			std::vector<unsigned int> midiChannels = {},
			std::vector<std::string> midiDevices = {},
			std::shared_ptr<LoopTake> sourceTake = nullptr,
			std::uint64_t transportStartSamps = 0u);
		void PunchIn(bool applyAudio = true, bool applyMidi = true, std::optional<std::uint32_t> edgeSample = std::nullopt);
		void PunchOut(bool applyAudio = true, bool applyMidi = true, std::optional<std::uint32_t> edgeSample = std::nullopt);
		// Keep MIDI capture and synthetic events on the job thread; audio transitions must stay allocation-free.
		void TriggerPunchInAudio() noexcept override;
		void TriggerPunchOutAudio() noexcept override;
		void SetTriggerSourceMutedAudio(bool muted) noexcept override;
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
		std::vector<std::shared_ptr<midi::MidiLoop>> GetMidiLoopSnapshot() const;
		// Job-side completed-loop edit boundary. Capture, finalisation and quantisation
		// share _midiCaptureMutex; the audio callback reads only the loop snapshot.
		bool PublishMidiEdit(const std::shared_ptr<midi::MidiLoop>& loop,
			const midi::MidiLoop::EditState& edit,
			std::uint64_t* acceptedRevision = nullptr);
		const std::vector<unsigned int>& MidiLoopChannels() const noexcept { return _midiLoopChannels; }
		const std::vector<std::string>& MidiLoopDevices() const noexcept { return _midiLoopDevices; }
		struct MidiStreamExport
		{
			std::string Id;
			unsigned int Channel = 0u;
			std::string Device;
			midi::MidiLoop::ExportState Loop;
		};
		struct MidiExportState
		{
			std::vector<MidiStreamExport> Streams;
			unsigned long PlayIndex = 0ul;
			unsigned long LoopLengthSamps = 0ul;
			midi::MidiQuantisationSettings Quantisation;
			std::uint32_t BaseIntervalSamps = 0u;
			std::uint32_t BaseDivisions = 0u;
			std::uint64_t QuantisationTransportStartSamps = 0u;
		};
		struct PendingAutomationBinding
		{
			std::size_t MidiStreamIndex = 0u;
			std::size_t LaneIndex = 0u;
			std::string TargetScope;
			unsigned int TargetPluginIndex = 0u;
			unsigned int TargetLoopIndex = 0u;
		};
		const std::vector<PendingAutomationBinding>& PendingAutomationBindings() const noexcept
			{ return _pendingAutomationBindings; }
		void ClearPendingAutomationBindings() noexcept { _pendingAutomationBindings.clear(); }
		static constexpr std::size_t MaxMidiStreamsForRestore = io::JamFile::MaxMidiStreamsPerTake;
		// Non-RT transfer at the exporter's already-paused, scene-locked boundary.
		// The per-loop origin is exported with this take's anchor correction folded
		// in, so loading can reset the live correction to zero.
		bool SnapshotMidiForExport(MidiExportState& state) const;
		// Non-RT construction before this take enters an audio snapshot.  Replaces
		// the complete MIDI stream set and publishes one immutable loop snapshot.
		bool RestoreMidiFromExport(const MidiExportState& state);
		static std::uint32_t ResolveMidiRecordSample(std::uint32_t eventGlobalSample,
			std::uint32_t globalSampleNow,
			std::uint32_t recordedSampleCount) noexcept;
		static std::uint32_t ResolveMidiRecordSampleFromTrigger(
			std::uint32_t eventGlobalSample,
			std::uint32_t triggerSample) noexcept;

		// Per-LoopTake non-destructive MIDI start-time quantisation. Propagated to
		// every owned midi::MidiLoop. Underlying recorded events are never modified;
		// disabling restores original timing exactly.
		void SetMidiQuantisation(const midi::MidiQuantisationSettings& settings) noexcept;
		void SetMidiQuantisationFromUserEdit(const midi::MidiQuantisationSettings& settings) noexcept;
		midi::MidiQuantisationSettings MidiQuantisation() const noexcept;
		midi::MidiQuantisationSettings ResolvedMidiQuantisation() const noexcept;
		midi::MidiQuantisationSettings ResolvedMidiQuantisationFor(const std::shared_ptr<midi::MidiLoop>& loop) const noexcept;
		// UI/job-side copy of current loop settings, including while recording.
		// Shares the writer lock; never call from the audio callback or under it.
		midi::MidiQuantisationSettings SnapshotMidiLoopQuantisation(const std::shared_ptr<midi::MidiLoop>& loop) const;
		void SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState state) noexcept;
		void SetMidiQuantisationInheritedPhaseOffset(std::int32_t offsetSamps) noexcept;
		void SetMidiQuantisationTransportStartSamps(std::uint64_t startSamps) noexcept;
		// Called at the first captured audio block so MIDI's local sample zero has
		// the same transport origin as the recorded sample counter.
		void CaptureMidiTransportStartAtAudioBoundary(std::uint64_t startSamps) noexcept;
		std::uint64_t MidiQuantisationTransportStartSamps() const noexcept;
		void CaptureFirstRecordBlockSceneAtAudioBoundary(std::uint64_t sceneSamps) noexcept;
		std::uint64_t FirstRecordBlockSceneSamps() const noexcept;
		bool SetMidiLoopQuantisationOverride(const std::shared_ptr<midi::MidiLoop>& loop, const midi::MidiLoop::QuantisationOverride& value);
		void SetMidiBaseGrid(std::uint32_t interval, std::uint32_t divisions) noexcept;
		bool IsCompletedRecording() const noexcept;
		void SetRemoteMidiQuantisationGrid(const RemoteTransportGeometry& geometry,
			std::int64_t originSamps) noexcept;
		void SetRackVisibility(bool visible);
		std::shared_ptr<gui::GuiRack> GetGuiRack() const { return _guiRack; }
		gui::GuiRackParams::RackState GetRackState() const;
		void CollapseRackToMaster();
		void CollapseRouterToChannels();

	protected:
		static unsigned int _CalcLoopHeight(unsigned int takeHeight, unsigned int numLoops);
		static std::int32_t _ClampPhaseOffset(std::int64_t offsetSamps) noexcept;

		virtual void _InitReceivers() override;
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		void _ReleaseResources() override;
		virtual std::vector<actions::JobAction> _CommitChanges() override;
		virtual const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
			base::AudioSource::AudioSourceType source) override;
		virtual void _ArrangeChildren() override;

		struct AudioState
		{
			// Snapshot pins keep retired loops alive until the job thread reclaims the state.
			std::vector<std::shared_ptr<Loop>> Loops;
			std::vector<unsigned int> RecordInputChannels;
            bool OrdinaryAudioCapture = false;
			std::vector<std::shared_ptr<audio::AudioMixer>> AudioMixers;
			std::vector<std::shared_ptr<audio::AudioBuffer>> AudioBuffers;
			std::vector<float> VstBlockScratch;
			std::vector<float*> VstBlockPtrs;
			std::vector<float> InstrumentScratch;
			std::vector<float*> InstrumentPtrs;
		};

		using MidiLoopSnapshot = std::vector<std::shared_ptr<midi::MidiLoop>>;

		gui::GuiRackParams _GetRackParams(utils::Size2d size);
		bool _RecordMidiEventUnlocked(const midi::MidiEvent& ev,
			const std::string& device,
			std::uint32_t globalSampleNow) noexcept;
		std::optional<bool> _ForcedMidiQuantisationEnabled() const noexcept;
		void _UpdateLoops();
		void _UpdateMidiModels(bool force = false);
		void _UpdateMidiModelRotation();
		double _MidiLoopIndexFrac() const noexcept;
		void _RemoveMidiModelChildren();
		void _WireVuSliders();
		midi::MidiQuantisationSettings _ResolvedMidiQuantisation(
			midi::MidiQuantisationSettings settings) const noexcept;
		void _PublishMidiLoopSnapshot();
		std::shared_ptr<const MidiLoopSnapshot> _MidiLoopSnapshotState() const;
		base::PublishedAudioSnapshot<const MidiLoopSnapshot>::Lease _BorrowMidiLoopSnapshot() const noexcept;
		void _PublishAudioState();
		std::shared_ptr<const AudioState> _AudioStateSnapshot() const;
		base::PublishedAudioSnapshot<const AudioState>::Lease _BorrowAudioState() const noexcept;
		const std::shared_ptr<base::AudioSink> _InputChannelFromState(const AudioState& state,
			unsigned int channel,
			base::AudioSource::AudioSourceType source) const;

	private:
		void _ApplyAudioPhaseOffset() noexcept;
		static bool _HasMidiQuantisationGestureModifiers(base::Action::Modifiers modifiers) noexcept;
		static unsigned long _NormalizeLoopIndex(long long index, unsigned long loopLength) noexcept;
		static std::uint32_t _NormalizeMidiLoopOffset(std::uint32_t offset,
			std::uint32_t loopLength) noexcept;
		static unsigned long _InitialMidiPlayIndex(unsigned long loopLength,
			int midiQuantisationErrorSamps) noexcept;
		static bool _AppendMidiEvent(const midi::MidiEvent& event,
			midi::MidiEvent* outEvents,
			std::size_t outCapacity,
			std::size_t& outCount) noexcept;
		static std::size_t _BuildRebasedMidiOverdubSourceEvents(const midi::MidiOverdubLoopState& state,
			midi::MidiEvent* outEvents,
			std::size_t outCapacity) noexcept;
		static void _DrainVstChain(std::shared_ptr<vst::VstChain> chain);

	protected:
		void _ResizeVstScratch(unsigned int channelCount);
		bool _ShiftDirectPlaybackCursors(long long deltaSamps) noexcept;
		void _RequestMidiRelocationChase() noexcept;
		void _ClearRelocatedRecordedPlaybackDelay(long long deltaSamps) noexcept;
		void _TryApplyLocalTransportOffset() noexcept;
		static long long _OffsetDelta(long long targetSamps, long long appliedSamps) noexcept;
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
		std::atomic<bool> _loopsNeedUpdating;
		std::atomic<std::uint64_t> _captureGeneration{ 1u };
		std::atomic<std::uint64_t> _completionPublishedGeneration{ 0u };
		std::atomic<std::uint64_t> _pendingCompletionGeneration{ 0u };
		std::atomic<std::uint64_t> _presentation{ 0u };
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
		void _PublishRecordingCompletion(std::uint64_t generation) noexcept;
		void _InvalidateRecordingCompletion() noexcept;
		void _SetPresentationMode(PresentationMode mode) noexcept;
		void _PublishCameraCapture(bool newLayer = false);
		// Structural owners under Scene::_sceneMutex only; never touched by audio.
		std::weak_ptr<CameraRecordingPublisher> _cameraRecordingPublisher;
		std::uint64_t _cameraRecordingGeneration = 0u;
		std::uint64_t _cameraLayerStartOrder = 0u;
		std::uint64_t _cameraObservedAudioPunchRevision = 0u;
		// Audio is the sole writer of this bounded odd/even revision. Job/UI
		// observes it off callback; no ownership, clocks or camera calls cross back.
		// Lifetime is this take, pinned by the existing callback snapshot/session.
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
		std::atomic<std::uint64_t> _cameraAudioPunchRevision{ 0u };
        std::atomic<std::int64_t> _midiCaptureTransportOrigin{0};
        std::atomic<bool> _hasMidiDevicePhaseObservation{false};
        std::atomic<std::uint64_t> _midiDevicePhaseObservation{0u}; // callback publishes coherent device sample + phase
        unsigned long _MidiPhaseAtDeviceSample(std::uint32_t sample, unsigned long length) const noexcept;
        std::uint32_t _MidiPhysicalCaptureOffset(std::uint32_t sample) const noexcept;
		bool _ordinaryAudioCapture = false; // structural owner, published in AudioState
        TriggerCaptureTiming _triggerCaptureTiming;
		bool _triggerCaptureConfigured = false; // immutable after structural publication
		std::atomic<std::uint32_t> _captureBoundarySample{ 0u };
		std::atomic<bool> _audioCaptureAdmitted{ true };
		std::atomic<bool> _capturePlaybackPending{ false };
        std::atomic<bool> _capturePlaybackPrepared{ false };
		std::atomic<unsigned int> _captureOutputAdvance{ 0u };
		bool _captureBoundaryStarted = false; // callback owner
		std::uint32_t _captureFallbackOrigin = 0u; // callback owner
		std::atomic<LoopTakeState> _state;
		// INACTIVE also describes a newly constructed/restored take. Cancellation
		// alone suppresses retained callback snapshots and queued lifecycle work.
		std::atomic<bool> _captureCancelled{ false };
		// Sticky for a retired take: old Station snapshots must never re-emit its MIDI.
		std::atomic<bool> _sourceLossCancelled{ false };
		std::weak_ptr<LoopTake> _activeBounceSource;
		std::shared_ptr<base::BounceWriter> _activeBounceWriter;
		std::string _id;
		std::string _sourceId;
		unsigned int _lastBufSize;
		unsigned int _fadeSamps;
		LoopTakeSource _sourceType;
		std::atomic<unsigned long> _recordedSampCount;
		std::atomic<unsigned int> _endRecordSampCount;
		std::atomic<unsigned int> _endRecordSamps;
		// Cross-thread cursor: incremented on the audio thread and read by UI/control code.
		std::atomic<bool> _localMasterQuantisationPrepared{ false };
		std::atomic<bool> _firstMasterActivationPending{ false };
		std::atomic<std::uint64_t> _preparedLocalMasterBaseGridPacked{ 0u };
		std::atomic<std::uint64_t> _preparedLocalMasterQuantisationPacked{ 0u };
		std::atomic<std::uint64_t> _preparedLocalMasterTransportStart{ 0u };
		std::atomic<unsigned long> _midiVisualPlayIndex;
		std::atomic<unsigned long> _midiVisualLoopLength;
		// Diagnostic scalars only: audio writer, job readers; lifetime is pinned by
		// the published take snapshot. The before/target pair is one 64-bit read.
		std::atomic<std::uint64_t> _mappedMidiRestorePair{ 0u };
		std::atomic<std::uint64_t> _mappedMidiRestoreCount{ 0u };
		std::atomic<std::uint64_t> _mappedMidiRestoreChangedCount{ 0u };
		std::atomic<unsigned int> _mappedMidiLargestBackstepSamps{ 0u };
		std::atomic<std::int32_t> _midiAnchorCorrection{ 0 };
		std::atomic<unsigned long> _midiSceneAnchor{ 0ul };
		std::atomic_bool _hasMidiSceneAnchor{ false };
		void _MoveMidiVisualCursor(unsigned long target, long long translationSamps) noexcept;
		// Job/UI writes occur before snapshot publication; audio reads/reconciles.
		std::atomic<long long> _desiredLocalTransportOffsetSamps{ 0 };
		long long _appliedLocalTransportOffsetSamps = 0;
		// Job thread publishes one shared signed transport delta; the audio thread
		// consumes it once after normal block advancement. Generation zero invalidates it.
		std::atomic<long long> _pendingTimingCorrectionSamps{ 0 };
		std::atomic<std::uint64_t> _timingCorrectionGeneration{ 0u };
		std::atomic<std::uint64_t> _queuedTimingCorrectionCount{ 0u };
		std::atomic<std::uint64_t> _consumedTimingCorrectionCount{ 0u };
		// Audio-thread-only generation gate for the unified audio-boundary command.
		std::uint64_t _audioTimingGeneration{ 0u };
		std::atomic<bool> _isPunchInActive;
		std::atomic<bool> _isMidiPunchInActive;
		std::shared_ptr<gui::GuiRack> _guiRack;
		std::shared_ptr<graphics::TakeCoreModel> _takeCore;
		std::shared_ptr<audio::AudioMixer> _masterMixer;
		std::shared_ptr<audio::CaptureSourceMuteControl> _captureSourceMuteControl =
			std::make_shared<audio::CaptureSourceMuteControl>();
		std::shared_ptr<audio::CaptureSourceMuteControl> _replacementSourceMuteControl;
		void _ReleaseReplacementSourceMute() noexcept;
		float _parentVisualScale = 1.0f;
		std::vector<std::shared_ptr<Loop>> _loops;
		std::vector<std::shared_ptr<Loop>> _backLoops;
		std::vector<unsigned int> _recordInputChannels;
		std::vector<std::shared_ptr<midi::MidiLoop>> _midiLoops;
		std::vector<unsigned int> _midiLoopChannels;
		std::vector<std::string> _midiLoopDevices;
		std::vector<midi::MidiNoteSnapshot> _midiRecordHeld;
		// Capture owner only, under _midiCaptureMutex; reset with recording state.
		std::vector<std::bitset<midi::MidiNote::TotalNoteSlots>> _midiRecordSeededHeld;
		// Job-thread-owned sample origin for an unseeded first MIDI take.
		std::optional<std::uint32_t> _midiRecordStartSample;
        // Capture mutex/job owner owns provisional events and close intent. Callback
        // observes only the pending scalar; cancellation uses existing take lifetime.
        std::atomic<bool> _midiTriggerCapturePending{ false };
        bool _midiCaptureCloseRequested = false;
        std::uint32_t _triggerCaptureStop = 0u;
        unsigned long _midiCaptureFinalLength = 0ul;
        unsigned long _midiCaptureFinalIndex = 0ul;
        LoopTakeState _midiCaptureFinalState = STATE_RECORDING;
        void _FinalizeMidiCapture(unsigned long length, unsigned long index, LoopTakeState& state);

		midi::MidiOverdubSession _midiOverdubSession;
		mutable std::mutex _midiCaptureMutex;
		base::PublishedAudioSnapshot<const MidiLoopSnapshot> _midiLoopSnapshot;
		std::atomic<std::uint8_t> _promotionTopologyGate{ 0u }; // 1 callback, 2 owner.
		base::PublishedAudioSnapshot<const MidiLoopSnapshot>::Lease _promotionMidiLoopLease; // Callback only.
		std::atomic<std::uint64_t> _midiQuantisationPacked;
		std::atomic<std::uint8_t> _globalMidiQuantStatePacked{
			static_cast<std::uint8_t>(io::JamFile::GlobalMidiQuantState::Off)
		};
		std::atomic<std::int32_t> _midiInheritedPhaseOffsetSamps{ 0 };
		std::atomic<std::uint64_t> _midiTransportStartSamps{ 0u };
		std::atomic<std::uint64_t> _firstRecordBlockSceneSamps{ 0u };
		// Audio callback publishes the captured start; the record-end job reads it.
		std::atomic_bool _midiTransportStartFromAudio{ false };
		// Seqlock-style publication avoids torn remote-grid reads without placing a
		// lock or allocation on any real-time path.
		// Job writer; callback/UI readers consume interval and divisions as one value.
		std::atomic<std::uint64_t> _midiBaseGridPacked{ 0u };
		std::atomic<std::uint64_t> _remoteMidiGridSequence{ 0u };
		std::atomic<std::uint32_t> _remoteMidiIntervalSamps{ 0u };
		std::atomic<std::uint32_t> _remoteMidiBpi{ 0u };
		std::atomic<std::int64_t> _remoteMidiOriginSamps{ 0 };
		std::atomic_bool _midiQuantisationUpdatePending;
		std::vector<std::shared_ptr<audio::AudioMixer>> _audioMixers;
		std::vector<std::shared_ptr<audio::AudioMixer>> _backAudioMixers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _audioBuffers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _backAudioBuffers;
		base::PublishedAudioSnapshot<const AudioState> _audioState;
		// Live VST chain published atomically for lock-free audio-thread reads.
		base::PublishedAudioSnapshot<vst::VstChain> _vstChain;
		std::shared_ptr<vst::VstChain> _backVstChain;
		std::atomic<bool> _flipVstChain{ false };
		// Non-audio membership/commit only; propagated to child loops. Weak
		// Station capture avoids retaining a removed hierarchy or touching audio.
		std::function<void()> _automationTopologyChanged;
		std::vector<actions::JobAction> _pendingVstLoads;
		std::vector<actions::JobAction> _pendingVstUnloads;
		float _sampleRate{ static_cast<float>(constants::DefaultSampleRate) };
		// _vstPluginPaths: written on job thread (OnAction), read on main thread (VstEntries).
		// Access is guarded by _vstPathsMutex in both directions.
		mutable std::mutex _vstPathsMutex;
		std::vector<std::wstring> _vstPluginPaths;
		std::vector<io::JamFile::VstEntry> _vstSavedEntries;
		// UI stages requests, job completes them. Audio never reads this counter.
		std::atomic<unsigned int> _vstJobsPending{ 0u };
		std::atomic<std::uint64_t> _rackRevision{1u};
		// Startup-only metadata. Runtime lane pointers are installed by Station only
		// after every chain named by these references has been constructed.
		std::vector<PendingAutomationBinding> _pendingAutomationBindings;
		std::vector<float> _vstBlockScratch;
		std::vector<float*> _vstBlockPtrs;
	};
}
