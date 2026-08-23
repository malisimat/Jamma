#pragma once

#include <cmath>
#include <cstdint>

namespace ninjam
{
	// One NINJAM interval is exposed to plugins as one bar of BPI/4.
	struct NinjamMusicalPosition
	{
		bool IsValid = false;
		bool PositionChanged = false;
		double Ppq = 0.0;
		std::int32_t BeatsPerInterval = 4;
	};

	class NinjamMusicalTransport
	{
	public:
		void Reset() noexcept { *this = {}; }

		static NinjamMusicalPosition LocalPosition(unsigned long loopCount, unsigned int phaseSamps,
			unsigned long masterLengthSamps, unsigned int grainSamps) noexcept
		{
			if (masterLengthSamps == 0ul || grainSamps == 0u || masterLengthSamps % grainSamps != 0ul)
				return {};
			const auto beatsPerInterval = static_cast<std::int32_t>(masterLengthSamps / grainSamps);
			return { true, false, static_cast<double>(loopCount) * beatsPerInterval
				+ static_cast<double>(phaseSamps % masterLengthSamps) / grainSamps, beatsPerInterval };
		}

		// Preserve the live PPQ epoch while a remote grid is pending. At its next
		// wrap, the only permitted locate is forward to a whole remote interval.
		void QueueRemote(std::uint64_t sceneCoordinateSamps, std::uint64_t remotePhaseSamps,
			std::uint64_t intervalLengthSamps, unsigned int beatsPerInterval,
			const NinjamMusicalPosition& current, unsigned int currentGrainSamps) noexcept
		{
			_intervalLengthSamps = intervalLengthSamps;
			_beatsPerInterval = static_cast<std::int32_t>(beatsPerInterval);
			_isRemoteActive = false;
			_positionChanged = false;
			if (!current.IsValid || intervalLengthSamps == 0u || beatsPerInterval == 0u || currentGrainSamps == 0u)
			{
				_isPending = false;
				return;
			}

			_isPending = true;
			_pendingSceneOriginSamps = sceneCoordinateSamps;
			_pendingPpqAtOrigin = current.Ppq;
			_pendingSamplesPerBeat = currentGrainSamps;
			const auto phase = remotePhaseSamps % intervalLengthSamps;
			_nextWrapSceneSamps = sceneCoordinateSamps + (intervalLengthSamps - phase) % intervalLengthSamps;
		}

		void Advance(std::uint64_t sceneCoordinateSamps) noexcept
		{
			_positionChanged = false;
			if (!_isPending || sceneCoordinateSamps < _nextWrapSceneSamps)
				return;

			const auto pendingAtWrap = _pendingPpqAtOrigin + static_cast<double>(
				_nextWrapSceneSamps - _pendingSceneOriginSamps) / _pendingSamplesPerBeat;
			_sceneOriginSamps = _nextWrapSceneSamps;
			_ppqAtOrigin = std::ceil(pendingAtWrap / _beatsPerInterval) * _beatsPerInterval;
			_isPending = false;
			_isRemoteActive = true;
			_positionChanged = _ppqAtOrigin > pendingAtWrap;
		}

		NinjamMusicalPosition PositionAt(std::uint64_t sceneCoordinateSamps) const noexcept
		{
			return PositionAt(sceneCoordinateSamps, {});
		}

		NinjamMusicalPosition PositionAt(std::uint64_t sceneCoordinateSamps,
			const NinjamMusicalPosition& local) const noexcept
		{
			if (_isPending && sceneCoordinateSamps >= _pendingSceneOriginSamps)
				return { true, _positionChanged, _pendingPpqAtOrigin + static_cast<double>(
					sceneCoordinateSamps - _pendingSceneOriginSamps) / _pendingSamplesPerBeat,
					local.BeatsPerInterval };
			if (_isRemoteActive && sceneCoordinateSamps >= _sceneOriginSamps)
				return { true, _positionChanged, _ppqAtOrigin + static_cast<double>(
					sceneCoordinateSamps - _sceneOriginSamps) * _beatsPerInterval / _intervalLengthSamps,
					_beatsPerInterval };
			return local;
		}

		bool HasRemoteGeometry() const noexcept { return _isPending || _isRemoteActive; }

	private:
		std::uint64_t _intervalLengthSamps = 0u;
		std::uint64_t _nextWrapSceneSamps = 0u;
		std::uint64_t _pendingSceneOriginSamps = 0u;
		std::uint64_t _sceneOriginSamps = 0u;
		double _pendingPpqAtOrigin = 0.0;
		double _pendingSamplesPerBeat = 0.0;
		double _ppqAtOrigin = 0.0;
		std::int32_t _beatsPerInterval = 0;
		bool _isPending = false;
		bool _isRemoteActive = false;
		bool _positionChanged = false;
	};
}
