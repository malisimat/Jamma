#include "Timer.h"

using namespace engine;

Timer::Timer() :
	_loopCount(0ul),
	_sampOffset(0u),
	_quantiseSamps(0u),
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
	_loopCount += loopCountIncrement;
}

void Timer::Clear()
{
	_quantiseSamps = 0u;
}

bool Timer::IsQuantisable() const
{
	return 0u != _quantiseSamps;
}

void Timer::SetQuantisation(unsigned int quantiseSamps,
	QuantisationType quantisation)
{
	_quantiseSamps = quantiseSamps;
	_quantisation = quantisation;
}

std::tuple<unsigned long, int> engine::Timer::QuantiseLength(unsigned long length)
{
	if (0u == _quantiseSamps)
		return std::make_tuple(length, 0);

	switch (_quantisation)
	{
	case QUANTISE_MULTIPLE:
	{
		auto nLow = length / _quantiseSamps;
		auto nHigh = nLow + 1;
		auto lenLow = nLow * _quantiseSamps;
		auto lenHigh = nHigh * _quantiseSamps;
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
		unsigned long lenLast = lenCur;
		auto currentMultiplier = 1u;

		while (currentMultiplier < 128)
		{
			currentMultiplier *= 2;
			lenCur = currentMultiplier * _quantiseSamps;

			if (lenCur >= length)
			{
				auto dLast = length - lenLast;
				auto dCur = lenCur - length;

				if (dLast < dCur)
					return std::make_tuple(lenLast, -1 * ((int)dLast));
				else
					return std::make_tuple(lenCur, (int)dCur);
			}

			lenLast = lenCur;
		}
	}
	}

	return std::make_tuple(length, 0);
}
