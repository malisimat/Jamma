#pragma once

#include <cstdint>

namespace midi
{
	std::uint64_t MapMidiTimestampToAudioSample(unsigned int sampleRate,
		std::uint64_t anchorSample,
		std::int64_t anchorMicros,
		std::int64_t eventMicros) noexcept;
}