#pragma once
#include "../vst/RackState.h"
#include "../audio/InputHistory.h"

// Performance/mixing channel and LoopTake owner; fans neutral operations downward
// while each take and loop retains its own state, anchors, length, and phase.

#include <atomic>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>
#include "LoopTake.h"
#include "../engine/Quantiser.h"
#include "../graphics/StationModel.h"
#include "Trigger.h"
#include "AudioSink.h"
#include "../base/PublishedAudioSnapshot.h"
#include "../audio/AudioMixer.h"
#include "../audio/AudioBuffer.h"
#include "../base/Jammable.h"
#include "../gui/GuiRack.h"
#include "../io/InitFile.h"
#include "../midi/MidiDelivery.h"
#include "../midi/CaptureHistory.h"
#include "../midi/AutomationPlayback.h"
#include "../midi/MidiVstOutputSink.h"
#include "../vst/VstChain.h"

namespace engine
{
	enum class StationVisualState : std::uint8_t
	{
		STATIONSTATE_DEFAULT,
		STATIONSTATE_RECORDING,
		STATIONSTATE_ENDRECORDING,
		STATIONSTATE_PLAYING,
		STATIONSTATE_OVERDUBBING,
		STATIONSTATE_PUNCHIN
	};

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
		std::string Id;
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
		std::vector<std::vector<unsigned long>> SnapshotAudioRoutesForExport() const;
		double MasterLevelForExport() const;
		std::vector<double> BusLevelsForExport() const;
		bool RestoreMixerLevels(double masterLevel, const std::vector<double>& busLevels);
		bool RestoreAudioRoutes(const std::vector<std::vector<unsigned long>>& routes);

		virtual std::string ClassName() const override { return "Station"; }
		virtual MultiAudioPlugType MultiAudioPlug() const override { return MULTIAUDIOPLUG_BOTH; }
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		void SetClickPressed(bool pressed, bool mute = false) noexcept { if (_stationModel) _stationModel->SetClickPressed(pressed, mute); }
		// Presentation query: callers serialize take membership with the scene lock.
		bool AllTakesMuted() const;
		utils::Position3d TopCapModelPosition() const;
		// Graphics/window owner only. Engine topology and serialization use ModelPosition.
		utils::Position3d PresentationModelPosition() const;
		void BeginPresentationMove(float startX);
		bool AdvancePresentationMove(float elapsedSeconds);
		void FinishPresentationMove() noexcept;
		bool IsPresentationMoving() const noexcept { return _stationModel && _stationModel->IsPresentationMoving(); }
		// Presentation-only; caller holds Scene::_sceneMutex.
		graphics::CameraBounds CameraDeckBoundsInWorld() const;
		float CameraTakeVisualScale() const noexcept;
		virtual	utils::Position2d Position() const override;
		virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override;
		virtual unsigned int NumOutputChannels(base::Audible::AudioSourceType source) const override;
		virtual void Zero(unsigned int numSamps,
			Audible::AudioSourceType source) override;
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink> dest,
			const std::shared_ptr<Trigger> trigger,
			int indexOffset,
			unsigned int numSamps,
			std::uint32_t blockStartSample = 0u,
            const audio::DacRenderContext* dac = nullptr);
		virtual void EndMultiPlay(unsigned int numSamps) override;
		// Fans one policy-neutral accepted correction out to every local take.
		// Audio-thread only; called once at the top of the callback block.
		void ApplyFirstMasterRecordingAtAudioBoundary(utils::Timer& clock, std::uint32_t blockStartSample = 0u) noexcept;
		void BeginCaptureAudioBoundary(std::uint32_t blockStartSample, std::uint64_t sceneSample, std::uint64_t absoluteSample,
            std::optional<std::uint32_t> earliestPhysicalSample = std::nullopt) noexcept;
		void CompleteMidiIngressPump(std::uint32_t deviceSample, bool overflow = false);
		void ApplyAcceptedTimingCorrection(long long deltaSamps,
			std::uint64_t generation, bool relocateMidi = false) noexcept;
		void CaptureSceneAnchors(std::uint64_t sceneCoordinateSamps) noexcept;
		void InvalidateSceneAnchors() noexcept;
		void ResetTimingEpoch() noexcept;
		void CaptureMappedSourceAnchors(std::int64_t sourceCoordinateSamps) noexcept;
		void RestoreMappedSourceCoordinate(std::int64_t sourceCoordinateSamps) noexcept;
		void SetLocalTransportOffsetSamps(long long targetSamps) noexcept;
		virtual void OnBlockWriteChannel(unsigned int channel,
			const base::AudioWriteRequest& request,
			int writeOffset) override;
		virtual void EndMultiWrite(unsigned int numSamps,
			bool updateIndex,
			Audible::AudioSourceType source) override;
		virtual void SetSelectDepth(base::SelectDepth depth) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual actions::ActionResult OnAction(actions::GuiAction action) override;
		virtual actions::ActionResult OnAction(actions::TriggerAction action) override;
		virtual void OnTick(Time curTime,
			unsigned int samps,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) override;
		virtual void Reset() override;
		// Job/UI presentation query; never called from the audio callback.
		StationVisualState GetVisualState() const noexcept;
		
		const std::vector<std::shared_ptr<LoopTake>>& GetLoopTakes() const
		{
			return (_HasUncommittedChanges() && _flipTakeBuffer) ? _backLoopTakes : _loopTakes;
		}
		std::vector<std::shared_ptr<LoopTake>> GetLoopTakeSnapshot() const;
		void SetCameraRecordingPublisher(const std::shared_ptr<CameraRecordingPublisher>& publisher);
		std::uint64_t LoopTakeRevision() const noexcept
		{
			return _loopTakeRevision.load(std::memory_order_acquire);
		}
		// Returns true if this station receives audio from a remote ninjam user.
		// Overriding this instead of dynamic_cast keeps the audio callback path safe.
		virtual bool IsRemote() const noexcept { return false; }
		void BeginMembershipRemoval(); // Job owner, after input producer closure.
		void SetMembershipRemoving(bool removing) noexcept { _membershipRemoving.store(removing, std::memory_order_release); }
		bool MembershipRemoving() const noexcept { return _membershipRemoving.load(std::memory_order_acquire); }

		std::shared_ptr<LoopTake> AddTake();
		void AddTake(std::shared_ptr<LoopTake> take);
		// Window/job owner only while Scene::_sceneMutex serializes topology changes.
		// Audio readers retain the published AudioState until its callback lease ends.
		bool DitchTake(const std::shared_ptr<LoopTake>& take) noexcept;
		unsigned int NumTakes() const;
		std::string Name() const;
		const std::string& Id() const noexcept { return _id; }
		void SetName(std::string name);
		void SetClock(std::shared_ptr<utils::Timer> clock);
		void SetQuantisationParams(std::optional<engine::QuantisationParams> params, bool confirm = false);
		void ClearQuantisationParams();
		void SetQuantisationOverlayAlpha(float alpha) noexcept;
		void SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState state) noexcept;
		void SetRemoteMidiQuantisationGrid(const RemoteTransportGeometry& geometry,
			std::int64_t originSamps, bool resetBaseGrid);
		void SetTransportOffsetLoopFrac(double loopFrac) noexcept;
		double TransportOffsetLoopFrac() const noexcept { return _transportOffsetLoopFrac.load(std::memory_order_relaxed); }
		std::int32_t TransportOffsetSamps() const noexcept;
		void SetGlobalPhaseOffsetSamps(std::int32_t offsetSamps) noexcept;
		void SetStationPhaseOffsetSamps(std::int32_t offsetSamps) noexcept;
		std::int32_t GlobalPhaseOffsetSamps() const noexcept { return _globalPhaseOffsetSamps; }
		std::int32_t StationPhaseOffsetSamps() const noexcept { return _stationPhaseOffsetSamps; }
		void RefreshQuantisationOverlayFromClock();
		// Callback-only lookup through the retained audio take snapshot.
		bool TryClaimPreparedMidiQuantisationAtAudioBoundary() noexcept;
		void ReleasePreparedMidiQuantisationClaimAtAudioBoundary() noexcept;
		void ActivatePreparedMidiQuantisationAtAudioBoundary() noexcept;
		std::optional<std::uint32_t> MasterPhaseAtAudioBoundary(
			const LoopTake* targetTake, const Loop* targetLoop,
			std::uint32_t masterLength) const noexcept;
		void SetupBuffers(unsigned int bufSize);
		void SetSampleRate(float sampleRate);
		int CurrentVstLatencySamps() const noexcept;
		void PollPlaybackPluginChanges() const noexcept;
		unsigned int RecordedPlaybackLatencySamps() const noexcept;
		bool CanConfigureRecordedPlaybackCompensation(unsigned int referenceLatencySamps) const noexcept;
		bool ConfigureRecordedPlaybackCompensation(unsigned int referenceLatencySamps);
		// Job thread: align recorded MIDI routes to the shared DAC output reference.
		bool ConfigureRecordedMidiCompensation(unsigned int referenceLatencySamps) noexcept;
		// Control/UI owner, with job, MIDI and audio writers excluded.
		bool ReconfigureVstFormat(float sampleRate, unsigned int blockSize);
		void SetLogging(const io::LoggingConfig& config) noexcept;
		void SetNumBusChannels(unsigned int chans);
		void SetNumAdcChannels(unsigned int chans);
		void SetNumDacChannels(unsigned int chans);
		unsigned int NumBusChannels() const;
		void OnBounce(unsigned int numSamps,
			const io::UserConfig& config,
			const std::optional<audio::AudioStreamParams>& params = std::nullopt);
		// Let the UI/job thread reclaim old snapshots after callback work, keeping LoopTake destruction off audio.
		void AcknowledgeAudioBoundary() noexcept;
		// Job owner only; lets a gate-proven stopped reader acknowledge publication.
		bool HasUnacknowledgedAudioState() const noexcept;
		void ReleaseRetiredAudioStates();
		std::size_t RetiredAudioStateCount() const noexcept { return _retiredAudioStates.size(); }
		void SetRackVisibility(bool showStationRack, bool showLoopTakeRacks);
		// UI owner: follow projected take centres, with a minimum gap between racks.
		bool LayoutTakeRacks(const glm::mat4& viewProjection, utils::Size2d viewport);
		std::shared_ptr<gui::GuiRack> GetGuiRack() const { return _guiRack; }
		std::vector<io::JamFile::VstEntry> VstEntries() const;
		bool TryGroupVstEntries(std::vector<io::JamFile::VstEntry>& output) const;
		bool TryRestoreGroupVstEntry(const io::JamFile::VstEntry& entry);
		bool AcceptsLiveMidiChannel(std::uint8_t channel) const noexcept;
		void SetAllowedMidiChannels(const std::vector<int>& channels);
		const std::vector<int>& AllowedMidiChannels() const noexcept { return _allowedMidiChannels; }
		// Called only by the live MIDI dispatcher after route and channel eligibility
		// have been resolved.
		bool TryEnqueueImmediateLiveMidi(const midi::MidiEvent& event) noexcept;
		// UI lifecycle owner only, with audio callback, job and live dispatcher
		// stopped. Discards stale monitoring input; preserves recorded/playback MIDI.
		void ResetLiveMidiWhileQuiescent() noexcept;
		// Called only by the Scene job thread for generated transitions and releases.
		bool TryEnqueueSyntheticLiveMidi(const midi::MidiEvent& event) noexcept;
		// Called by the job-thread legacy MIDI path. Recording-held observation must
		// not also enqueue audible MIDI.
		void ObservePhysicalMidiForRecording(const midi::MidiEvent& event, const std::string& deviceName);
		// Emit synthetic NoteOff for currently held live notes and clear held state.
		void FlushLiveHeldMidiNotes() noexcept;
		void FlushLiveHeldMidiNotesForDevice(const std::string& deviceName) noexcept;
		void FlushPendingLostLiveMidiNotes() noexcept;
		// Replacement semantics: one MIDI output routes to at most one plugin.
		void SetMidiVstRoute(unsigned int midiOutputIndex, size_t vstIndex);
		void ClearMidiVstRoutes();
		// Non-RT persistence transfer. The audio callback continues to consume only
		// immutable, retained snapshots; these methods never mutate one in place.
		midi::MidiVstRoutingSnapshot SnapshotMidiVstRoutesForExport() const;
		bool RestoreMidiVstRoutes(const midi::MidiVstRoutingSnapshot& routes,
			size_t loadedPluginCount);

		// Startup restoration on a fresh station, before it is admitted to an audio
		// reader. Preserve unavailable/raw saved rack data for explicit recovery.
		void RestoreSavedRack(const std::vector<io::JamFile::VstEntry>& entries,
			std::array<midi::LiveVstChannelRoute, 16> routes, bool inactive);
		void RestoreLiveVstRoutesBeforePublication(std::array<midi::LiveVstChannelRoute, 16> routes);

		// VST chain management (non-RT, queued through the job thread).
		// LoadVstPlugin queues an async load; once the load completes the plugin
		// is inserted at the end of the station's effect chain.
		// initialState — optional VST2 state blob (from IVstPlugin::GetState) to
		// restore immediately after the plugin finishes loading.  Pass {} for
		// normal interactive loads where no state needs to be restored.
		void LoadVstPlugin(std::wstring path,
			std::vector<std::uint8_t> initialState = {});
		// Startup-only synchronous counterpart used before Scene::InitAudio().
		bool LoadVstPluginSynchronously(const std::wstring& path,
			const std::vector<std::uint8_t>& initialState = {},
			bool bypass = false, const std::string& stateId = {});
		void UnloadVstPlugin(size_t index);
		vst::VstRackSlot GetVstRackSlot() const;
		static constexpr std::size_t MaxRackPlugins = vst::RackState::MaxRackPlugins;
		static constexpr std::size_t MaxRackDestinations = vst::RackState::MaxRackDestinations; // Active, pending and retiring recovery owners.
		std::vector<vst::VstRackSlot> GetVstRackSlots(std::size_t offset = 0u, std::size_t count = MaxRackPlugins) const;
		std::size_t RackSlotCount() const;
		std::string RackDiagnostic() const;
		bool RecoverRack(std::uint64_t revision);
		bool RemoveInactiveRackSlot(std::size_t ordinal, std::uint64_t revision);
		bool ClearInactiveRackRoutes(std::uint64_t revision);
		bool SetLiveVstChannelRoutes(std::array<midi::LiveVstChannelRoute, 16> routes, std::uint64_t revision);
		void RefreshRackLatency(); // Existing non-RT latency pump, serialized by Scene.
		bool LoadManagedVstSlot(std::wstring path, const std::string& retryIdentity = {}, bool discardOnFailure = false);
		std::optional<std::wstring> TakeFailedInteractiveVstLoad(const std::string& identity);
		bool SetLiveVstChannelRoute(unsigned int channel, midi::LiveVstChannelRoute route, std::uint64_t revision);
		const std::array<midi::LiveVstChannelRoute, 16>& LiveVstChannelRoutes() const noexcept { return _rackState.LiveChannelRoutes(); }
		std::uint64_t RackRevision() const noexcept { return _rackState.Revision(); }
		bool RackInactive() const noexcept { return _rackState.Inactive(); }
		bool RackLatencySuspended() const noexcept { return _rackState.LatencySuspended(); }
		bool LoadManagedVstPlugin(std::wstring path);
		bool UnloadManagedVstPlugin(const std::string& identity);
		void ForceUnloadAllVstPlugins();

		// Non-RT accessor to retrieve a loaded plugin instance (or nullptr).
		std::shared_ptr<vst::IVstPlugin> GetVstPlugin(size_t index) const;
		bool OwnsPlugin(const vst::IVstPlugin* plugin) const noexcept
		{
			const auto chain = _rackState.ActiveChain();
			return chain && chain->ContainsPlugin(plugin);
		}

		// Rebuild the flat parameter-automation dispatch list from the current
		// loop set and lane wirings, then publish it atomically for the audio
		// thread. Call on the non-audio (UI/action) thread whenever automation
		// wiring changes or a recording is released.
		void RebuildAutomationDispatch();

		// Resolve the MIDI loop that should host editor-driven automation for a
		// plugin hosted by this station (station-level or take-level chain).
		// Returns the owner's last recorded MIDI loop, or nullptr when this
		// station does not host the plugin or has no recorded MIDI loop yet.
		// Non-audio (MIDI pump) thread only.
		std::shared_ptr<midi::MidiLoop> ResolveEditorAutomationLoop(const vst::IVstPlugin* plugin) const;

		// Resolve the MIDI anchor correction for any loop owned by this station.
		// Finds the owning take and returns its _midiAnchorCorrection value.
		// Non-audio thread only (iterates take snapshot).
		std::int32_t ResolveMidiAnchorCorrectionFor(const midi::MidiLoop* loop) const noexcept;

		// Called on the job thread to actually perform the load / unload.
		virtual actions::ActionResult OnAction(actions::JobAction action) override;

	protected:
		static constexpr utils::Size2d _Gap = { 5, 5 };
		static constexpr unsigned int _DefaultNumBusChannels = 8;

		static constexpr float _StationModelHeight = 470.0f;
		static constexpr float _StationModelYOffset = _StationModelHeight * 0.5f;

		static unsigned int _CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes);
		
		static unsigned int _ResolveSampleRate(std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params);
		void _TrySeedClockFromFirstLoop(const std::shared_ptr<utils::Timer>& clock,
			unsigned long loopLengthSamps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params,
			const std::string& takeId);

		virtual void _InitReceivers() override;
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		virtual std::vector<actions::JobAction> _CommitChanges() override;
		virtual const std::shared_ptr<base::AudioSink> _InputChannel(unsigned int channel,
			Audible::AudioSourceType source) override;
		virtual void _ArrangeChildren() override;

		struct AudioState
		{
			std::uint64_t Generation = 0u;
			// Retire strong ownership after the boundary acknowledgement so LoopTakes cannot die in the callback.
			std::vector<std::shared_ptr<LoopTake>> OwnedLoopTakes;
			std::vector<std::weak_ptr<LoopTake>> LoopTakes;
			std::vector<std::shared_ptr<audio::AudioMixer>> AudioMixers;
			std::vector<std::shared_ptr<audio::AudioBuffer>> AudioBuffers;
			std::vector<float> VstBlockScratch;
			unsigned int VstRenderChannels = 0u; // Prepared padded width includes retiring processors.
			std::vector<float*> VstBlockPtrs;
			std::vector<float> InstrumentScratch;
			std::vector<float*> InstrumentPtrs;
			midi::MidiDelivery::PreparedState Midi;
		};

		using LoopTakeSnapshot = std::vector<std::weak_ptr<LoopTake>>;

		void _CollapseOtherTakeRouters();
		void _CollapseOtherTakeRoutersToChannels();
		void _ApplyMidiQuantisationPhaseOffset() noexcept;
		void _PublishLoopTakeSnapshot();
		std::shared_ptr<const LoopTakeSnapshot> _LoopTakeSnapshotState() const;
		void _PublishAudioState();
		std::shared_ptr<const AudioState> _AudioStateSnapshot() const;
		base::PublishedAudioSnapshot<const AudioState>::Lease _BorrowAudioState() const noexcept;

		gui::GuiRackParams _GetRackParams(utils::Size2d size);
		std::optional<std::shared_ptr<LoopTake>> _TryGetTake(std::string id);
		void _SetTakePresentation(LoopTake& take, LoopTake::PresentationMode mode) noexcept;
		void _WireVuSliders();
		using MidiVstRoutingSnapshot = midi::MidiVstRoutingSnapshot;
		static constexpr std::size_t MaxMidiVstRouteOutputs = 4096u;

		// --- WriteBlock helpers (audio thread) ---

		// Copy each audio buffer into the corresponding VST scratch pointer.
		void _PrepareVstScratch(const AudioState& state, unsigned int sampsToRead) noexcept;

		// Run one VST block: update host time (derived from the clock and current
		// tempo), open the MIDI block, drain live MIDI, read MIDI from loop takes,
		// then process the audio block.
		// When vstActive is false only live MIDI is drained (to prevent queue back-log).
		void _RunVstBlock(vst::VstChain* chain,
			const AudioState& state,
			bool vstActive,
			unsigned int channelCount,
			unsigned int sampsToRead,
			std::uint32_t blockStartSample) noexcept;


		// Cancel and remove without resetting resources borrowed by callback snapshots.
		// Job/UI owners enqueue held NoteOffs; retirement releases resources off callback.
		void _DitchLoopTake(std::shared_ptr<LoopTake>& take, bool flushLiveHeld = true) noexcept;

		// Last recorded MIDI loop in a take (most recently created loop with a
		// non-zero length), or nullptr. Non-audio thread helper.
		static std::shared_ptr<midi::MidiLoop> _LastRecordedMidiLoop(const std::shared_ptr<LoopTake>& take);
		bool _flipTakeBuffer;
		bool _flipAudioBuffer;
		// Immutable metadata publication: UI/job/MIDI diagnostics may read concurrently.
		// No callback reader; Scene serializes accepted writes with rig promotion/export.
		std::atomic<std::shared_ptr<const std::string>> _name;
		const std::string _id;
		unsigned int _fadeSamps;
		unsigned int _lastBufSize;
		std::shared_ptr<utils::Timer> _clock;
		std::shared_ptr<graphics::StationModel> _stationModel;
		// Structural job owner orders meaningful accepted capture-mode changes.
		std::uint64_t _nextPresentationSerial = 1u;
		// Structural owners only; Scene retains the publisher, retirement holds no Scene.
		std::weak_ptr<CameraRecordingPublisher> _cameraRecordingPublisher;
		std::shared_ptr<gui::GuiRack> _guiRack;
		std::shared_ptr<audio::AudioMixer> _masterMixer;
		std::shared_ptr<gui::GuiToggle> _mixerToggle;
		std::shared_ptr<gui::GuiToggle> _routerToggle;
		std::shared_ptr<gui::GuiRouter> _router;
		std::vector<std::shared_ptr<LoopTake>> _loopTakes;
		std::vector<std::shared_ptr<LoopTake>> _backLoopTakes;
		std::atomic<std::shared_ptr<const LoopTakeSnapshot>> _loopTakeSnapshot;
		std::atomic<std::uint64_t> _loopTakeRevision{ 0u };
		std::vector<std::shared_ptr<audio::AudioMixer>> _audioMixers;
		std::vector<std::shared_ptr<audio::AudioMixer>> _backAudioMixers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _audioBuffers;
		std::vector<std::shared_ptr<audio::AudioBuffer>> _backAudioBuffers;
		struct FirstMasterRecording
		{
			utils::Timer::RecordingSeed Timing;
			std::shared_ptr<LoopTake> Take;
			std::shared_ptr<utils::Timer> Clock;
		};
		base::PublishedAudioSnapshot<const FirstMasterRecording> _firstMasterRecording;
		std::atomic<std::uint64_t> _firstMasterRecordingConsumed{ 0u };
		// Callback receipt; consumed version releases this coherent owner-side log.
		std::atomic<std::uint64_t> _firstMasterRecordingScene{ 0u };
		std::atomic<unsigned long> _firstMasterRecordingPhase{ 0ul };
		std::atomic<std::uint8_t> _firstMasterRecordingStatus{ 0u };
		base::PublishedAudioSnapshot<const AudioState> _audioState;
		std::atomic<std::uint8_t> _promotionTopologyGate{ 0u }; // 1 callback, 2 owner.
		base::PublishedAudioSnapshot<const AudioState>::Lease _promotionAudioLease; // Callback only.
		struct RetiredAudioState
		{
			std::uint64_t ReleaseAfterGeneration = 0u;
			std::shared_ptr<const AudioState> State;
		};
		std::vector<RetiredAudioState> _retiredAudioStates;
		std::uint64_t _nextAudioStateGeneration = 1u;
		std::atomic<std::uint64_t> _audioCompletedStateGeneration{ 0u };
		std::atomic<double> _transportOffsetLoopFrac{ 0.0 };
		midi::AutomationPlayback _automationPlayback;
		bool _automationDispatchDirty = false; // Serialized child topology callback; UI commit consumes.

		vst::RackState _rackState;
		vst::RackState::Format _RackFormat() const;
		bool _RackLatencyFeasible(const vst::VstChain& chain) const noexcept;
		void _CompactMidiVstRoutesAfterRemoval(std::size_t index); // Rack calls under its metadata mutex.
		midi::MidiDelivery _midiDelivery;
		midi::CaptureHistory _captureHistory;
		// Coherent correspondence: callback writes, structural owner reads via
		// the existing Timer observation plus this device boundary publication.
		std::atomic<std::uint32_t> _captureDeviceBoundary{ 0u };
        std::atomic<std::uint64_t> _captureAbsoluteBoundary{ 0u };
        std::atomic<std::uint32_t> _captureBoundaryRevision{ 0u };
        std::atomic<std::uint32_t> _captureEarliestPhysicalSample{ 0u };
        std::atomic<bool> _captureHistoryEpochKnown{ false };
        std::atomic<bool> _midiFormatEpochPending{ true };
        std::atomic<bool> _midiFormatEpochKnown{ false };
        std::atomic<std::uint32_t> _midiFormatEpochSample{ 0u };
		std::vector<int> _allowedMidiChannels;
		std::atomic<bool> _membershipRemoving{ false }; // Scene owner writes, UI/job readers; station lifetime pins teardown.
		std::atomic<std::uint16_t> _allowedMidiChannelMask{ 0u };
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
		io::JamFile::GlobalMidiQuantState _globalMidiQuantState = io::JamFile::GlobalMidiQuantState::Off;
		// Structural owner only, serialized with AddTake by Scene/Trigger ownership.
		// Callback reads each take's existing atomic grid publication, never these
		// retained defaults. Station teardown follows callback shutdown.
		RemoteTransportGeometry _remoteMidiGrid;
		std::int64_t _remoteMidiGridOriginSamps = 0;
		std::int32_t _globalPhaseOffsetSamps = 0;
		std::int32_t _stationPhaseOffsetSamps = 0;
	};
}
