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
	_locallyRequestedTempo = options.PushLocalTempoOnJoin ? localTiming : std::nullopt;
	_joinPushAwaitingOutcome = _locallyRequestedTempo.has_value();
	_joinPushSent = false;
	_joinPushWrap = 0ul;
	_joinAligned = false;
	_diagnostics = {};
}

void NinjamTimingCoordinator::Disconnect() noexcept
{
	_tracker.Disconnect();
	_pendingTempoChange.reset();
	_ignoredTempoChange.reset();
	_locallyRequestedTempo.reset();
	_joinPushAwaitingOutcome = false;
	_joinPushSent = false;
	_joinPushWrap = 0ul;
	_joinAligned = false;
	++_diagnostics.PhaseEventsInvalidated;
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

	NinjamTimingObservation observation;
	observation.IntervalLengthSamps = timing.IntervalLengthSamps;
	observation.IntervalPositionSamps = timing.IntervalPositionSamps;
	observation.LocalSample = clock.AbsoluteSamplePos(0ul);
	const auto event = _tracker.Observe(observation);
	const auto trackerDiagnostics = _tracker.Diagnostics();
	_diagnostics.ObservationsAccepted = trackerDiagnostics.ObservationsAccepted;
	_diagnostics.ObservationsRejected = trackerDiagnostics.ObservationsRejected;
	_diagnostics.DuplicateObservations = trackerDiagnostics.DuplicateObservations;
	_diagnostics.GenerationChanges = trackerDiagnostics.GenerationChanges;
	_diagnostics.WrapEvents = trackerDiagnostics.WrapEvents;
	_diagnostics.JoinEvents = trackerDiagnostics.JoinEvents;
	if (!event.has_value())
		return update;

	if (event->Type == NinjamTimingEventType::GenerationChanged)
	{
		_joinAligned = false;
		update.InvalidatePendingCorrections = true;
		++_diagnostics.PhaseEventsInvalidated;
	}
	if (!_joinAligned && clock.SeedSourceLength() == timing.IntervalLengthSamps)
	{
		_tracker.BeginJoinAlignment(clock.SampOffset());
		_joinAligned = true;
	}
	if (event->Type == NinjamTimingEventType::GenerationChanged)
	{
		const auto proposal = _MakeProposal(timing, config);
		if (proposal.has_value())
		{
			++_diagnostics.TempoProposals;
			if (_locallyRequestedTempo.has_value() && _joinPushSent
				&& _MatchesRequest(proposal.value(), _locallyRequestedTempo.value()))
			{
				_locallyRequestedTempo.reset();
				_joinPushAwaitingOutcome = false;
				++_diagnostics.TempoAcknowledged;
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

	if (_locallyRequestedTempo.has_value() && (!_joinPushAwaitingOutcome || event->RemoteWrapCount > _joinPushWrap))
	{
		update.TempoRequest = NinjamTempoRequest{ _locallyRequestedTempo->Bpm, _locallyRequestedTempo->Bpi };
		_joinPushWrap = event->RemoteWrapCount;
		_joinPushAwaitingOutcome = true;
		_joinPushSent = true;
	}
	else if (_joinPushAwaitingOutcome && event->RemoteWrapCount > (_joinPushWrap + 1ul))
	{
		_joinPushAwaitingOutcome = false;
		_locallyRequestedTempo.reset();
	}

	const auto seedLength = clock.SeedSourceLength();
	if (seedLength == 0ul)
		return update;
	const auto delta = event->Type == NinjamTimingEventType::Join ? event->PhaseDeltaSamps :
		SignedCircularDifference(clock.SampOffset(), event->RemotePositionSamps,
			static_cast<unsigned int>(seedLength));
	const auto magnitude = delta < 0 ? -delta : delta;
	_diagnostics.MaxPhaseErrorSamps = std::max(_diagnostics.MaxPhaseErrorSamps, magnitude);
	const auto bucket = magnitude <= 1 ? 0u : magnitude <= 16 ? 1u : magnitude <= 128 ? 2u : 3u;
	++_diagnostics.PhaseErrorBuckets[bucket];
	if (magnitude > static_cast<long long>(constants::DefaultBufferSizeSamps * 2u))
	{
		++_diagnostics.SafetyLimitRejections;
		return update;
	}
	if (delta != 0)
	{
		update.PhaseCorrection = NinjamPhaseCorrection{ delta, event->Generation,
			event->Type == NinjamTimingEventType::Join };
		++_diagnostics.PhaseEventsQueued;
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
		derived->Bpm, derived->Bpi, timing.IntervalPositionSamps };
}

NinjamTimingUpdate NinjamTimingCoordinator::_AcceptTempoChange(const NinjamTempoChange& change,
	utils::Timer& clock)
{
	NinjamTimingUpdate update;
	const auto oldLength = clock.SeedSourceLength();
	const auto delta = oldLength == 0ul ? 0 : SignedCircularDifference(clock.SampOffset(),
		change.IntervalPositionSamps, change.IntervalLengthSamps);
	update.ClockSettings = NinjamClockSettings{ change.IntervalLengthSamps, change.GrainSamps,
		utils::Timer::QUANTISE_POWER, change.IntervalPositionSamps };
	if (delta != 0)
		update.PhaseCorrection = NinjamPhaseCorrection{ delta, _tracker.Generation(), false };
	_pendingTempoChange.reset();
	_ignoredTempoChange.reset();
	++_diagnostics.TempoAccepted;
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