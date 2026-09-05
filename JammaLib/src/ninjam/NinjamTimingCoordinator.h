#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include "NinjamTiming.h"
#include "NinjamTimingTracker.h"
#include "NinjamAudioTimingCommand.h"
#include "../engine/QuantisationTiming.h"

namespace io
{
	struct UserConfig;
}

namespace ninjam
{
	struct NinjamSessionTimingStatus;

	struct NinjamTempoJoinOptions
	{
		// Servers commonly quantise a requested decimal BPM to an integer. Treat a
		// near result as the pushed tempo so local content follows its transport.
		static constexpr float TempoRequestAcknowledgementToleranceBpm = 1.0f;
		static constexpr auto DefaultTempoRequestDeadline = std::chrono::seconds(15);

		bool PushLocalTempoOnJoin = true;
		bool PromptBeforeApplyingRemoteTempo = true;
		// Maximum re-sends of a local tempo request before it is abandoned. Retries
		// do not move the original sent-at anchor (§2.6/§3.5).
		unsigned int MaxTempoRequestRetries = 3u;
		std::chrono::steady_clock::duration TempoRequestDeadline = DefaultTempoRequestDeadline;
	};

	// Explicit lifecycle for a locally pushed tempo request. Replaces the ad hoc
	// boolean flags so acknowledgement, retry, expiry, and send-failure are
	// modelled as discrete transitions (§2.6/§3.5).
	enum class TempoRequestState : std::uint8_t
	{
		Idle,
		Queued,
		SentAwaitingOutcome,
		Acknowledged,
		Expired
	};

	struct NinjamTempoChange
	{
		unsigned int IntervalLengthSamps = 0u;
		unsigned int SourceSampleRate = 0u;
		unsigned int GrainSamps = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
		unsigned int IntervalPositionSamps = 0u;
		std::uint64_t AudioBlockStartSample = 0u;

		bool HasSameProposalIdentity(const NinjamTempoChange& other) const noexcept;
	};

	struct NinjamTempoRequest
	{
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
	};

	enum class NinjamNoSyncReason : std::uint8_t
	{
		None,
		StayLocal,
		Reconnect,
		Disconnect,
		PhysicalLoss,
		InvalidTiming,
		ObservationDeadline
	};

	struct NinjamRemoteGridPublication
	{
		engine::RemoteTransportGeometry Geometry;
		std::int64_t OriginSamps = 0;
	};

	struct NinjamTimingUpdate
	{
		std::optional<NinjamDesiredTransportState> DesiredTransport;
		std::optional<NinjamTempoRequest> TempoRequest;
		std::optional<NinjamRemoteGridPublication> RemoteGrid;
		bool PromptForTempoChange = false;
		NinjamNoSyncReason NoSyncReason = NinjamNoSyncReason::None;
	};

	// Kind of the most recent transport command the coordinator emitted, recorded
	// for live telemetry so the job thread can reconcile emitted commands against
	// audio-side consumption without any per-block logging (Phase 7).
	enum class NinjamEmittedCommand : std::uint8_t
	{
		None,
		Replace,
		Join,
		Discipline,
		Invalidate
	};

	enum class NinjamTimingDiagnosticReason : std::uint8_t
	{
		ObservationDisconnected,
		ObservationInvalid,
		ObservationZeroInterval,
		ObservationInvalidSampleRate,
		ObservationInvalidTempo,
		ObservationInvalidBpi,
		ObservationMissingAudioBoundary,
		ObservationMissingLocalTransport,
		ObservationInvalidGrain,
		TrackerImplausibleBackward,
		SafetyLimitExceeded,
		DesiredPublished,
		NoSyncPublished,
		DesiredApplied,
		DesiredApplyLag,
		DesiredApplyCaughtUp,
		Count
	};

	struct NinjamTimingDiagnosticEvent
	{
		std::uint64_t Sequence = 0u;
		NinjamTimingDiagnosticReason Reason = NinjamTimingDiagnosticReason::ObservationInvalid;
		std::uint64_t SessionEpoch = 0u;
		std::uint64_t AppliedSessionEpoch = 0u;
		std::uint64_t DesiredVersion = 0u;
		std::uint64_t AppliedVersion = 0u;
		std::uint64_t Generation = 0u;
		long long ValueSamps = 0;
		std::uint64_t LimitSamps = 0u;
	};

	struct NinjamTimingDiagnostics
	{
		static constexpr std::size_t EventCapacity = 32u;
		static constexpr std::size_t ReasonCount =
			static_cast<std::size_t>(NinjamTimingDiagnosticReason::Count);

		void SetCaptureEnabled(bool enabled) noexcept { CaptureEnabled = enabled; }
		void Capture(NinjamTimingDiagnosticReason reason,
			std::uint64_t sessionEpoch,
			std::uint64_t desiredVersion,
			std::uint64_t appliedVersion,
			std::uint64_t generation,
			long long valueSamps,
			std::uint64_t limitSamps,
			std::uint64_t appliedSessionEpoch = 0u) noexcept;
		std::uint64_t Count(NinjamTimingDiagnosticReason reason) const noexcept;

		bool CaptureEnabled = false;
		std::uint64_t CapturedEventCount = 0u;
		std::uint64_t EventOverflowCount = 0u;
		std::uint64_t EventSequence = 0u;
		std::array<std::uint64_t, ReasonCount> ReasonCounts{};
		std::array<NinjamTimingDiagnosticEvent, EventCapacity> Events{};
		NinjamTimingDiagnosticEvent LatestEvent{};
		std::uint64_t ObservationsAccepted = 0u;
		std::uint64_t ObservationsRejected = 0u;
		std::uint64_t DuplicateObservations = 0u;
		std::uint64_t GenerationChanges = 0u;
		std::uint64_t WrapEvents = 0u;
		std::uint64_t JoinEvents = 0u;
		std::uint64_t PhaseEventsQueued = 0u;
		std::uint64_t PhaseEventsConsumed = 0u;
		std::uint64_t PhaseEventsInvalidated = 0u;
		std::uint64_t SafetyLimitRejections = 0u;
		std::uint64_t TempoProposals = 0u;
		std::uint64_t TempoAccepted = 0u;
		std::uint64_t TempoRejected = 0u;
		std::uint64_t TempoAcknowledged = 0u;
		long long MaxPhaseErrorSamps = 0;
		std::uint64_t PhaseErrorBuckets[4]{};
		// Phase 7 live-validation telemetry (job-thread drained fixed counters).
		std::uint64_t MaxObservationAgeSamps = 0u;   // Oldest observation age at consume time.
		std::uint64_t TempoRequestsSent = 0u;        // First-attempt local tempo pushes.
		std::uint64_t TempoRequestRetries = 0u;      // Re-sends against the original anchor.
		std::uint64_t TempoRequestsExpired = 0u;     // Requests abandoned after exhausting retries.
		std::uint64_t CommandsEmitted = 0u;          // Monotonic emitted-command sequence.
		std::uint64_t LastCommandGeneration = 0u;    // Generation tag of the last emitted command.
		NinjamEmittedCommand LastCommandType = NinjamEmittedCommand::None;
	};


	class NinjamTimingCoordinator
	{
	public:
		NinjamTimingUpdate Connect(const NinjamTempoJoinOptions& options,
			const std::optional<engine::QuantisationTiming>& localTiming) noexcept;
		NinjamTimingUpdate Disconnect() noexcept;
		NinjamTimingUpdate ObserveSessionStatus(const NinjamSessionTimingStatus& status,
			const NinjamTempoJoinOptions& options,
			const std::optional<engine::QuantisationTiming>& localTiming) noexcept;
		NinjamTimingUpdate Observe(const NinjamTiming& timing,
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent,
		const io::UserConfig& config,
			utils::Timer& clock,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		NinjamTimingUpdate Tick(const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent,
			utils::Timer& clock,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		std::optional<NinjamTempoChange> PendingTempoChange() const { return _pendingTempoChange; }
		NinjamTimingUpdate ResolveTempoChange(bool accept,
			const std::optional<engine::QuantisationTiming>& localTiming,
			utils::Timer& clock);
		// Feedback from the network layer after attempting to deliver a tempo
		// request. A failed send returns the request to Queued so the next interval
		// boundary re-sends it; success leaves it awaiting server acknowledgement.
		void NotifyTempoRequestSent(bool success,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
		TempoRequestState RequestState() const noexcept { return _requestState; }
		bool IsConnected() const noexcept { return _tracker.IsConnected(); }
		std::uint64_t SessionEpoch() const noexcept { return _sessionEpoch; }
		void SetDiagnosticsCaptureEnabled(bool enabled) noexcept;
		NinjamTimingDiagnostics ObserveAppliedTimingReceipt(
			const std::optional<NinjamDesiredTimingReceipt>& receipt) noexcept;
		NinjamTimingDiagnostics Diagnostics() const noexcept;
		static const char* DiagnosticReasonName(NinjamTimingDiagnosticReason reason) noexcept;
		static const char* FollowPolicyName(NinjamLocalFollowPolicy policy) noexcept;
		static NinjamLocalFollowPolicy SelectLocalFollowPolicy(
			const std::optional<engine::QuantisationTiming>& localTiming,
			float remoteBpm) noexcept;

	private:
		static bool _MatchesRequest(const NinjamTiming& timing,
			const engine::QuantisationTiming& request) noexcept;
		static std::optional<NinjamTempoChange> _MakeProposal(const NinjamTiming& timing) noexcept;
		NinjamTimingUpdate _AcceptTempoChange(const NinjamTempoChange& change,
			const std::optional<engine::QuantisationTiming>& localTiming,
			utils::Timer& clock);
		NinjamTimingUpdate _PublishRemoteDesired(const NinjamTempoChange& change,
			NinjamLocalFollowPolicy policy, NinjamDesiredTimingIntent intent,
			bool remoteGridChanged) noexcept;
		NinjamTimingUpdate _PublishNoSync(NinjamNoSyncReason reason) noexcept;
		void _RecordEmittedCommand(NinjamEmittedCommand kind, std::uint64_t generation) noexcept;
		NinjamTimingUpdate _EnterNoSync(NinjamNoSyncReason reason,
			bool clearLatestProposal = true) noexcept;
		NinjamTimingUpdate _ExpireTempoRequest(
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent,
			utils::Timer& clock,
			std::chrono::steady_clock::time_point now);
		void _CaptureDiagnostic(NinjamTimingDiagnosticReason reason,
			std::uint64_t desiredVersion = 0u,
			std::uint64_t appliedVersion = 0u,
			std::uint64_t generation = 0u,
			long long valueSamps = 0,
			std::uint64_t limitSamps = 0u,
			std::uint64_t appliedSessionEpoch = 0u) noexcept;
		NinjamTimingTracker _tracker;
		NinjamTempoJoinOptions _options{};
		std::optional<NinjamTempoChange> _pendingTempoChange;
		std::optional<NinjamTempoChange> _ignoredTempoChange;
		std::optional<NinjamTempoChange> _latestObservedTempoChange;
		std::optional<engine::QuantisationTiming> _requestedTempo;
		std::optional<std::chrono::steady_clock::time_point> _firstSuccessfulTempoRequestSend;
		TempoRequestState _requestState = TempoRequestState::Idle;
		unsigned long _requestSentAtWrap = 0ul;
		std::uint64_t _observationOrdinal = 0u;
		std::uint64_t _requestSentObservationOrdinal = 0u;
		std::uint64_t _commandGeneration = 0u;
		std::uint64_t _desiredVersion = 0u;
		NinjamLocalFollowPolicy _activeFollowPolicy = NinjamLocalFollowPolicy::NoSync;
		bool _tempoRequestSendConfirmed = false;
		unsigned int _requestRetries = 0u;
		bool _joinAligned = false;
		bool _physicalAvailable = false;
		bool _timingValid = false;
		bool _noSyncActive = false;
		NinjamNoSyncReason _lastNoSyncReason = NinjamNoSyncReason::None;
		std::uint64_t _sessionEpoch = 0u;
		std::optional<std::chrono::steady_clock::time_point> _lastValidObservationAt;
		std::optional<std::uint64_t> _lastObservationAudioBlockStartSample;
		std::uint64_t _lastDiagnosticAppliedSessionEpoch = 0u;
		std::uint64_t _lastDiagnosticAppliedVersion = 0u;
		std::uint64_t _diagnosticLagSessionEpoch = 0u;
		std::uint64_t _diagnosticLagDesiredVersion = 0u;
		NinjamTimingDiagnostics _diagnostics;
	};
}
