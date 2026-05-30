#include "MidiTimestampMapper.h"

#include <limits>

using namespace engine;

namespace
{
	constexpr std::uint64_t MicrosPerSecond = 1000000ull;

	std::uint64_t SaturatingAdd(std::uint64_t lhs, std::uint64_t rhs) noexcept
	{
		const auto max = std::numeric_limits<std::uint64_t>::max();
		if (max - lhs < rhs)
			return max;

		return lhs + rhs;
	}

	std::uint64_t SaturatingMul(std::uint64_t lhs, std::uint64_t rhs) noexcept
	{
		if (lhs == 0ull || rhs == 0ull)
			return 0ull;

		const auto max = std::numeric_limits<std::uint64_t>::max();
		if (lhs > max / rhs)
			return max;

		return lhs * rhs;
	}

	std::uint64_t AbsInt64AsUint64(std::int64_t value) noexcept
	{
		return static_cast<std::uint64_t>(-(value + 1)) + 1ull;
	}

	std::uint64_t DeltaMicrosAfterAnchor(std::int64_t anchorMicros, std::int64_t eventMicros) noexcept
	{
		if (eventMicros <= anchorMicros)
			return 0ull;

		if (anchorMicros >= 0)
			return static_cast<std::uint64_t>(eventMicros - anchorMicros);

		if (eventMicros < 0)
			return static_cast<std::uint64_t>(eventMicros - anchorMicros);

		return SaturatingAdd(static_cast<std::uint64_t>(eventMicros), AbsInt64AsUint64(anchorMicros));
	}
}

std::uint64_t engine::MapMidiTimestampToAudioSample(unsigned int sampleRate,
	std::uint64_t anchorSample,
	std::int64_t anchorMicros,
	std::int64_t eventMicros) noexcept
{
	const auto deltaMicros = DeltaMicrosAfterAnchor(anchorMicros, eventMicros);
	if (sampleRate == 0u || deltaMicros == 0ull)
		return anchorSample;

	const auto rate = static_cast<std::uint64_t>(sampleRate);
	const auto wholeSeconds = deltaMicros / MicrosPerSecond;
	const auto remainingMicros = deltaMicros % MicrosPerSecond;

	const auto wholeSamples = SaturatingMul(wholeSeconds, rate);
	const auto fractionalSamples = SaturatingMul(remainingMicros, rate) / MicrosPerSecond;
	const auto deltaSamples = SaturatingAdd(wholeSamples, fractionalSamples);

	return SaturatingAdd(anchorSample, deltaSamples);
}