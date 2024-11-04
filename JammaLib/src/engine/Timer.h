#pragma once

#include <chrono>
#include <tuple>

typedef std::chrono::time_point<std::chrono::steady_clock> Time;

namespace engine
{
	class Timer
	{
	public:
		enum QuantisationType
		{
			QUANTISE_OFF,
			QUANTISE_MULTIPLE,
			QUANTISE_POWER
		};

	public:
		Timer();
		~Timer();

	public:
		static Time GetTime();
		static Time GetZero();
		static bool IsZero(Time t);
		static double GetElapsedSeconds(Time t1, Time t2);

		void Tick(unsigned int sampsIncrement, unsigned int loopCountIncrement);
		void Clear();
		bool IsQuantisable() const;
		void SetQuantisation(unsigned int quantiseSamps, QuantisationType quantisation);
		std::tuple<unsigned long, int> QuantiseLength(unsigned long length);

	private:
		unsigned long _loopCount;
		unsigned int _sampOffset;
		unsigned int _quantiseSamps;
		QuantisationType _quantisation;
	};
}

