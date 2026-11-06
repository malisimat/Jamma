#pragma once

#include <cstdint>
#include <chrono>
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
		enum class Outcome { None, Cancelled, Activated };
        struct Result { Outcome State=Outcome::None; std::uint64_t Version=0u; };
        static constexpr auto PreparationDeadline=std::chrono::seconds(5);
        Result Advance(Quantiser& quantisation, audio::AudioHost& audio,
            const std::vector<std::shared_ptr<Station>>& callerLockedStations,
            std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
	private:
		std::uint64_t _lastCommittedVersion = 0u;
		std::uint64_t _preparingVersion = 0u, _preparingDelta = 0u;
	};
}
