#include "NinjamTimingTracker.h"

using namespace ninjam;

void NinjamTimingTracker::Connect() noexcept
{
	_connected = true;
	_hasLastPosition = false;
	_lastPositionSamps = 0u;
	_intervalLengthSamps = 0u;
	_remoteWrapCount = 0ul;
	_joinPending = false;
	_joinDeltaSamps = 0;
	_joinGeneration = 0u;
	_diagnostics = {};
}

void NinjamTimingTracker::Disconnect() noexcept
{
	_connected = false;
	_hasLastPosition = false;
	_lastPositionSamps = 0u;
	_intervalLengthSamps = 0u;
	_remoteWrapCount = 0ul;
	_joinPending = false;
}

std::optional<NinjamTimingEvent> NinjamTimingTracker::Observe(const NinjamTimingObservation& observation)
{
	if (!_connected || observation.IntervalLengthSamps == 0u)
	{
		++_diagnostics.ObservationsRejected;
		return std::nullopt;
	}

	const auto position = observation.IntervalPositionSamps % observation.IntervalLengthSamps;
	if (observation.IntervalLengthSamps != _intervalLengthSamps)
	{
		_intervalLengthSamps = observation.IntervalLengthSamps;
		_lastPositionSamps = position;
		_hasLastPosition = true;
		++_generation;
		_joinPending = false;
		++_diagnostics.ObservationsAccepted;
		++_diagnostics.GenerationChanges;
		return NinjamTimingEvent{ NinjamTimingEventType::GenerationChanged, _generation,
			_remoteWrapCount, _intervalLengthSamps, position, 0 };
	}

	if (_hasLastPosition && position == _lastPositionSamps)
	{
		++_diagnostics.DuplicateObservations;
		return std::nullopt;
	}

	const auto wasBackward = _hasLastPosition && position < _lastPositionSamps;
	if (wasBackward)
	{
		const auto endWindow = _intervalLengthSamps - (_intervalLengthSamps / 4u);
		const auto startWindow = _intervalLengthSamps / 4u;
		if (_lastPositionSamps < endWindow || position > startWindow)
		{
			++_diagnostics.ObservationsRejected;
			return std::nullopt;
		}
	}

	_lastPositionSamps = position;
	_hasLastPosition = true;
	++_diagnostics.ObservationsAccepted;
	if (!wasBackward)
		return std::nullopt;

	++_remoteWrapCount;
	++_diagnostics.WrapEvents;
	if (_joinPending && _joinGeneration == _generation)
	{
		_joinPending = false;
		++_diagnostics.JoinEvents;
		return NinjamTimingEvent{ NinjamTimingEventType::Join, _generation, _remoteWrapCount,
			_intervalLengthSamps, position, _joinDeltaSamps };
	}

	return NinjamTimingEvent{ NinjamTimingEventType::Wrap, _generation, _remoteWrapCount,
		_intervalLengthSamps, position, 0 };
}

void NinjamTimingTracker::BeginJoinAlignment(unsigned long localMasterPositionSamps) noexcept
{
	if (!_connected || _intervalLengthSamps == 0u || !_hasLastPosition)
		return;

	_joinPending = true;
	_joinGeneration = _generation;
	_joinDeltaSamps = SignedCircularDifference(
		static_cast<unsigned int>(localMasterPositionSamps % _intervalLengthSamps),
		_lastPositionSamps,
		_intervalLengthSamps);
}