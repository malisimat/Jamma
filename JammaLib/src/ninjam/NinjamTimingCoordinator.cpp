#include "NinjamTimingCoordinator.h"
#include "../include/Constants.h"
#include "../io/UserConfig.h"

#include <algorithm>
#include <cmath>

using namespace ninjam;

void NinjamTimingCoordinator::Connect(const NinjamTempoJoinOptions& options,
	const std::optional<timing::QuantisationTiming>& localTiming) noexcept
{
	_tracker.Connect();
	_options = options;
	_pendingTempoChange.reset();
	_ignoredTempoChange.reset();
	_requestedTempo = options.PushLocalTempoOnJoin ? localTiming : std::nullopt;
	_requestState = _requestedTempo.has_value() ? TempoRequestState::Queued : TempoRequestState::Idle;
	_requestSentAtWrap = 0ul;
	_observationOrdinal = 0u;
	_requestSentObservationOrdinal = 0u;
	_commandGeneration = 0u;
	_tempoRequestSendConfirmed = false;
	_requestRetries = 0u;
	_joinAligned = false;
	_diagnostics = {};
}

void NinjamTimingCoordinator::Disconnect() noexcept
{
	_tracker.Disconnect();
	_pendingTempoChange.reset();
	_ignoredTempoChange.reset();
	_requestedTempo.reset();
	_requestState = TempoRequestState::Idle;
	_requestSentAtWrap = 0ul;
	_observationOrdinal = 0u;
	_requestSentObservationOrdinal = 0u;
	_tempoRequestSendConfirmed = false;
	_requestRetries = 0u;
	_joinAligned = false;
	++_diagnostics.PhaseEventsInvalidated;
}

void NinjamTimingCoordinator::NotifyTempoRequestSent(bool success) noexcept
{
	if (_requestState != TempoRequestState::SentAwaitingOutcome)
		return;
	_tempoRequestSendConfirmed = success;
	if (success)
	{
		_requestSentObservationOrdinal = _observationOrdinal;
		return;
	}
	// A failed delivery returns the request to the queue so the next interval
	// boundary re-sends it without consuming a retry against a stale anchor.
	_requestState = TempoRequestState::Queued;
	_requestSentObservationOrdinal = 0u;
}

NinjamTimingUpdate NinjamTimingCoordinator::Observe(const NinjamTiming& timing,
	const std::optional<timing::QuantisationTiming>& localTiming,
	bool hasLocalContent,
	const io::UserConfig& config,
	utils::Timer& clock)
{
	NinjamTimingUpdate update;
	if (!timing.IsConnected || !timing.IsValid || timing.IntervalLengthSamps == 0u)
		return update;
	++_observationOrdinal;

	NinjamTimingObservation observation;
	observation.IntervalLengthSamps = timing.IntervalLengthSamps;
	observation.IntervalPositionSamps = timing.IntervalPositionSamps;
	// Preserve the callback-time local anchor (Timer absolute domain) so phase is
	// compared at the observation instant rather than at job-processing time (§2.7).
	observation.LocalSample = timing.LocalBlockStartSample;
	const auto event = _tracker.Observe(observation);
	const auto trackerDiagnostics = _tracker.Diagnostics();
	_diagnostics.ObservationsAccepted = trackerDiagnostics.ObservationsAccepted;
	_diagnostics.ObservationsRejected = trackerDiagnostics.ObservationsRejected;
	_diagnostics.DuplicateObservations = trackerDiagnostics.DuplicateObservations;
	_diagnostics.GenerationChanges = trackerDiagnostics.GenerationChanges;
	_diagnostics.WrapEvents = trackerDiagnostics.WrapEvents;
	_diagnostics.JoinEvents = trackerDiagnostics.JoinEvents;
	if (_pendingTempoChange.has_value())
	{
		const auto proposal = _MakeProposal(timing, config);
		if (proposal.has_value() && _SameTempo(_pendingTempoChange.value(), proposal.value()))
		{
			_pendingTempoChange->IntervalPositionSamps = timing.IntervalPositionSamps;
			_pendingTempoChange->AudioBlockStartSample = timing.AudioBlockStartSample;
		}
	}
	if (!event.has_value())
		return update;

	if (event->Type == NinjamTimingEventType::GenerationChanged)
	{
		_joinAligned = false;
		update.InvalidatePendingCorrections = true;
		++_diagnostics.PhaseEventsInvalidated;
		_RecordEmittedCommand(NinjamEmittedCommand::Invalidate, event->Generation);
	}
	if (!_joinAligned && clock.SeedSourceLength() == timing.IntervalLengthSamps)
	{
		_tracker.BeginJoinAlignment(clock.SampOffset());
		_joinAligned = true;
		_pendingTempoChange.reset();
		_ignoredTempoChange.reset();
	}
	if (event->Type == NinjamTimingEventType::GenerationChanged)
	{
		const auto proposal = _MakeProposal(timing, config);
		if (proposal.has_value())
		{
			++_diagnostics.TempoProposals;
			if (_requestState == TempoRequestState::SentAwaitingOutcome && _tempoRequestSendConfirmed && _requestedTempo.has_value()
				&& _observationOrdinal > _requestSentObservationOrdinal
				&& _MatchesRequest(proposal.value(), _requestedTempo.value()))
			{
				_requestedTempo.reset();
				_requestState = TempoRequestState::Acknowledged;
				++_diagnostics.TempoAcknowledged;
				return _AcceptTempoChange(proposal.value(), clock);
			}
			else if (_requestState == TempoRequestState::Queued
				|| _requestState == TempoRequestState::SentAwaitingOutcome)
			{
				// The server's old timing remains context while a local push is plausible.
				// Do not turn it into an apply-now proposal before the request resolves.
			}
			else if (!_options.PromptBeforeApplyingRemoteTempo || !hasLocalContent)
				return _AcceptTempoChange(proposal.value(), clock);
			else if (!_ignoredTempoChange.has_value() || !_SameTempo(_ignoredTempoChange.value(), proposal.value()))
			{
				_pendingTempoChange = proposal;
				update.PromptForTempoChange = true;
			}
		}
	}

	if (event->Type != NinjamTimingEventType::Wrap && event->Type != NinjamTimingEventType::Join)
		return update;
	if (clock.SeedSourceLength() != timing.IntervalLengthSamps
		&& (_pendingTempoChange.has_value() || _ignoredTempoChange.has_value()))
		return update;

	// A fresh valid observation whose tempo matches our request acknowledges it even
	// when the applied tempo left the rounded device interval length unchanged and
	// therefore produced no generation-change event (§2.6).
	if (_requestState == TempoRequestState::SentAwaitingOutcome && _tempoRequestSendConfirmed && _requestedTempo.has_value()
		&& _observationOrdinal > _requestSentObservationOrdinal
		&& timing.Bpi == _requestedTempo->Bpi
		&& std::abs(timing.Bpm - _requestedTempo->Bpm) < 0.01f)
	{
		_requestedTempo.reset();
		_requestState = TempoRequestState::Acknowledged;
		++_diagnostics.TempoAcknowledged;
	}

	if (_requestState == TempoRequestState::Queued && _requestedTempo.has_value())
	{
		update.TempoRequest = NinjamTempoRequest{ _requestedTempo->Bpm, _requestedTempo->Bpi };
		_requestSentAtWrap = event->RemoteWrapCount;
		_requestSentObservationOrdinal = 0u;
		_tempoRequestSendConfirmed = false;
		_requestState = TempoRequestState::SentAwaitingOutcome;
		_requestRetries = 0u;
		++_diagnostics.TempoRequestsSent;
	}
	else if (_requestState == TempoRequestState::SentAwaitingOutcome && _requestedTempo.has_value()
		&& event->RemoteWrapCount > (_requestSentAtWrap + 1ul))
	{
		if (_requestRetries < _options.MaxTempoRequestRetries)
		{
			// Re-send against the original anchor; the sent-at wrap is deliberately
			// not advanced so the expiry window measures from the first attempt.
			update.TempoRequest = NinjamTempoRequest{ _requestedTempo->Bpm, _requestedTempo->Bpi };
			++_requestRetries;
			++_diagnostics.TempoRequestRetries;
		}
		else
		{
			_requestState = TempoRequestState::Expired;
			_requestedTempo.reset();
			++_diagnostics.TempoRequestsExpired;
			const auto proposal = _MakeProposal(timing, config);
			if (proposal.has_value())
			{
				if (!_options.PromptBeforeApplyingRemoteTempo || !hasLocalContent)
					return _AcceptTempoChange(proposal.value(), clock);
				_pendingTempoChange = proposal;
				update.PromptForTempoChange = true;
				return update;
			}
		}
	}

	const auto seedLength = clock.SeedSourceLength();
	if (seedLength == 0ul)
		return update;
	// Project the current Timer offset back to the observation's callback anchor so
	// remote (observation-time) and local phase are compared at the same instant.
	// Both anchors live in the Timer absolute-sample domain. A zero anchor means no
	// callback anchor was supplied, so compare against the live offset directly.
	unsigned int localOffset = clock.SampOffset();
	if (observation.LocalSample != 0u)
	{
		const auto nowAnchor = static_cast<std::uint64_t>(
			clock.AbsoluteSamplePos(static_cast<unsigned long>(observation.LocalSample)));
		if (nowAnchor >= observation.LocalSample)
		{
			const auto elapsed = static_cast<unsigned int>(
				(nowAnchor - observation.LocalSample) % seedLength);
			_diagnostics.MaxObservationAgeSamps = std::max(_diagnostics.MaxObservationAgeSamps,
				static_cast<std::uint64_t>(nowAnchor - observation.LocalSample));
			localOffset = static_cast<unsigned int>(
				(static_cast<unsigned long>(localOffset) + seedLength - elapsed) % seedLength);
		}
	}
	const auto delta = event->Type == NinjamTimingEventType::Join ? event->PhaseDeltaSamps :
		SignedCircularDifference(localOffset, event->RemotePositionSamps,
			static_cast<unsigned int>(seedLength));
	const auto magnitude = delta < 0 ? -delta : delta;
	_diagnostics.MaxPhaseErrorSamps = std::max(_diagnostics.MaxPhaseErrorSamps, magnitude);
	const auto bucket = magnitude <= 1 ? 0u : magnitude <= 16 ? 1u : magnitude <= 128 ? 2u : 3u;
	++_diagnostics.PhaseErrorBuckets[bucket];
	// A join can legitimately align anywhere within the interval, so it is bounded
	// only by the seed half-interval (the circular difference already guarantees
	// that). Ongoing phase discipline from wrap events is expected to be tiny, so
	// keep the small buffer-scale cap to reject anomalous drift on that path only.
	const bool isJoin = event->Type == NinjamTimingEventType::Join;
	const auto safetyLimit = isJoin
		? static_cast<long long>(seedLength / 2ul)
		: static_cast<long long>(constants::DefaultBufferSizeSamps * 2u);
	if (magnitude > safetyLimit)
	{
		++_diagnostics.SafetyLimitRejections;
		return update;
	}
	if (delta != 0 || event->Type == NinjamTimingEventType::Wrap)
	{
		update.PhaseCorrection = NinjamPhaseCorrection{ delta, ++_commandGeneration,
			event->Type == NinjamTimingEventType::Join };
		++_diagnostics.PhaseEventsQueued;
		_RecordEmittedCommand(isJoin ? NinjamEmittedCommand::Join : NinjamEmittedCommand::Discipline,
			event->Generation);
	}
	return update;
}

void NinjamTimingCoordinator::BeginJoinAlignment(utils::Timer& clock) noexcept
{
	_tracker.BeginJoinAlignment(clock.SampOffset());
}

bool NinjamTimingCoordinator::_SameTempo(const NinjamTempoChange& lhs, const NinjamTempoChange& rhs) noexcept
{
	return lhs.IntervalLengthSamps == rhs.IntervalLengthSamps
		&& lhs.SourceSampleRate == rhs.SourceSampleRate
		&& lhs.GrainSamps == rhs.GrainSamps
		&& lhs.Bpi == rhs.Bpi
		&& std::abs(lhs.Bpm - rhs.Bpm) < 0.01f;
}

bool NinjamTimingCoordinator::_MatchesRequest(const NinjamTempoChange& proposal,
	const timing::QuantisationTiming& request) noexcept
{
	return proposal.Bpi == request.Bpi && std::abs(proposal.Bpm - request.Bpm) < 0.01f;
}

std::optional<NinjamTempoChange> NinjamTimingCoordinator::_MakeProposal(const NinjamTiming& timing,
	const io::UserConfig& config)
{
	if (!timing.IsValid || timing.IntervalLengthSamps == 0u || timing.DeviceSampleRate == 0u)
		return std::nullopt;
	const auto derived = config.DeduceLoopTiming(timing.IntervalLengthSamps, timing.DeviceSampleRate);
	if (!derived.has_value() || derived->GrainSamps == 0u)
		return std::nullopt;
	return NinjamTempoChange{ timing.IntervalLengthSamps, timing.DeviceSampleRate, derived->GrainSamps,
		derived->Bpm, derived->Bpi, timing.IntervalPositionSamps, timing.AudioBlockStartSample };
}

NinjamTimingUpdate NinjamTimingCoordinator::_AcceptTempoChange(const NinjamTempoChange& change,
	utils::Timer& clock)
{
	NinjamTimingUpdate update;
	const auto generation = ++_commandGeneration;
	update.ClockSettings = NinjamClockSettings{ change.IntervalLengthSamps, change.GrainSamps,
		utils::Timer::QUANTISE_POWER, change.IntervalPositionSamps, generation,
		change.AudioBlockStartSample };
	_pendingTempoChange.reset();
	_ignoredTempoChange.reset();
	++_diagnostics.TempoAccepted;
	_RecordEmittedCommand(NinjamEmittedCommand::Replace, generation);
	return update;
}

NinjamTimingUpdate NinjamTimingCoordinator::ResolveTempoChange(bool accept,
	const std::optional<timing::QuantisationTiming>& localTiming,
	utils::Timer& clock)
{
	(void)localTiming;
	if (!_pendingTempoChange.has_value())
		return {};
	const auto change = _pendingTempoChange.value();
	if (!accept)
	{
		_ignoredTempoChange = change;
		_pendingTempoChange.reset();
		++_diagnostics.TempoRejected;
		return {};
	}
	return _AcceptTempoChange(change, clock);
}

NinjamTimingDiagnostics NinjamTimingCoordinator::Diagnostics() const noexcept
{
	return _diagnostics;
}

void NinjamTimingCoordinator::_RecordEmittedCommand(NinjamEmittedCommand kind,
	std::uint64_t generation) noexcept
{
	++_diagnostics.CommandsEmitted;
	_diagnostics.LastCommandGeneration = generation;
	_diagnostics.LastCommandType = kind;
}