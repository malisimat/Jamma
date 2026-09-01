#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include "NinjamTiming.h"
#include "NinjamTimingTracker.h"
#include "NinjamAudioTimingCommand.h"
#include "../engine/Quantiser.h"

namespace io
{
	struct UserConfig;
}

namespace ninjam
{
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

	struct NinjamPhaseCorrection
	{
		long long DeltaSamps = 0;
		std::uint64_t Generation = 0u;
		bool IsJoin = false;
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
	};

	struct NinjamTempoRequest
	{
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
	};

	struct NinjamClockSettings
	{
		unsigned long SeedLengthSamps = 0ul;
		unsigned int QuantiseSamps = 0u;
		unsigned int BeatsPerInterval = 0u;
		utils::Timer::QuantisationType Quantisation = utils::Timer::QUANTISE_OFF;
		unsigned int PhaseSamps = 0u;
		// Explicit generation for the timing replacement, independent of whether a
		// nonzero phase correction is present. Prevents a valid zero-phase tempo
		// replacement from being silently rejected by the audio generation gate.
		std::uint64_t Generation = 0u;
		std::uint64_t AudioBlockStartSample = 0u;
		NinjamLocalFollowPolicy LocalFollowPolicy = NinjamLocalFollowPolicy::ContinuousSync;
		float RemoteBpm = 0.0f;
		float LocalBpm = 0.0f;
		bool HasLocalTiming = false;
	};

	enum class NinjamNoSyncReason : std::uint8_t
	{
		None,
		StayLocal,
		Reconnect,
		Disconnect
	};

	struct NinjamTimingUpdate
	{
		std::optional<NinjamClockSettings> ClockSettings;
		std::optional<NinjamPhaseCorrection> PhaseCorrection;
		std::optional<NinjamTempoRequest> TempoRequest;
		bool InvalidatePendingCorrections = false;
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

	struct NinjamTimingDiagnostics
	{
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
		void Connect(const NinjamTempoJoinOptions& options,
			const std::optional<engine::QuantisationTiming>& localTiming) noexcept;
		void Disconnect() noexcept;
		NinjamTimingUpdate Observe(const NinjamTiming& timing,
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent,
		const io::UserConfig& config,
			utils::Timer& clock,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		std::optional<NinjamTempoChange> PendingTempoChange() const { return _pendingTempoChange; }
		NinjamTimingUpdate ResolveTempoChange(bool accept,
			const std::optional<engine::QuantisationTiming>& localTiming,
			utils::Timer& clock);
		void NotifyPhaseCorrectionConsumed() noexcept { ++_diagnostics.PhaseEventsConsumed; }
		// Feedback from the network layer after attempting to deliver a tempo
		// request. A failed send returns the request to Queued so the next interval
		// boundary re-sends it; success leaves it awaiting server acknowledgement.
		void NotifyTempoRequestSent(bool success,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
		TempoRequestState RequestState() const noexcept { return _requestState; }
		bool IsConnected() const noexcept { return _tracker.IsConnected(); }
		NinjamTimingDiagnostics Diagnostics() const noexcept;
		static const char* FollowPolicyName(NinjamLocalFollowPolicy policy) noexcept;
		static NinjamLocalFollowPolicy SelectLocalFollowPolicy(
			const std::optional<engine::QuantisationTiming>& localTiming,
			float remoteBpm) noexcept;

	private:
		static bool _SameTempo(const NinjamTempoChange& lhs, const NinjamTempoChange& rhs) noexcept;
		static bool _MatchesRequest(const NinjamTiming& timing,
			const engine::QuantisationTiming& request) noexcept;
		static std::optional<NinjamTempoChange> _MakeProposal(const NinjamTiming& timing,
			const io::UserConfig& config);
		NinjamTimingUpdate _AcceptTempoChange(const NinjamTempoChange& change,
			const std::optional<engine::QuantisationTiming>& localTiming,
			utils::Timer& clock);
		void _RecordEmittedCommand(NinjamEmittedCommand kind, std::uint64_t generation) noexcept;
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
		bool _tempoRequestSendConfirmed = false;
		unsigned int _requestRetries = 0u;
		bool _joinAligned = false;
		NinjamTimingDiagnostics _diagnostics;
	};
}
