#pragma once

#include <cstdint>

namespace utils
{
	// A plugin-facing musical location. One external interval is conventionally
	// exposed as a bar; the caller supplies its beats-per-interval value.
	struct MusicalPosition
	{
		bool IsValid = false;
		bool PositionChanged = false;
		double Ppq = 0.0;
		double Tempo = 0.0;
		std::int32_t BeatsPerInterval = 4;
	};

	// Audio-thread-owned external-grid alignment. It deliberately has no
	// knowledge of NINJAM, timers, plugins, or audio-loop phase.
	class MusicalTransport
	{
	public:
		void Reset() noexcept;
		void QueueExternal(std::uint64_t sceneCoordinateSamps,
			std::uint64_t externalPhaseSamps, std::uint64_t intervalLengthSamps,
			unsigned int beatsPerInterval, double tempo, const MusicalPosition& current,
			unsigned int currentSamplesPerBeat) noexcept;
		void Advance(std::uint64_t sceneCoordinateSamps) noexcept;
		MusicalPosition PositionAt(std::uint64_t sceneCoordinateSamps,
			const MusicalPosition& local) const noexcept;

	private:
		std::uint64_t _nextWrapSceneSamps = 0u;
		std::uint64_t _pendingSceneOriginSamps = 0u;
		std::uint64_t _sceneOriginSamps = 0u;
		double _pendingPpqAtOrigin = 0.0;
		double _pendingSamplesPerBeat = 0.0;
		double _ppqAtOrigin = 0.0;
		double _tempo = 0.0;
		std::uint64_t _intervalLengthSamps = 0u;
		std::int32_t _beatsPerInterval = 0;
		bool _isPending = false;
		bool _isExternalActive = false;
		bool _positionChanged = false;
	};
}
