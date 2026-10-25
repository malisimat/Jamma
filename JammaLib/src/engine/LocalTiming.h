#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace audio { class AudioHost; }

namespace engine
{
	class Quantiser;
	class Station;

	// Job-owned handshake for a pending local master. The caller holds its scene
	// lock throughout; AudioHost retains all audio-boundary timing authority.
	class LocalTiming
	{
	public:
		void Advance(Quantiser& quantisation, audio::AudioHost& audio,
			const std::vector<std::shared_ptr<Station>>& callerLockedStations);
	private:
		std::uint64_t _lastCommittedVersion = 0u;
		std::uint64_t _preparingVersion = 0u, _preparingDelta = 0u;
	};
}
