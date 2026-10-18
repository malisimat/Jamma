#pragma once
#include "../vst/VstRackSlot.h"
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
#include "../graphics/QuantisationModel.h"
#include "../graphics/QuantisationDivisionModel.h"
#include "../graphics/StationModel.h"
#include "Trigger.h"
#include "AudioSink.h"
#include "../base/PublishedAudioSnapshot.h"
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
		bool IsPresentationMoving() const noexcept { return _presentationOffsetX != 0.0f; }
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
		bool LoadManagedVstPlugin(std::wstring path);
		bool UnloadManagedVstPlugin(const std::string& identity);
		void ForceUnloadAllVstPlugins();

		// Non-RT accessor to retrieve a loaded plugin instance (or nullptr).
		std::shared_ptr<vst::IVstPlugin> GetVstPlugin(size_t index) const;
		bool OwnsPlugin(const vst::IVstPlugin* plugin) const noexcept
		{
			const auto chain = _vstChain.load(std::memory_order_acquire);
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

		static constexpr unsigned int _HiddenSeedSamps = 1u;
		static constexpr float _StationModelHeight = 470.0f;
		static constexpr float _StationModelYOffset = _StationModelHeight * 0.5f;

		static unsigned int _CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes);
		
		static void _DrainVstChain(std::shared_ptr<vst::VstChain> chain);
		static unsigned int _ResolveSampleRate(std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params);
		static void _TrySeedClockFromFirstLoop(const std::shared_ptr<utils::Timer>& clock,
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

		// Prepared/pinned on the structural owner; MIDI ownership is mutated only
		// by audio dispatch. Counts are per destination, never per changing output index.
		struct MidiPlaybackBinding;
		struct MidiDestination
		{
			std::shared_ptr<vst::IVstPlugin> Plugin;
			std::bitset<midi::MidiNote::TotalNoteSlots> LiveHeld;
			// Audio owner consumes; quiescent lifecycle owner marks actual delivered
			// live notes after all writers drain. Destination lifetime follows snapshots.
			std::bitset<midi::MidiNote::TotalNoteSlots> PendingLiveReset;
			std::array<std::uint32_t, midi::MidiNote::TotalNoteSlots> LiveNoteOnRevision{};
			std::array<unsigned int, midi::MidiNote::TotalNoteSlots> PlaybackOwners{};
			std::array<unsigned int, midi::MidiNote::TotalNoteSlots> RecordedAudibleOwners{}; // Audio delivery time.
			// Audio callback owner: rejected releases retry in later blocks.
			std::bitset<midi::MidiNote::TotalNoteSlots> PendingReleases;
			std::array<std::uint8_t, midi::MidiNote::TotalNoteSlots> PendingReleaseVelocity{};
			std::bitset<midi::MidiNote::TotalNoteSlots> PluginHeld;
			std::bitset<midi::MidiNote::TotalNoteSlots> BlockHeldAtStart;
			std::bitset<midi::MidiNote::TotalNoteSlots> BlockTouched;
			std::size_t ReleaseCursor = 0u;
			struct ScheduledMidi
			{
				midi::MidiEvent Event;
				std::uint64_t Sequence = 0u;
				MidiPlaybackBinding* Owner = nullptr; // Pinned by QueuedEvents.
				bool IsRealtime = false;
			};
			static constexpr std::size_t ScheduledCapacity = 1024u;
			static constexpr std::size_t ScheduledReleaseReserve = 256u;
			std::array<ScheduledMidi, ScheduledCapacity> Scheduled{};
			std::size_t ScheduledCount = 0u; // Audio callback only.
			std::uint64_t ScheduleSequence = 0u;
			std::atomic<unsigned int> CompensationDelaySamps{ 0u };
			std::atomic<std::uint32_t> CompensationEpoch{ 0u }; // Job publication.
			std::uint32_t AppliedCompensationEpoch = 0u; // Audio callback.
			unsigned int ActiveCompensationDelaySamps = 0u; // Audio callback.
			bool RecoveringCompensationChange = false; // Current callback block.
			bool LastBypassed = false; // Audio callback.
		};
		struct MidiPlaybackBinding
		{
			std::shared_ptr<LoopTake> Take;
			std::shared_ptr<midi::MidiLoop> Loop;
			std::shared_ptr<MidiDestination> Destination;
			std::bitset<midi::MidiNote::TotalNoteSlots> Held;
			std::bitset<midi::MidiNote::TotalNoteSlots> AudibleHeld; // Delayed note reached the plugin.
			std::array<std::uint16_t, midi::MidiNote::TotalNoteSlots> AudibleSlots{};
			std::array<std::uint16_t, midi::MidiNote::TotalNoteSlots> AudibleSlotPositions{};
			std::size_t AudibleSlotCount = 0u; // Dense callback-owned active slots.
			std::atomic<bool> HasHeld{ false }; // Audio publishes; job reclaims unused bindings.
			std::atomic<unsigned int> QueuedEvents{ 0u }; // Pins raw queue identity until removed.
			std::atomic<bool> RoutingActive{ true }; // Job publication; callback cancellation.
			std::uint64_t ObservedSourceRevision = 0u; // Audio callback.
			bool HasObservedSourceRevision = false;
			bool HasObservedSourceState = false;
			bool CancelScheduledThisBlock = false;
			std::uint64_t ObservedRelocationGeneration = 0u; // Audio callback.
			std::atomic<bool> ReleaseConsumed{ false };
			std::size_t ReleaseSlot = 0u; // Audio only; preserves the per-block ingress budget.
			bool ReleaseQueued = false; // Job producer only.
			bool EmitSourceLossRelease = true; // Frozen before queue publication.
		};
		struct SyntheticMidi
		{
			midi::MidiEvent Event;
			MidiPlaybackBinding* Release = nullptr; // Pinned until audio acknowledges.
			bool SourceLossNoteOff = false;
			std::uint32_t LiveNoteOnRevision = 0u;
		};
		struct AudioState
		{
			std::uint64_t Generation = 0u;
			// Retire strong ownership after the boundary acknowledgement so LoopTakes cannot die in the callback.
			std::vector<std::shared_ptr<LoopTake>> OwnedLoopTakes;
			std::vector<std::weak_ptr<LoopTake>> LoopTakes;
			std::vector<std::shared_ptr<audio::AudioMixer>> AudioMixers;
			std::vector<std::shared_ptr<audio::AudioBuffer>> AudioBuffers;
			std::vector<float> VstBlockScratch;
			std::vector<float*> VstBlockPtrs;
			std::shared_ptr<vst::VstChain> MidiChain;
			std::vector<std::shared_ptr<MidiDestination>> LiveMidiDestinations;
			std::vector<std::shared_ptr<MidiDestination>> MidiDestinations;
			std::vector<std::shared_ptr<MidiDestination>> RetiredMidiDestinations;
			mutable std::atomic<bool> RetiredMidiQueueCancelled{ false };
			std::vector<std::vector<std::shared_ptr<MidiPlaybackBinding>>> MidiPlaybackRoutes;
		};
		class MidiPlaybackSink final : public midi::IMidiOutputSink
		{
		public:
			explicit MidiPlaybackSink(const AudioState& state) noexcept : _state(state) {}
			void OnEvent(unsigned int outputIndex, const midi::MidiEvent& event) noexcept override;
			bool TryOnEvent(unsigned int outputIndex, const midi::MidiEvent& event) noexcept override;
		private:
			const AudioState& _state;
		};
		void _PrepareMidiDestinations(AudioState& state);
		static bool _TryDeliverMidi(MidiDestination& destination, const midi::MidiEvent& event, bool isRealtime, MidiPlaybackBinding* owner = nullptr) noexcept;
		static bool _TryDeliverMidiNow(MidiDestination& destination, const midi::MidiEvent& event, bool isRealtime) noexcept;
		static bool _TryDeliverRecordedMidiNow(MidiDestination& destination, const midi::MidiEvent& event,
			MidiPlaybackBinding& owner, bool isRealtime) noexcept;
		static void _DropRecordedAudibleOwner(MidiDestination& destination, MidiPlaybackBinding& owner,
			bool emitRelease = true) noexcept;
		static void _DrainScheduledMidi(const AudioState& state, std::uint32_t blockStartSample, unsigned int numSamps) noexcept;
		static std::bitset<midi::MidiNote::TotalNoteSlots> _CancelScheduledMidiForOwner(
			MidiDestination& destination, MidiPlaybackBinding* owner) noexcept;
		static void _RecoverCancelledScheduledMidi(MidiDestination& destination,
			const std::bitset<midi::MidiNote::TotalNoteSlots>& affected) noexcept;
		static void _InvalidateScheduledMidiAtBlockStart(const AudioState& state, bool forceCancel = false) noexcept;
		static void _DrainPendingMidiReleases(const AudioState& state, std::uint32_t sample) noexcept;
		static void _RecoverSkippedMidiBlocks(const AudioState& state) noexcept;
		static void _DispatchLiveMidi(const AudioState& state, const midi::MidiEvent& event,
			bool sourceLossNoteOff, std::uint32_t liveNoteOnRevision) noexcept;
		static bool _ReleaseLostPlaybackMidi(MidiPlaybackBinding& binding, std::uint32_t sample,
			unsigned int& budget, unsigned int& slotBudget) noexcept;
		void _QueueLostPlaybackMidi(const std::shared_ptr<LoopTake>& take, bool emitRelease = true);
		void _FlushPendingLostPlaybackMidi();
		bool _TryEnqueueSyntheticMidi(SyntheticMidi entry) noexcept;

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
			static bool _TryEnqueueOrderedLiveMidi(midi::MidiQueue<1024>& queue,
				bool& hasLastSample,
				std::uint32_t& lastSample,
				const midi::MidiEvent& event) noexcept;

		// Cancel and remove without resetting resources borrowed by callback snapshots.
		// Job/UI owners enqueue held NoteOffs; retirement releases resources off callback.
		void _DitchLoopTake(std::shared_ptr<LoopTake>& take, bool flushLiveHeld = true) noexcept;

		// --- Parameter automation dispatch ---

		// One pre-resolved automation mapping ready for the audio thread. All
		// pointer chasing and lane metadata are baked in by RebuildAutomationDispatch
		// so the audio path is a flat loop over a hot cache line.
		// IMMUTABLE after publication: the audio thread must never write into this
		// struct, which is what makes the double-buffer scheme race-free.
		struct AutomationDispatch
		{
			vst::IVstPlugin* plugin = nullptr;          // raw observer — lifetime owned by VstChain
			unsigned int     paramIdx = 0u;
			midi::MidiLoop*                   loop = nullptr;            // raw observer — lifetime owned by LoopTake
			std::uint8_t                      laneIdx = 0u;              // which lane within loop to read
			std::uint32_t                     loopLengthSamps = 0u;      // frozen at rebuild
			std::uint32_t                     automationGlobalSampleOrigin = 0u; // frozen at rebuild; automation origin
			const std::atomic<std::int32_t>*  anchorCorrection = nullptr; // live correction from owning LoopTake
			// effective origin = automationGlobalSampleOrigin + anchorCorrection (modular uint32)
		};
		// Per-entry playback state owned exclusively by the audio thread.
		// Kept separate from AutomationDispatch so the dispatch buffers are
		// immutable after publication; the writer can safely overwrite the
		// back buffer without racing the audio callback's cursor/value updates.
		struct AutomationPlaybackState
		{
			std::uint16_t cursorIdx = 0u;
			std::uint64_t stateGeneration = 0;
			float         lastValue = -2.0f;  // sentinel: force first write
		};
		static constexpr std::size_t MaxAutomationDispatches = 64u;
		static constexpr float AutomationEpsilon = 1.0f / 65536.0f; // below 16-bit param resolution

		// Run one automation dispatch block on the audio thread: advance each
		// lane's cursor, interpolate, and SetParameter (delta-gated). Real-time safe.
		void _RunAutomationDispatch(std::uint32_t blockStartSample,
			std::uint32_t numSamps) noexcept;

		// Last recorded MIDI loop in a take (most recently created loop with a
		// non-zero length), or nullptr. Non-audio thread helper.
		static std::shared_ptr<midi::MidiLoop> _LastRecordedMidiLoop(const std::shared_ptr<LoopTake>& take);
		bool _flipTakeBuffer;
		bool _flipAudioBuffer;
		std::string _name;
		const std::string _id;
		unsigned int _fadeSamps;
		unsigned int _lastBufSize;
		std::shared_ptr<utils::Timer> _clock;
		std::shared_ptr<QuantisationModel> _quantisationModel;
		std::shared_ptr<QuantisationDivisionModel> _quantisationDivisionModel;
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
		// Flat automation dispatch list, double-buffered and published with an
		// atomic-swap release store (audio thread reads with acquire). Built only on
		// the non-audio thread in RebuildAutomationDispatch.
		// The buffers are immutable once published — the audio thread never writes
		// into them, so the writer can safely fill the back buffer at any time.
		std::atomic<const AutomationDispatch*> _automationDispatch{ nullptr };
		AutomationDispatch _automationDispatchBuf[2][MaxAutomationDispatches]{};
		std::uint8_t       _automationDispatchCount[2]{};
		std::uint8_t       _automationDispatchBack = 0u;
		// Audio-thread-only playback state, parallel to the active dispatch list.
		// Reset whenever _RunAutomationDispatch detects a new list was published.
		AutomationPlaybackState  _automationPlaybackState[MaxAutomationDispatches]{};
		// Last dispatch pointer seen by the audio thread; used to detect rebuilds.
		const AutomationDispatch* _automationDispatchFront = nullptr;

		// VST insert chain applied after all LoopTakes are mixed down,
		// just before each channel is sent to the output AudioMixer.
		// Published atomically for lock-free audio-thread reads.
		std::atomic<std::shared_ptr<vst::VstChain>> _vstChain;
		std::shared_ptr<vst::VstChain> _backVstChain;
		std::atomic<bool> _flipVstChain{ false };

		// Pending load/unload requests staged by LoadVstPlugin / UnloadVstPlugin.
		// Read only on the job thread (from OnAction(JobAction)).
		// Each entry carries the plugin path and an optional VST2 initial-state blob.
		std::vector<actions::JobAction> _pendingVstLoads;
		std::vector<actions::JobAction> _pendingVstUnloads;
		// _vstPluginPaths: written on job thread (OnAction), read on main thread (VstEntries).
		// Access is guarded by _vstPathsMutex in both directions.
		mutable std::mutex _vstPathsMutex;
		std::vector<std::wstring> _vstPluginPaths;
		std::vector<io::JamFile::VstEntry> _vstSavedEntries;
		// UI stages requests, job completes them. Audio never reads this counter.
		std::atomic<unsigned int> _vstJobsPending{ 0u };
		midi::MidiQueue<1024> _immediateLiveMidiIngress;
		midi::MidiQueue<1024, SyntheticMidi> _syntheticLiveMidiIngress;
		std::vector<std::weak_ptr<MidiDestination>> _midiDestinations;
		std::vector<std::shared_ptr<MidiPlaybackBinding>> _midiPlaybackBindings;
		std::vector<std::shared_ptr<MidiPlaybackBinding>> _pendingLostPlaybackMidi;
		bool _hasLastImmediateLiveMidiSample = false;
		std::uint32_t _lastImmediateLiveMidiSample = 0u;
		bool _hasLastSyntheticLiveMidiSample = false;
		std::uint32_t _lastSyntheticLiveMidiSample = 0u;
		mutable std::mutex _liveHeldMidiMutex;
		std::vector<std::pair<std::string, midi::MidiNoteSnapshot>> _liveHeldMidi;
		struct RecentPhysicalMidi
		{
			midi::MidiEvent Event;
			std::string Device;
			std::uint8_t PriorDeviceVelocity = 0u;
			std::uint8_t PriorAnyVelocity = 0u;
			std::uint64_t Sequence = 0u;
		};
		// Structural/MIDI job owner; guarded with the existing held-state mutex.
		std::vector<RecentPhysicalMidi> _recentPhysicalMidi;
		std::vector<std::weak_ptr<LoopTake>> _midiCaptureTargets;
		std::optional<std::uint32_t> _midiHistoryDroppedThroughSample;
		std::uint64_t _physicalMidiSequence = 0u;
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
		midi::MidiNoteSnapshot _anonymousLiveHeldMidi;
		std::bitset<midi::MidiNote::TotalNoteSlots> _pendingLostLiveMidiNoteOffs;
		// Audio publishes delivered live note-ons; job snapshots that revision for
		// lost-live releases. Dropped ingress must never reserve note ownership.
		std::array<std::atomic<std::uint32_t>, midi::MidiNote::TotalNoteSlots> _liveNoteOnRevisions{};
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
		std::optional<engine::QuantisationParams> _pendingQuantisationParams;
		bool _pendingQuantisationConfirm = false;
		float _quantisationOverlayAlpha = 0.0f;
		io::JamFile::GlobalMidiQuantState _globalMidiQuantState = io::JamFile::GlobalMidiQuantState::Off;
		// Structural owner only, serialized with AddTake by Scene/Trigger ownership.
		// Callback reads each take's existing atomic grid publication, never these
		// retained defaults. Station teardown follows callback shutdown.
		RemoteTransportGeometry _remoteMidiGrid;
		std::int64_t _remoteMidiGridOriginSamps = 0;
		std::int32_t _globalPhaseOffsetSamps = 0;
		std::int32_t _stationPhaseOffsetSamps = 0;
		float _presentationOffsetX = 0.0f; // Window owner; no callback/job access.
		float _presentationStartOffsetX = 0.0f;
		float _presentationElapsed = 0.0f;
	};
}
