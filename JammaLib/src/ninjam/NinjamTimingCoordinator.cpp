#include "NinjamTimingCoordinator.h"
#include "NinjamSession.h"
#include "../include/Constants.h"
#include "../io/UserConfig.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace ninjam;

NinjamTimingUpdate NinjamTimingCoordinator::Connect(const NinjamTempoJoinOptions& options,
	const std::optional<engine::QuantisationTiming>& localTiming) noexcept
{
	_tracker.Connect();
	_options = options;
	_pendingTempoChange.reset();
	_ignoredTempoChange.reset();
	_latestObservedTempoChange.reset();
	_requestedTempo = options.PushLocalTempoOnJoin ? localTiming : std::nullopt;
	_firstSuccessfulTempoRequestSend.reset();
	_requestState = _requestedTempo.has_value() ? TempoRequestState::Queued : TempoRequestState::Idle;
	_requestSentAtWrap = 0ul;
	_observationOrdinal = 0u;
	_requestSentObservationOrdinal = 0u;
	_commandGeneration = 0u;
	_tempoRequestSendConfirmed = false;
	_requestRetries = 0u;
	_joinAligned = false;
	_physicalAvailable = false;
	_timingValid = false;
	_noSyncActive = true;
	_lastNoSyncReason = NinjamNoSyncReason::Reconnect;
	_activeFollowPolicy = NinjamLocalFollowPolicy::NoSync;
	_lastValidObservationAt.reset();
	_lastObservationAudioBlockStartSample.reset();
	_diagnostics = {};
	return _PublishNoSync(NinjamNoSyncReason::Reconnect);
}

NinjamTimingUpdate NinjamTimingCoordinator::Disconnect() noexcept
{
	if (!_tracker.IsConnected() && _noSyncActive)
		return {};
	_tracker.Disconnect();
	_pendingTempoChange.reset();
	_ignoredTempoChange.reset();
	_latestObservedTempoChange.reset();
	_requestedTempo.reset();
	_firstSuccessfulTempoRequestSend.reset();
	_requestState = TempoRequestState::Idle;
	_requestSentAtWrap = 0ul;
	_observationOrdinal = 0u;
	_requestSentObservationOrdinal = 0u;
	_tempoRequestSendConfirmed = false;
	_requestRetries = 0u;
	_joinAligned = false;
	_physicalAvailable = false;
	_timingValid = false;
	_noSyncActive = true;
	_lastNoSyncReason = NinjamNoSyncReason::Disconnect;
	_lastValidObservationAt.reset();
	_lastObservationAudioBlockStartSample.reset();
	++_diagnostics.PhaseEventsInvalidated;
	_activeFollowPolicy = NinjamLocalFollowPolicy::NoSync;
	return _PublishNoSync(NinjamNoSyncReason::Disconnect);
}

NinjamTimingUpdate NinjamTimingCoordinator::ObserveSessionStatus(
	const NinjamSessionTimingStatus& status,
	const NinjamTempoJoinOptions& options,
	const std::optional<engine::QuantisationTiming>& localTiming) noexcept
{
	if (status.IsAvailable)
	{
		if (status.SessionEpoch == 0u || status.SessionEpoch <= _sessionEpoch)
		{
			return {};
		}
		Connect(options, localTiming);
		_physicalAvailable = true;
		_sessionEpoch = status.SessionEpoch;
		return _PublishNoSync(NinjamNoSyncReason::Reconnect);
	}

	if (!_physicalAvailable || status.SessionEpoch != _sessionEpoch)
		return {};

	_physicalAvailable = false;
	return _EnterNoSync(NinjamNoSyncReason::PhysicalLoss);
}

void NinjamTimingCoordinator::NotifyTempoRequestSent(bool success,
	std::chrono::steady_clock::time_point now) noexcept
{
	if (_requestState != TempoRequestState::SentAwaitingOutcome)
		return;
	_tempoRequestSendConfirmed = success;
	if (success)
	{
		_requestSentObservationOrdinal = _observationOrdinal;
		if (!_firstSuccessfulTempoRequestSend.has_value())
			_firstSuccessfulTempoRequestSend = now;
		return;
	}
	// A failed delivery returns the request to the queue so the next interval
	// boundary re-sends it without consuming a retry against a stale anchor.
	_requestState = TempoRequestState::Queued;
	_requestSentObservationOrdinal = 0u;
}

NinjamTimingUpdate NinjamTimingCoordinator::Observe(const NinjamTiming& timing,
	const std::optional<engine::QuantisationTiming>& localTiming,
	bool hasLocalContent,
	const io::UserConfig& config,
	utils::Timer& clock,
	std::chrono::steady_clock::time_point now)
{
	NinjamTimingUpdate update;
	if (!timing.IsConnected || !timing.IsValid || timing.IntervalLengthSamps == 0u)
		return _timingValid ? _EnterNoSync(NinjamNoSyncReason::InvalidTiming) : update;
	// Remote and local phase must describe the same audio boundary. An observation
	// without either explicit anchor is deferred; never substitute a live Timer
	// read taken later on the job thread.
	if (!timing.HasAudioBlockStartSample || !timing.HasLocalTransport)
		return update;
	const auto isFreshObservation = !_lastObservationAudioBlockStartSample.has_value()
		|| _lastObservationAudioBlockStartSample.value() != timing.AudioBlockStartSample;
	if (_noSyncActive && _lastNoSyncReason == NinjamNoSyncReason::ObservationDeadline
		&& !isFreshObservation)
	{
		return update;
	}
	if (!_tracker.IsConnected())
		_tracker.Connect();
	_timingValid = true;
	_noSyncActive = false;
	_lastNoSyncReason = NinjamNoSyncReason::None;
	if (isFreshObservation || !_lastValidObservationAt.has_value())
	{
		_lastObservationAudioBlockStartSample = timing.AudioBlockStartSample;
		_lastValidObservationAt = now;
	}
	_diagnostics.MaxObservationAgeSamps = std::max(_diagnostics.MaxObservationAgeSamps,
		timing.ObservationAgeSamps);
	++_observationOrdinal;
	_latestObservedTempoChange = _MakeProposal(timing, config);

	update = _ExpireTempoRequest(localTiming, hasLocalContent, clock, now);
	if (update.DesiredTransport.has_value() || update.PromptForTempoChange)
		return update;

	NinjamTimingObservation observation;
	observation.IntervalLengthSamps = timing.IntervalLengthSamps;
	observation.IntervalPositionSamps = timing.IntervalPositionSamps;
	// Preserve the callback-time local anchor (Timer absolute domain) so phase is
	// compared at the observation instant rather than at job-processing time (§2.7).
	observation.LocalSample = timing.LocalTransport.AbsoluteSamplePos;
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
	if (_requestState == TempoRequestState::SentAwaitingOutcome && _tempoRequestSendConfirmed
		&& _requestedTempo.has_value() && _latestObservedTempoChange.has_value()
		&& _observationOrdinal > _requestSentObservationOrdinal
		&& _MatchesRequest(timing, _requestedTempo.value()))
	{
		_requestedTempo.reset();
		_requestState = TempoRequestState::Acknowledged;
		++_diagnostics.TempoAcknowledged;
		return _AcceptTempoChange(_latestObservedTempoChange.value(), localTiming, clock);
	}

	if (!event.has_value())
		return update;

	if (event->Type == NinjamTimingEventType::GenerationChanged)
	{
		_joinAligned = false;
		++_diagnostics.PhaseEventsInvalidated;
		_RecordEmittedCommand(NinjamEmittedCommand::Invalidate, event->Generation);
	}
	if (!_joinAligned
		&& timing.LocalTransport.MasterLengthSamps == timing.IntervalLengthSamps)
	{
		_tracker.BeginJoinAlignment(static_cast<unsigned long>(
			timing.LocalTransport.MasterPhaseSamps));
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
			if (_requestState == TempoRequestState::Queued
				|| _requestState == TempoRequestState::SentAwaitingOutcome)
			{
				// The server's old timing remains context while a local push is plausible.
				// Do not turn it into an apply-now proposal before the request resolves.
			}
			else if (!_options.PromptBeforeApplyingRemoteTempo || !hasLocalContent)
				return _AcceptTempoChange(proposal.value(), localTiming, clock);
			else if (!_ignoredTempoChange.has_value() || !_SameTempo(_ignoredTempoChange.value(), proposal.value()))
			{
				_pendingTempoChange = proposal;
				update.PromptForTempoChange = true;
				update.DesiredTransport = _PublishNoSync(NinjamNoSyncReason::None).DesiredTransport;
			}
		}
	}

	if (event->Type != NinjamTimingEventType::Wrap && event->Type != NinjamTimingEventType::Join)
		return update;
	if (timing.LocalTransport.MasterLengthSamps != timing.IntervalLengthSamps
		&& (_pendingTempoChange.has_value() || _ignoredTempoChange.has_value()))
		return update;

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
					return _AcceptTempoChange(proposal.value(), localTiming, clock);
				_pendingTempoChange = proposal;
				update.PromptForTempoChange = true;
				return update;
			}
		}
	}

	const auto seedLength = timing.LocalTransport.MasterLengthSamps;
	if (seedLength == 0u || seedLength > (std::numeric_limits<unsigned int>::max)())
		return update;
	const auto localOffset = static_cast<unsigned int>(
		timing.LocalTransport.MasterPhaseSamps % seedLength);
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
		? static_cast<long long>(seedLength / 2u)
		: static_cast<long long>(constants::DefaultBufferSizeSamps * 2u);
	if (magnitude > safetyLimit)
	{
		++_diagnostics.SafetyLimitRejections;
		return update;
	}
	if ((delta != 0 || event->Type == NinjamTimingEventType::Wrap)
		&& _latestObservedTempoChange.has_value())
	{
		auto desiredUpdate = _PublishRemoteDesired(_latestObservedTempoChange.value(), _activeFollowPolicy,
			event->Type == NinjamTimingEventType::Join
				? NinjamDesiredTimingIntent::JoinAlignment
				: NinjamDesiredTimingIntent::PhaseDiscipline, false);
		update.DesiredTransport = desiredUpdate.DesiredTransport;
		update.RemoteGrid = desiredUpdate.RemoteGrid;
		++_diagnostics.PhaseEventsQueued;
		_RecordEmittedCommand(isJoin ? NinjamEmittedCommand::Join : NinjamEmittedCommand::Discipline,
			event->Generation);
	}
	return update;
}

NinjamTimingUpdate NinjamTimingCoordinator::Tick(
	const std::optional<engine::QuantisationTiming>& localTiming,
	bool hasLocalContent,
	utils::Timer& clock,
	std::chrono::steady_clock::time_point now)
{
	auto update = _ExpireTempoRequest(localTiming, hasLocalContent, clock, now);
	if (!_timingValid || !_lastValidObservationAt.has_value()
		|| now - _lastValidObservationAt.value() < _options.TempoRequestDeadline)
	{
		return update;
	}

	auto noSync = _EnterNoSync(NinjamNoSyncReason::ObservationDeadline, false);
	update.DesiredTransport = noSync.DesiredTransport;
	update.NoSyncReason = noSync.NoSyncReason;
	return update;
}

NinjamTimingUpdate NinjamTimingCoordinator::_ExpireTempoRequest(
	const std::optional<engine::QuantisationTiming>& localTiming,
	bool hasLocalContent,
	utils::Timer& clock,
	std::chrono::steady_clock::time_point now)
{
	NinjamTimingUpdate update;
	if (_requestState != TempoRequestState::SentAwaitingOutcome
		|| !_firstSuccessfulTempoRequestSend.has_value()
		|| now - _firstSuccessfulTempoRequestSend.value() < _options.TempoRequestDeadline)
	{
		return update;
	}

	const bool preservePushedLocalTransport = _requestedTempo.has_value();
	_requestState = TempoRequestState::Expired;
	_requestedTempo.reset();
	++_diagnostics.TempoRequestsExpired;
	if (!_latestObservedTempoChange.has_value())
		return update;
	if (!_options.PromptBeforeApplyingRemoteTempo && !hasLocalContent && !preservePushedLocalTransport)
		return _AcceptTempoChange(_latestObservedTempoChange.value(), localTiming, clock);
	_pendingTempoChange = _latestObservedTempoChange;
	update.PromptForTempoChange = true;
	return update;
}

NinjamTimingUpdate NinjamTimingCoordinator::_EnterNoSync(
	NinjamNoSyncReason reason, bool clearLatestProposal) noexcept
{
	if (_noSyncActive)
	{
		if (reason == _lastNoSyncReason)
			return {};
		_tracker.Disconnect();
		_lastNoSyncReason = reason;
		++_diagnostics.PhaseEventsInvalidated;
		auto update = _PublishNoSync(reason);
		_RecordEmittedCommand(NinjamEmittedCommand::Invalidate, _commandGeneration);
		return update;
	}

	_tracker.Disconnect();
	_ignoredTempoChange.reset();
	const auto preservesExpiredRequest = reason == NinjamNoSyncReason::ObservationDeadline;
	if (!preservesExpiredRequest)
		_pendingTempoChange.reset();
	if (clearLatestProposal && !preservesExpiredRequest)
		_latestObservedTempoChange.reset();
	_requestedTempo.reset();
	_firstSuccessfulTempoRequestSend.reset();
	if (!preservesExpiredRequest)
		_requestState = TempoRequestState::Idle;
	_requestSentAtWrap = 0ul;
	_observationOrdinal = 0u;
	_requestSentObservationOrdinal = 0u;
	_tempoRequestSendConfirmed = false;
	_requestRetries = 0u;
	_joinAligned = false;
	_timingValid = false;
	_noSyncActive = true;
	_lastNoSyncReason = reason;
	_lastValidObservationAt.reset();
	if (reason != NinjamNoSyncReason::ObservationDeadline)
		_lastObservationAudioBlockStartSample.reset();
	++_diagnostics.PhaseEventsInvalidated;

	NinjamTimingUpdate update;
	update = _PublishNoSync(reason);
	update.NoSyncReason = reason;
	_RecordEmittedCommand(NinjamEmittedCommand::Invalidate, _commandGeneration);
	return update;
}

bool NinjamTimingCoordinator::_SameTempo(const NinjamTempoChange& lhs, const NinjamTempoChange& rhs) noexcept
{
	return lhs.IntervalLengthSamps == rhs.IntervalLengthSamps
		&& lhs.SourceSampleRate == rhs.SourceSampleRate
		&& lhs.GrainSamps == rhs.GrainSamps
		&& lhs.Bpi == rhs.Bpi
		&& std::abs(lhs.Bpm - rhs.Bpm) < 0.01f;
}

bool NinjamTimingCoordinator::_MatchesRequest(const NinjamTiming& timing,
	const engine::QuantisationTiming& request) noexcept
{
	return timing.Bpi == request.Bpi
		&& std::abs(timing.Bpm - request.Bpm)
		<= NinjamTempoJoinOptions::TempoRequestAcknowledgementToleranceBpm;
}

std::optional<NinjamTempoChange> NinjamTimingCoordinator::_MakeProposal(const NinjamTiming& timing,
	const io::UserConfig& config)
{
	if (!timing.IsValid || timing.IntervalLengthSamps == 0u || timing.DeviceSampleRate == 0u)
		return std::nullopt;
	if (timing.Bpi == 0u)
	{
		// Older/partial observations did not carry BPI. Retain the local deduction
		// only for that compatibility case; a supplied remote BPI is never replaced.
		const auto derived = config.DeduceLoopTiming(timing.IntervalLengthSamps, timing.DeviceSampleRate);
		if (!derived.has_value() || derived->GrainSamps == 0u)
			return std::nullopt;
		return NinjamTempoChange{ timing.IntervalLengthSamps, timing.DeviceSampleRate, derived->GrainSamps,
			derived->Bpm, derived->Bpi, timing.IntervalPositionSamps, timing.AudioBlockStartSample };
	}
	// The server BPI is authoritative. Local deduction is only for the initial
	// local seed and must not rewrite the accepted remote grid.
	const auto grain = static_cast<unsigned int>((static_cast<std::uint64_t>(timing.IntervalLengthSamps)
		+ timing.Bpi / 2u) / timing.Bpi);
	if (grain == 0u)
		return std::nullopt;
	return NinjamTempoChange{ timing.IntervalLengthSamps, timing.DeviceSampleRate, grain,
		timing.Bpm, timing.Bpi, timing.IntervalPositionSamps, timing.AudioBlockStartSample };
}

NinjamTimingUpdate NinjamTimingCoordinator::_PublishRemoteDesired(
	const NinjamTempoChange& change, NinjamLocalFollowPolicy policy,
	NinjamDesiredTimingIntent intent, bool remoteGridChanged) noexcept
{
	NinjamTimingUpdate update;
	NinjamDesiredTransportState desired;
	desired.Version = ++_desiredVersion;
	desired.SessionEpoch = _sessionEpoch;
	desired.Generation = ++_commandGeneration;
	desired.Intent = intent;
	desired.LocalFollowPolicy = policy;
	desired.HasRemoteTiming = true;
	desired.IntervalLengthSamps = change.IntervalLengthSamps;
	desired.GrainSamps = change.GrainSamps;
	desired.BeatsPerInterval = change.Bpi;
	desired.TempoBpm = change.Bpm;
	desired.Quantisation = utils::Timer::QUANTISE_POWER;
	desired.RemotePhaseSamps = change.IntervalPositionSamps;
	desired.HasObservationSample = true;
	desired.ObservationSample = change.AudioBlockStartSample;
	update.DesiredTransport = desired;
	if (remoteGridChanged)
	{
		const auto origin = static_cast<std::int64_t>(change.AudioBlockStartSample)
			- static_cast<std::int64_t>(change.IntervalPositionSamps);
		update.RemoteGrid = NinjamRemoteGridPublication{
			engine::RemoteTransportGeometry{ change.IntervalLengthSamps, change.Bpi,
				change.IntervalPositionSamps, change.Bpm, desired.Generation }, origin };
	}
	_activeFollowPolicy = policy;
	return update;
}

NinjamTimingUpdate NinjamTimingCoordinator::_PublishNoSync(NinjamNoSyncReason reason) noexcept
{
	NinjamTimingUpdate update;
	NinjamDesiredTransportState desired;
	desired.Version = ++_desiredVersion;
	desired.SessionEpoch = _sessionEpoch;
	desired.Generation = _commandGeneration;
	desired.Intent = NinjamDesiredTimingIntent::NoSync;
	desired.LocalFollowPolicy = NinjamLocalFollowPolicy::NoSync;
	update.DesiredTransport = desired;
	update.NoSyncReason = reason;
	_activeFollowPolicy = NinjamLocalFollowPolicy::NoSync;
	return update;
}

NinjamTimingUpdate NinjamTimingCoordinator::_AcceptTempoChange(const NinjamTempoChange& change,
	const std::optional<engine::QuantisationTiming>& localTiming,
	utils::Timer& clock)
{
	const auto policy = SelectLocalFollowPolicy(localTiming, change.Bpm);
	auto update = _PublishRemoteDesired(change, policy,
		NinjamDesiredTimingIntent::Replacement, true);
	const auto generation = update.DesiredTransport->Generation;
	_pendingTempoChange.reset();
	_ignoredTempoChange.reset();
	++_diagnostics.TempoAccepted;
	_RecordEmittedCommand(NinjamEmittedCommand::Replace, generation);
	return update;
}

NinjamTimingUpdate NinjamTimingCoordinator::ResolveTempoChange(bool accept,
	const std::optional<engine::QuantisationTiming>& localTiming,
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
		auto update = _PublishNoSync(NinjamNoSyncReason::StayLocal);
		update.NoSyncReason = NinjamNoSyncReason::StayLocal;
		_RecordEmittedCommand(NinjamEmittedCommand::Invalidate, _commandGeneration);
		return update;
	}
	return _AcceptTempoChange(change, localTiming, clock);
}

NinjamTimingDiagnostics NinjamTimingCoordinator::Diagnostics() const noexcept
{
	return _diagnostics;
}

const char* NinjamTimingCoordinator::FollowPolicyName(NinjamLocalFollowPolicy policy) noexcept
{
	switch (policy)
	{
	case NinjamLocalFollowPolicy::ContinuousSync: return "continuous-sync";
	case NinjamLocalFollowPolicy::BlockSync: return "block-sync";
	default: return "no-sync";
	}
}

NinjamLocalFollowPolicy NinjamTimingCoordinator::SelectLocalFollowPolicy(
	const std::optional<engine::QuantisationTiming>& localTiming, float remoteBpm) noexcept
{
	return !localTiming.has_value()
		|| std::abs(remoteBpm - localTiming->Bpm)
		<= NinjamTempoJoinOptions::TempoRequestAcknowledgementToleranceBpm
		? NinjamLocalFollowPolicy::ContinuousSync
		: NinjamLocalFollowPolicy::BlockSync;
}

void NinjamTimingCoordinator::_RecordEmittedCommand(NinjamEmittedCommand kind,
	std::uint64_t generation) noexcept
{
	++_diagnostics.CommandsEmitted;
	_diagnostics.LastCommandGeneration = generation;
	_diagnostics.LastCommandType = kind;
}
