#pragma once

// Audio-callback boundary: applies complete published state, owns Timer/map updates,
// and fans policy-neutral operations to entities that retain their own cursors.

#include <array>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>
#include "AudioDevice.h"
#include "ChannelMixer.h"
#include "NinjamMetronome.h"
#include "../io/UserConfig.h"
#include "../engine/Station.h"
#include "../engine/StationRemote.h"
#include "../midi/MidiClockAnchor.h"
#include "../ninjam/NinjamController.h"
#include "../ninjam/NinjamTimingObservationMailbox.h"
#include "../ninjam/NinjamAudioTimingCommand.h"
#include "../ninjam/NinjamLoopAlignment.h"
#include "../utils/Timer.h"
#include "../base/PublishedAudioSnapshot.h"

namespace engine
{
	struct RigSnapshot;
	class SourceLossIntegrationTestAccess;
	struct GroupMuteApplication;
}

namespace audio
{
	class NinjamAudioBoundaryTestAccess;
	class LocalMasterPromotionSafetyTestAccess;
	class RigAudioBoundaryTestAccess;
	class AudioHostRealtimeTestAccess;

	struct StationInventory : std::vector<std::shared_ptr<engine::Station>>
	{
		std::uint64_t TopologyRevision = 0u;
	};

	class AudioHost
	{
		friend class RigAudioBoundaryTestAccess;
		friend class AudioHostRealtimeTestAccess;
		friend class engine::SourceLossIntegrationTestAccess;
	public:
		using TickCallback = std::function<void(Time streamTime, unsigned int numSamps,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<AudioStreamParams>& params)>;

		AudioHost(io::UserConfig userConfig);
		~AudioHost();

		bool Init(std::shared_ptr<ninjam::NinjamController> ninjamController,
			TickCallback tickCallback, bool generatedRig = false,
			const AsioInventory* inventory = nullptr, bool exactSelection = false);
		// The exact shared clock is retained in the prepared callback, not copied per block.
		// Its instance is fixed for this stream; timing geometry still changes at the boundary.
		bool InitWithBlockAdvancement(std::shared_ptr<ninjam::NinjamController> controller,
			std::shared_ptr<utils::Timer> clock, bool generatedRig = false, const AsioInventory* inventory = nullptr, bool exactSelection = false);
		// Callback-only compatibility seam: caller retains clock; stations use our existing lease.
		void AdvanceBlock(utils::Timer* clock, Time time, unsigned int numSamps,
			const std::optional<io::UserConfig>& config, const std::optional<AudioStreamParams>& params);
		const AsioOpenReport& GetAsioOpenReport() const noexcept { return _asioOpenReport; }
		// Serialized lifecycle owner only: false means the previous stream was
		// retained because replacement could not establish a teardown barrier.
		bool LastInitReplacedStream() const noexcept { return _lastInitReplacedStream; }
		void Close();
		struct QuiescenceState
		{
			bool WasRunning = false;
			bool AdmissionWasOpen = false;
		};
		// Serialized control owner only; caller excludes job/session/MIDI writers.
		bool BeginQuiescence(QuiescenceState& state, std::string& error);
		bool EndQuiescence(const QuiescenceState& state, std::string& error);
		bool Reconfigure(const io::UserConfig::AudioSettings& settings, std::string& error);
		const AsioInventory& GetAsioInventory() const noexcept { return _asioInventory; }

		void SetStations(std::shared_ptr<const std::vector<std::shared_ptr<engine::Station>>> stations, std::uint64_t revision = 0u);
		void PublishPendingRigSnapshot(std::shared_ptr<const engine::RigSnapshot> snapshot);
		void RequestRigTriggerTransition(std::uint64_t candidateRevision,
			std::shared_ptr<const engine::RigSnapshot> acceptedSnapshot,
			std::shared_ptr<const engine::RigSnapshot> candidateSnapshot);
		void ClearRigTriggerTransition();
		std::uint64_t TransitionReadyRigRevision() const noexcept
		{
			return _transitionReadyRigRevision.load(std::memory_order_acquire);
		}
		std::uint64_t RejectedRigRevision() const noexcept
		{
			return _rejectedRigRevision.load(std::memory_order_acquire);
		}
		// Call off the audio thread after a newer revision has been acknowledged.
		void ReleaseRigSnapshotsBefore(std::uint64_t revision);
		// Called by the job owner to reclaim snapshots after callback leases end.
		void CollectRetiredSnapshots();
		void PublishGroupMuteApplication(std::shared_ptr<const engine::GroupMuteApplication> application);
		std::uint64_t GroupMuteReceipt() const noexcept { return _groupMuteReceipt.load(std::memory_order_acquire); }
		std::uint64_t AppliedRigRevision() const noexcept
		{
			return _appliedRigRevision.load(std::memory_order_acquire);
		}
		std::uint64_t AudioCallbackHeartbeat() const noexcept
		{
			return _audioCallbackHeartbeat.load(std::memory_order_relaxed);
		}
		// Job owner closes admission before borrowing audio-owned lifecycle state.
		// A closed gate alone is insufficient: the admitted callback must also leave.
		// It proves engine-reader quiescence, not driver teardown: stream geometry
		// and AudioHost lifetime still require the existing driver lifecycle barrier.
		void CloseCallbackAdmission() noexcept;
		bool CallbackAdmissionIsQuiescent() const noexcept;
		bool ReopenCallbackAdmission() noexcept;
		bool ApplyPendingRigWhileQuiescent() noexcept;

		std::shared_ptr<const std::vector<std::shared_ptr<engine::Station>>>
			GetStationsSnapshot() const { return _audioStations.load(); }
		base::PublishedAudioSnapshot<const StationInventory>::Lease
			BorrowStationsSnapshot() const noexcept { return _audioStations.borrow(); }
		// Audio callback only: references the last successfully borrowed station list.
		const std::vector<std::shared_ptr<engine::Station>>& AudioStationsForCallback() const noexcept
		{
			return *_callbackStations;
		}
		std::uint64_t GetAudioSampleCounter() const { return _audioSampleCounter.load(std::memory_order_relaxed); }
		const midi::MidiClockAnchor& GetMidiClockAnchor() const { return _midiClockAnchor; }
		midi::MidiClockAnchor& GetMidiClockAnchor_Ref() { return _midiClockAnchor; }
		const io::UserConfig& GetUserConfig() const { return _userConfig; }

		AudioStreamParams GetStreamParams() const 
		{ 
			const auto params = _publishedStreamParams.load(std::memory_order_acquire);
			return params ? *params : AudioStreamParams{};
		}
		
		AudioDevice* GetDevice() const { return _audioDevice.get(); }
		bool HasMatchingRunningStream(const io::UserConfig::AudioSettings& request);
		bool HasOpenStream(); // Serialized control callers; includes paused streams.
		// Window lifecycle owner: notification observation only, no hardware polling.
		bool ObserveBackendReset(bool deviceChange = false,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		bool SuspendResetStream(std::string& error);
		
		std::shared_ptr<ChannelMixer> GetChannelMixer() { return _channelMixer; }
		void SetNinjamMetronomeEnabled(bool enabled) noexcept { _ninjamMetronomeEnabled.store(enabled, std::memory_order_release); }
		bool NinjamMetronomeEnabled() const noexcept { return _ninjamMetronomeEnabled.load(std::memory_order_acquire); }
		std::optional<ninjam::NinjamTiming> LatestNinjamTiming() const noexcept
		{
			return _ninjamTimingMailbox.ReadLatest();
		}
		void PublishDesiredTiming(const ninjam::NinjamDesiredTransportState& desired);
		std::optional<ninjam::NinjamDesiredTimingReceipt> LastAppliedDesiredTiming() const noexcept;
		void PublishLocalTransportOffsetLoopFrac(double normalizedLoopFrac) noexcept;
		struct LocalMasterTimingReceipt
		{
			std::uint64_t Version = 0u;
			std::uint64_t OldAbsoluteSamplePos = 0u;
			std::uint32_t NewPhaseSamps = 0u;
			std::uint32_t NewMasterLengthSamps = 0u;
		};
		void CancelLocalMasterTiming(std::uint64_t version) noexcept
			{ _localMasterCancelledVersion.store(version, std::memory_order_seq_cst); }
		void CommitLocalMasterTiming(std::uint64_t version) noexcept
			{ _localMasterCommitVersion.store(version, std::memory_order_seq_cst); }
		std::uint64_t PublishLocalMasterTiming(unsigned int seedSamps,
			std::uint32_t masterLengthSamps, utils::Timer::QuantisationType type,
			const engine::LoopTake* targetTake, const engine::Loop* targetLoop) noexcept;
		std::optional<LocalMasterTimingReceipt> LastPreparedLocalMasterTiming() const noexcept;
		std::uint64_t LocalMasterPreparationRetryVersion() const noexcept
			{ return _localMasterPreparationRetryVersion.load(std::memory_order_seq_cst); }
		std::uint64_t LastActivatedLocalMasterTimingVersion() const noexcept
			{ return _localMasterActivatedVersion.load(std::memory_order_seq_cst); }
		// Internally advanced streams keep this same Timer instance until reinitialization.
		// Shares the master transport clock so the audio callback can apply unified
		// timing commands to it at the same boundary as the local takes.
		void SetTimingClock(std::shared_ptr<utils::Timer> clock)
		{
			std::scoped_lock lock(_retainedRigSnapshotsMutex);
			_timingClock.store(std::move(clock));
		}
		float GetAdcPeak(unsigned int channel) const noexcept
		{
			return channel < _AdcPeakChannels ? _adcPeaks[channel].load(std::memory_order_relaxed) : 0.0f;
		}

	private:
		friend class NinjamAudioBoundaryTestAccess;
		bool _Init(std::shared_ptr<ninjam::NinjamController> controller, TickCallback tickCallback,
			std::shared_ptr<utils::Timer> clock, bool generatedRig, const AsioInventory* inventory, bool exactSelection);
		friend class LocalMasterPromotionSafetyTestAccess;
		bool TryEnterCallback() noexcept;
		void PrepareStream(const AudioStreamParams& params);
		void LeaveCallback() noexcept;
		class CallbackLease
		{
		public:
			explicit CallbackLease(AudioHost& host) noexcept : _host(host) {}
			~CallbackLease() { _host.LeaveCallback(); }
			CallbackLease(const CallbackLease&) = delete;
			CallbackLease& operator=(const CallbackLease&) = delete;
		private:
			AudioHost& _host;
		};
		// AudioHost exclusively owns this local-only single-writer/single-reader
		// handoff. Publications coalesce to the latest absolute target, including zero.
		class LocalTransportOffsetLoopFracMailbox
		{
		public:
			void Publish(double normalizedLoopFrac) noexcept;
			std::optional<double> ConsumeLatest() noexcept;

		private:
			static constexpr unsigned int _MaxReadAttempts = 4u;
			std::atomic<std::uint64_t> _sequence{ 0u };
			std::atomic_bool _hasPublication{ false };
			std::atomic<double> _normalizedLoopFrac{ 0.0 };
			std::uint64_t _consumedSequence = 0u;
		};

		struct LocalMasterTimingRequest
		{
			std::uint64_t Version = 0u;
			unsigned int SeedSamps = 0u;
			std::uint32_t MasterLengthSamps = 0u;
			utils::Timer::QuantisationType Type = utils::Timer::QUANTISE_OFF;
			const engine::LoopTake* TargetTake = nullptr;
			const engine::Loop* TargetLoop = nullptr;
		};
		class LocalMasterTimingMailbox
		{
		public:
			std::uint64_t Publish(const LocalMasterTimingRequest& request) noexcept;
			std::optional<LocalMasterTimingRequest> ConsumeLatest() noexcept;
			void Acknowledge(std::uint64_t version) noexcept { _consumedSequence = version; }
		private:
			std::atomic<std::uint64_t> _sequence{ 0u };
			std::atomic<unsigned int> _seedSamps{ 0u };
			std::atomic<std::uint32_t> _masterLengthSamps{ 0u };
			std::atomic<utils::Timer::QuantisationType> _type{ utils::Timer::QUANTISE_OFF };
			std::atomic<const engine::LoopTake*> _targetTake{ nullptr };
			std::atomic<const engine::Loop*> _targetLoop{ nullptr };
			std::uint64_t _consumedSequence = 0u; // callback owner
		};
		void ApplyLocalMasterTimingAtAudioBoundary(
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			utils::Timer* timingClock) noexcept;
		bool ApplyDesiredTimingAtAudioBoundary(std::uint64_t blockStartSample,
			unsigned int sampleRate) noexcept;
		bool ApplyDesiredTimingAtAudioBoundary(std::uint64_t blockStartSample,
			unsigned int sampleRate,
			const std::vector<std::shared_ptr<engine::Station>>* stations,
			utils::Timer* timingClock) noexcept;
		void ApplyPendingRigSnapshotAtAudioBoundary() noexcept;
		void ApplyPendingGroupMuteAtAudioBoundary() noexcept;
		void PublishRigTriggerTransitionAtAudioBoundary() noexcept;
		void ApplyLocalTransportOffsetAtAudioBoundary(
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			utils::Timer* timingClock) noexcept;
		void PublishAudioCallbackProgress(std::uint64_t blockStartSample,
			unsigned int numSamps) noexcept;
		std::optional<std::int64_t> RestoreMappedSourceAtScene(
			const std::vector<std::shared_ptr<engine::Station>>* stations,
			std::uint64_t sceneCoordinateSamps) noexcept;
		void CaptureMappedSourceAnchorsAfterOffset(
			const std::vector<std::shared_ptr<engine::Station>>& stations) noexcept;
		static int AudioCallback(void* outBuffer,
			void* inBuffer,
			unsigned int numSamps,
			double streamTime,
			RtAudioStreamStatus status,
			void* userData);
		static int BackendAudioCallback(void* outBuffer, void* inBuffer,
			unsigned int numSamps, double streamTime, RtAudioStreamStatus status, void* userData);

		void _OnAudio(float* inBuffer,
			float* outBuffer,
			unsigned int numSamps,
			double streamTime);

	private:
		io::UserConfig _userConfig;
		std::optional<io::UserConfig> _tickUserConfig;
		// Prepared before startStream; each failed stream is closed before the next write.
		// The callback only reads this stable storage while its stream is running.
		std::optional<AudioStreamParams> _preparedStreamParams;
		// Off-callback readers copy immutable metadata instead of racing device replacement.
		std::atomic<std::shared_ptr<const AudioStreamParams>> _publishedStreamParams;
		std::mutex _audioMutex;
		std::unique_ptr<AudioDevice> _audioDevice;
		AsioOpenReport _asioOpenReport;
		AsioInventory _asioInventory; // lifecycle owner writes; UI reads under control exclusion
		bool _backendLifetimeUncertain = false;
		bool _backendResetObserved = false; // Serialized lifecycle owner.
		friend class AsioStreamTestAccess;
		std::chrono::steady_clock::time_point _deviceChangeUntil{}, _lastDeviceHeartbeatAt{};
		std::chrono::milliseconds _deviceStallThreshold{500};
		std::uint64_t _lastDeviceHeartbeat = 0u;
		// Lifecycle writes; job admission recovery reads. Failure stays closed until explicit recovery.
		std::atomic<bool> _callbackRecoveryBlocked{false}; // lifecycle owner; blocks resume/replacement until stop is confirmed
		// A driver that cannot confirm stop retains its callback userdata. The context
		// fences host access separately from engine admission during final teardown.
		struct CallbackContext
		{
			// Host: lifecycle writer/backend reader; Readers: backend writers/final
			// teardown reader. Sequential consistency fences late backend ingress
			// against nulling Host and draining the readers that observed it.
			// OutputChannels: prepared with the backend stopped; callback reads only
			// for silence after Host teardown. Quarantined with an uncertain driver.
			std::atomic<AudioHost*> Host{ nullptr };
			std::atomic<unsigned int> Readers{ 0u };
			std::atomic<unsigned int> OutputChannels{ 0u };
		};
		std::unique_ptr<CallbackContext> _callbackContext;
		bool _lastInitReplacedStream = false;
		std::shared_ptr<ChannelMixer> _channelMixer;
		NinjamMetronome _ninjamMetronome;
		ninjam::NinjamMetronomeTimingState _ninjamMetronomeTimingState;
		ninjam::NinjamTimingObservationMailbox _ninjamTimingMailbox;
		std::mutex _ninjamDesiredTimingPublishMutex;
		ninjam::NinjamDesiredTransportStateMailbox _ninjamDesiredTimingMailbox;
		std::atomic<std::uint64_t> _lastAppliedTimingReceiptSequence{ 0u };
		std::atomic<std::uint64_t> _lastAppliedDesiredVersion{ 0u };
		std::atomic<std::uint64_t> _lastAppliedSessionEpoch{ 0u };
		std::atomic<std::uint64_t> _lastAppliedDesiredGeneration{ 0u };
		std::atomic<ninjam::NinjamDesiredTimingIntent> _lastAppliedDesiredIntent{
			ninjam::NinjamDesiredTimingIntent::NoSync };
		std::atomic<ninjam::NinjamLocalFollowPolicy> _lastAppliedDesiredPolicy{
			ninjam::NinjamLocalFollowPolicy::NoSync };
		std::atomic<std::uint64_t> _lastAppliedTimingSceneCoordinate{ 0u };
		std::atomic<long long> _lastAppliedLocalSourceCorrectionSamps{ 0 };
		// Audio writer/job readers; bracketed by the existing receipt sequence.
		// Lifetime follows AudioHost and its stopped-before-destruction callback.
		std::atomic<unsigned long> _lastAppliedSourceLengthSamps{ 0ul };
		std::atomic<unsigned long> _lastAppliedRemoteLengthSamps{ 0ul };
		std::atomic<std::int64_t> _lastAppliedSourceCoordinateSamps{ 0 };
		LocalTransportOffsetLoopFracMailbox _localTransportOffsetLoopFracMailbox;
		LocalMasterTimingMailbox _localMasterTimingMailbox;
		LocalMasterTimingRequest _preparedLocalMasterTiming{}; // Callback owner.
		std::uint64_t _preparedLocalMasterSceneSample = 0u;
		std::uint32_t _preparedLocalMasterPhase = 0u;
		std::atomic<std::uint64_t> _localMasterCommitVersion{ 0u };
		std::atomic<std::uint64_t> _localMasterCancelledVersion{ 0u };
		std::atomic<std::uint64_t> _localMasterActivatedVersion{ 0u };
		std::atomic<std::uint64_t> _localMasterPreparationRetryVersion{ 0u };
		std::atomic<std::uint64_t> _localMasterReceiptSequence{ 0u };
		std::atomic<std::uint64_t> _localMasterReceiptVersion{ 0u };
		std::atomic<std::uint64_t> _localMasterReceiptOldAbsolute{ 0u };
		std::atomic<std::uint32_t> _localMasterReceiptNewPhase{ 0u };
		std::atomic<std::uint32_t> _localMasterReceiptNewLength{ 0u };
		base::PublishedAudioSnapshot<utils::Timer> _timingClock;
		// Last successful callback lease is the bounded-publication fallback.
		base::PublishedAudioSnapshot<utils::Timer>::Lease _lastAudioTimingClock;
		std::uint64_t _ninjamTimingObservationSequence = 0u;
		double _localTransportOffsetLoopFrac = 0.0;
		unsigned long _localTransportOffsetMasterLength = 0ul;
		long long _localTransportOffsetTargetSamps = 0;
		std::atomic_bool _ninjamMetronomeEnabled{ true };

		std::atomic<std::uint64_t> _audioSampleCounter{ 0 };
		// Replaced only with callback admission closed, then read by callback.
		std::uint64_t _captureHistoryEpochDeviceSample = 0u;
		midi::MidiClockAnchor _midiClockAnchor;
		static constexpr unsigned int _AdcPeakChannels = 32u;
		std::array<std::atomic<float>, _AdcPeakChannels> _adcPeaks{};

		const std::vector<std::shared_ptr<engine::Station>>* _callbackStations = &_emptyStations; // Audio owner; pinned by a retained lease.
		base::PublishedAudioSnapshot<const StationInventory> _audioStations;
		// Serialized job writes; audio borrows transiently and publishes receipt.
		// Reclaim application references off callback. Stop reader before teardown.
		base::PublishedAudioSnapshot<const engine::GroupMuteApplication> _groupMuteApplication;
		std::uint64_t _groupLatestVersion = 0u; // writer only, under retention mutex
		std::uint64_t _groupAppliedVersion = 0u; // audio only
		std::atomic<std::uint64_t> _groupMuteReceipt{ 0u };
		// Constructed with AudioHost, so an empty station set never triggers local-static
		// initialization or STL debug iterator allocation on the audio callback.
		const std::vector<std::shared_ptr<engine::Station>> _emptyStations;
		// Declared after its publisher so teardown releases this lease first.
		base::PublishedAudioSnapshot<const StationInventory>::Lease
			_lastAudioStations;
	// Retain snapshots off audio so callback references cannot become the last owner.
		base::PublishedAudioSnapshot<const engine::RigSnapshot> _pendingRigSnapshot;
		// Replacing this handle must not destroy the last snapshot or Trigger on audio.
		base::PublishedAudioSnapshot<const engine::RigSnapshot>::Lease _audioAppliedRigSnapshot;
		std::atomic<std::uint64_t> _rigTriggerTransitionRequestRevision{ 0u };
		std::atomic<std::uint64_t> _rigTriggerTransitionAcceptedRevision{ 0u };
		base::PublishedAudioSnapshot<const engine::RigSnapshot> _rigTriggerTransitionCandidate;
		std::atomic<std::uint64_t> _transitionReadyRigRevision{ 0u };
		std::atomic<std::uint64_t> _rejectedRigRevision{ 0u };
		std::mutex _retainedRigSnapshotsMutex;
		std::vector<std::shared_ptr<const engine::RigSnapshot>> _retainedRigSnapshots;
		std::atomic<std::uint64_t> _appliedRigRevision{ 0u };
		std::atomic<std::uint64_t> _audioCallbackHeartbeat{ 0u };
		// One callback may own the engine. Close shares the admission word, so a
		// late callback cannot race a quiescent owner's lifecycle pump.
		static constexpr std::uint32_t _CallbackAdmissionClosed = 2u;
		static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
		std::atomic<std::uint32_t> _callbackAdmission{ 0u };
		// Rejected callbacks cannot borrow prepared parameters while the job owner
		// has the gate. This scalar supplies only the output silencing geometry.
		std::atomic<unsigned int> _callbackOutputChannels{ 0u };
		std::uint64_t _audioRigRevision = 0u;
		std::shared_ptr<ninjam::NinjamController> _ninjamController;
		// Audio-thread owned phase-map geometry. The map is rebased after every
		// accepted common correction so the next block cannot undo it.
		ninjam::NinjamLocalFollowPolicy _activeNinjamFollowPolicy = ninjam::NinjamLocalFollowPolicy::NoSync;
		ninjam::NinjamDesiredTransportState _appliedNinjamTiming{};
		ninjam::SyncPhaseMap _syncPhaseMap;
		bool _captureMappedSourceAnchorsAfterOffset = false;
		bool _mappedSourceHandledAtCommandBoundary = false;
		TickCallback _tickCallback;
	};
}
