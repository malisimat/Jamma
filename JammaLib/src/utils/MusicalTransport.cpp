#include "MusicalTransport.h"

#include <cmath>

using namespace utils;

void MusicalTransport::Reset() noexcept
{
	*this = {};
}

void MusicalTransport::QueueExternal(std::uint64_t sceneCoordinateSamps,
	std::uint64_t externalPhaseSamps, std::uint64_t intervalLengthSamps,
	unsigned int beatsPerInterval, double tempo, const MusicalPosition& current,
	unsigned int currentSamplesPerBeat) noexcept
{
	_isExternalActive = false;
	_positionChanged = false;
	if (!current.IsValid || intervalLengthSamps == 0u || beatsPerInterval == 0u
		|| tempo <= 0.0 || currentSamplesPerBeat == 0u)
	{
		_isPending = false;
		return;
	}

	_isPending = true;
	_intervalLengthSamps = intervalLengthSamps;
	_beatsPerInterval = static_cast<std::int32_t>(beatsPerInterval);
	_tempo = tempo;
	_pendingSceneOriginSamps = sceneCoordinateSamps;
	_pendingPpqAtOrigin = current.Ppq;
	_pendingSamplesPerBeat = currentSamplesPerBeat;
	const auto phase = externalPhaseSamps % intervalLengthSamps;
	_nextWrapSceneSamps = sceneCoordinateSamps + (intervalLengthSamps - phase) % intervalLengthSamps;
}

void MusicalTransport::Advance(std::uint64_t sceneCoordinateSamps) noexcept
{
	_positionChanged = false;
	if (!_isPending || sceneCoordinateSamps < _nextWrapSceneSamps)
		return;

	const auto pendingAtWrap = _pendingPpqAtOrigin + static_cast<double>(
		_nextWrapSceneSamps - _pendingSceneOriginSamps) / _pendingSamplesPerBeat;
	_sceneOriginSamps = _nextWrapSceneSamps;
	_ppqAtOrigin = std::ceil(pendingAtWrap / _beatsPerInterval) * _beatsPerInterval;
	_isPending = false;
	_isExternalActive = true;
	_positionChanged = _ppqAtOrigin > pendingAtWrap;
}

MusicalPosition MusicalTransport::PositionAt(std::uint64_t sceneCoordinateSamps,
	const MusicalPosition& local) const noexcept
{
	if (_isPending && sceneCoordinateSamps >= _pendingSceneOriginSamps)
		return { true, _positionChanged, _pendingPpqAtOrigin + static_cast<double>(
			sceneCoordinateSamps - _pendingSceneOriginSamps) / _pendingSamplesPerBeat,
			local.Tempo, local.BeatsPerInterval };
	if (_isExternalActive && sceneCoordinateSamps >= _sceneOriginSamps)
		return { true, _positionChanged, _ppqAtOrigin + static_cast<double>(
			sceneCoordinateSamps - _sceneOriginSamps) * _beatsPerInterval / _intervalLengthSamps,
			_tempo, _beatsPerInterval };
	return local;
}
