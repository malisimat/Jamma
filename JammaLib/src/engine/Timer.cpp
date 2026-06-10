#include "Timer.h"

using namespace engine;

Timer::Timer() :
	_loopCount(0ul),
	_sampOffset(0u),
	_quantiseSamps(0u),
	_seedSourceLengthSamps(0ul),
	_quantisation(QUANTISE_OFF)
{
}

Timer::~Timer()
{
}

Time Timer::GetTime()
{
	return std::chrono::steady_clock::now();
}

Time Timer::GetZero()
{
	Time time;
	return time.min();
}

bool Timer::IsZero(Time t)
{
	Time time;
	return time.min() == t;
}

double Timer::GetElapsedSeconds(Time t1, Time t2)
{
	return std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1).count();
}

void Timer::Tick(unsigned int sampsIncrement, unsigned int loopCountIncrement)
{
	(void)loopCountIncrement;

	const auto loopLength = _seedSourceLengthSamps.load(std::memory_order_acquire);
	if (0ul == loopLength)
		return;

	const auto sampleOffset = static_cast<unsigned long>(_sampOffset.load(std::memory_order_relaxed));
	const auto totalSamps = sampleOffset + static_cast<unsigned long>(sampsIncrement);
	const auto wraps = totalSamps / loopLength;
	const auto next = totalSamps % loopLength;

	if (wraps > 0ul)
		_loopCount.fetch_add(wraps, std::memory_order_relaxed);

	_sampOffset.store(static_cast<unsigned int>(next), std::memory_order_relaxed);
}

void Timer::Clear()
{
	_quantiseSamps.store(0u, std::memory_order_release);
	_seedSourceLengthSamps.store(0ul, std::memory_order_release);
	_sampOffset.store(0u, std::memory_order_release);
	_loopCount.store(0ul, std::memory_order_release);
}

bool Timer::IsQuantisable() const
{
	return 0u != _quantiseSamps.load(std::memory_order_acquire);
}

void Timer::SetQuantisation(unsigned int quantiseSamps,
	QuantisationType quantisation)
{
	_quantiseSamps.store(quantiseSamps, std::memory_order_release);
	_seedSourceLengthSamps.store(0ul, std::memory_order_release);
	_sampOffset.store(0u, std::memory_order_release);
	_loopCount.store(0ul, std::memory_order_release);
	_quantisation.store(quantisation, std::memory_order_release);
}

void Timer::SetSeedSourceLength(unsigned long loopLengthSamps)
{
	_seedSourceLengthSamps.store(loopLengthSamps, std::memory_order_release);
	if (0ul == loopLengthSamps)
	{
		_sampOffset.store(0u, std::memory_order_release);
		return;
	}

	auto sampleOffset = static_cast<unsigned long>(_sampOffset.load(std::memory_order_acquire));
	if (sampleOffset >= loopLengthSamps)
		sampleOffset %= loopLengthSamps;
	_sampOffset.store(static_cast<unsigned int>(sampleOffset), std::memory_order_release);
}

void Timer::SetMasterLoopIndexFrac(double loopIndexFrac) noexcept
{
	auto clampedFrac = loopIndexFrac;
	if (clampedFrac < 0.0)
		clampedFrac = 0.0;
	else if (clampedFrac > 1.0)
		clampedFrac = 1.0;

	const auto loopLength = _seedSourceLengthSamps.load(std::memory_order_acquire);
	if (0ul == loopLength)
	{
		_sampOffset.store(0u, std::memory_order_release);
		return;
	}

	unsigned long sampleOffset = 0ul;
	if (clampedFrac <= 0.0)
		sampleOffset = loopLength - 1ul;
	else if (clampedFrac < 1.0)
		sampleOffset = static_cast<unsigned long>((1.0 - clampedFrac) * static_cast<double>(loopLength));

	if (sampleOffset >= loopLength)
		sampleOffset = loopLength - 1ul;

	_sampOffset.store(static_cast<unsigned int>(sampleOffset), std::memory_order_release);
}

unsigned int Timer::QuantiseSamps() const
{
	return _quantiseSamps.load(std::memory_order_acquire);
}

Timer::QuantisationType Timer::Quantisation() const
{
	return _quantisation.load(std::memory_order_acquire);
}

unsigned long Timer::SeedSourceLength() const
{
	return _seedSourceLengthSamps.load(std::memory_order_acquire);
}

double Timer::MasterLoopIndexFrac() const noexcept
{
	const auto loopLength = _seedSourceLengthSamps.load(std::memory_order_acquire);
	if (0ul == loopLength)
		return 0.0;

	const auto sampleOffset = static_cast<unsigned long>(_sampOffset.load(std::memory_order_acquire)) % loopLength;
	const auto frac = 1.0 - (static_cast<double>(sampleOffset) / static_cast<double>(loopLength));
	if (frac < 0.0)
		return 0.0;
	if (frac > 1.0)
		return 1.0;
	return frac;
}

std::tuple<unsigned long, int> engine::Timer::QuantiseLength(unsigned long length)
{
	const auto quantiseSamps = _quantiseSamps.load(std::memory_order_acquire);
	const auto quantisation = _quantisation.load(std::memory_order_acquire);
	if (0u == quantiseSamps)
		return std::make_tuple(length, 0);

	switch (quantisation)
	{
	case QUANTISE_MULTIPLE:
	{
		auto nLow = length / quantiseSamps;
		auto nHigh = nLow + 1;
		auto lenLow = nLow * quantiseSamps;
		auto lenHigh = nHigh * quantiseSamps;
		auto dLow = length - lenLow;
		auto dHigh = lenHigh - length;
		if (dLow < dHigh)
			return std::make_tuple(lenLow, (int)dLow);
		else
			return std::make_tuple(lenHigh, -1 * (int)dHigh);
	}
	case QUANTISE_POWER:
	{
		unsigned long lenCur = 0;
		unsigned long lenLast = 0;
		auto currentMultiplier = 1u;

		while (currentMultiplier <= 128)
		{
			lenCur = currentMultiplier * quantiseSamps;

			if (lenCur >= length)
			{
				auto dLast = length - lenLast;
				auto dCur = lenCur - length;

				if (dLast < dCur)
					return std::make_tuple(lenLast, (int)dLast);
				else
					return std::make_tuple(lenCur, -1 * ((int)dCur));
			}

			lenLast = lenCur;
			currentMultiplier *= 2;
		}
	}
	}

	return std::make_tuple(length, 0);
}
