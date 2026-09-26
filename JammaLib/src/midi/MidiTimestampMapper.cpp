#include "MidiTimestampMapper.h"

#include <cmath>
#include <limits>

using namespace midi;

static constexpr std::uint64_t MicrosPerSecond = 1000000ull;

MidiDriverTimestamp MidiDriverTimestampMapper::Map(double deltaSeconds,
	std::int64_t callbackArrivalMicros) noexcept
{
	if (!_initialized)
	{
		_initialized = true;
		_lastEventMicros = callbackArrivalMicros;
		return { _lastEventMicros, MidiTimestampSource::InitialArrival };
	}

	// WinMM reports millisecond deltas. Bound corrupt or discontinuous input;
	// ordinary callback latency stays in the driver-derived timeline.
	constexpr double maxDeltaSeconds = 60.0;
	constexpr std::int64_t maxLeadMicros = 100000;
	constexpr std::int64_t maxLagMicros = 2000000;
	MidiTimestampSource fallback = MidiTimestampSource::InvalidDeltaFallback;
	if (std::isfinite(deltaSeconds) && deltaSeconds >= 0.0 && deltaSeconds <= maxDeltaSeconds)
	{
		const auto deltaMicros = static_cast<std::int64_t>(deltaSeconds * MicrosPerSecond + 0.5);
		if (_lastEventMicros <= (std::numeric_limits<std::int64_t>::max)() - deltaMicros)
		{
			const auto candidate = _lastEventMicros + deltaMicros;
			if (candidate <= callbackArrivalMicros
				? static_cast<std::uint64_t>(callbackArrivalMicros) - static_cast<std::uint64_t>(candidate)
					<= static_cast<std::uint64_t>(maxLagMicros)
				: static_cast<std::uint64_t>(candidate) - static_cast<std::uint64_t>(callbackArrivalMicros)
					<= static_cast<std::uint64_t>(maxLeadMicros))
			{
				_lastEventMicros = candidate;
				return { candidate, MidiTimestampSource::DriverDelta };
			}
		}
		fallback = MidiTimestampSource::DiscontinuityFallback;
	}

	// A bad delta starts a new arrival-based epoch without reversing order.
	if (callbackArrivalMicros > _lastEventMicros)
		_lastEventMicros = callbackArrivalMicros;
	return { _lastEventMicros, fallback };
}

void MidiDriverTimestampMapper::Reset() noexcept
{
	_initialized = false;
	_lastEventMicros = 0;
}

static std::uint64_t SaturatingAdd(std::uint64_t lhs, std::uint64_t rhs) noexcept
{
	const auto max = std::numeric_limits<std::uint64_t>::max();
	if (max - lhs < rhs)
		return max;

	return lhs + rhs;
}

static std::uint64_t SaturatingMul(std::uint64_t lhs, std::uint64_t rhs) noexcept
{
	if (lhs == 0ull || rhs == 0ull)
		return 0ull;

	const auto max = std::numeric_limits<std::uint64_t>::max();
	if (lhs > max / rhs)
		return max;

	return lhs * rhs;
}

std::uint64_t midi::MapMidiTimestampToAudioSample(unsigned int sampleRate,
	std::uint64_t anchorSample,
	std::int64_t anchorMicros,
	std::int64_t eventMicros) noexcept
{
	if (sampleRate == 0u || eventMicros == anchorMicros)
		return anchorSample;

	const auto eventIsAfterAnchor = eventMicros > anchorMicros;
	const auto deltaMicros = eventIsAfterAnchor
		? static_cast<std::uint64_t>(eventMicros) - static_cast<std::uint64_t>(anchorMicros)
		: static_cast<std::uint64_t>(anchorMicros) - static_cast<std::uint64_t>(eventMicros);
	const auto rate = static_cast<std::uint64_t>(sampleRate);
	const auto wholeSeconds = deltaMicros / MicrosPerSecond;
	const auto remainingMicros = deltaMicros % MicrosPerSecond;

	const auto wholeSamples = SaturatingMul(wholeSeconds, rate);
	const auto fractionalSamples = SaturatingMul(remainingMicros, rate) / MicrosPerSecond;
	const auto deltaSamples = SaturatingAdd(wholeSamples, fractionalSamples);

	return eventIsAfterAnchor ? SaturatingAdd(anchorSample, deltaSamples)
		: anchorSample >= deltaSamples ? anchorSample - deltaSamples : 0ull;
}
