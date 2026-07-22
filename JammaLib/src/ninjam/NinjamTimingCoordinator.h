#pragma once

#include <cstdint>
#include <optional>
#include "NinjamTiming.h"
#include "NinjamTimingTracker.h"
#include "../timing/TimingQuantiser.h"

namespace io
{
	struct UserConfig;
}

namespace ninjam
{
	struct NinjamTempoJoinOptions
	{
		bool PushLocalTempoOnJoin = false;
		bool PromptBeforeApplyingRemoteTempo = true;
		// Maximum re-sends of a local tempo request before it is abandoned. Retries
		// do not move the original sent-at anchor (§2.6/§3.5).
		unsigned int MaxTempoRequestRetries = 3u;
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
		utils::Timer::QuantisationType Quantisation = utils::Timer::QUANTISE_OFF;
		unsigned int PhaseSamps = 0u;
		// Explicit generation for the timing replacement, independent of whether a
		// nonzero phase correction is present. Prevents a valid zero-phase tempo
		// replacement from being silently rejected by the audio generation gate.
		std::uint64_t Generation = 0u;
	};

	struct NinjamTimingUpdate
	{
		std::optional<NinjamClockSettings> ClockSettings;
		std::optional<NinjamPhaseCorrection> PhaseCorrection;
		std::optional<NinjamTempoRequest> TempoRequest;
		bool InvalidatePendingCorrections = false;
		bool PromptForTempoChange = false;
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
	};

	class NinjamTimingCoordinator
	{
	public:
		void Connect(const NinjamTempoJoinOptions& options,
			const std::optional<timing::QuantisationTiming>& localTiming) noexcept;
		void Disconnect() noexcept;
		NinjamTimingUpdate Observe(const NinjamTiming& timing,
			const std::optional<timing::QuantisationTiming>& localTiming,
			bool hasLocalContent,
		const io::UserConfig& config,
		utils::Timer& clock);
		void BeginJoinAlignment(utils::Timer& clock) noexcept;
		std::optional<NinjamTempoChange> PendingTempoChange() const { return _pendingTempoChange; }
		NinjamTimingUpdate ResolveTempoChange(bool accept,
			const std::optional<timing::QuantisationTiming>& localTiming,
			utils::Timer& clock);
		void NotifyPhaseCorrectionConsumed() noexcept { ++_diagnostics.PhaseEventsConsumed; }
		// Feedback from the network layer after attempting to deliver a tempo
		// request. A failed send returns the request to Queued so the next interval
		// boundary re-sends it; success leaves it awaiting server acknowledgement.
		void NotifyTempoRequestSent(bool success) noexcept;
		TempoRequestState RequestState() const noexcept { return _requestState; }
		bool IsConnected() const noexcept { return _tracker.IsConnected(); }
		NinjamTimingDiagnostics Diagnostics() const noexcept;

	private:
		static bool _SameTempo(const NinjamTempoChange& lhs, const NinjamTempoChange& rhs) noexcept;
		static bool _MatchesRequest(const NinjamTempoChange& proposal,
			const timing::QuantisationTiming& request) noexcept;
		static std::optional<NinjamTempoChange> _MakeProposal(const NinjamTiming& timing,
			const io::UserConfig& config);
		NinjamTimingUpdate _AcceptTempoChange(const NinjamTempoChange& change,
			utils::Timer& clock);
		NinjamTimingTracker _tracker;
		NinjamTempoJoinOptions _options{};
		std::optional<NinjamTempoChange> _pendingTempoChange;
		std::optional<NinjamTempoChange> _ignoredTempoChange;
		std::optional<timing::QuantisationTiming> _requestedTempo;
		TempoRequestState _requestState = TempoRequestState::Idle;
		unsigned long _requestSentAtWrap = 0ul;
		unsigned int _requestRetries = 0u;
		bool _joinAligned = false;
		NinjamTimingDiagnostics _diagnostics;
	};
}