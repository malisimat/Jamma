#pragma once

#include <atomic>
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
		void SetSeedSourceLength(unsigned long loopLengthSamps);
		void SetMasterLoopIndexFrac(double loopIndexFrac) noexcept;
		unsigned int QuantiseSamps() const;
		QuantisationType Quantisation() const;
		unsigned long SeedSourceLength() const;
		double MasterLoopIndexFrac() const noexcept;
		std::tuple<unsigned long, int> QuantiseLength(unsigned long length);

	private:
		std::atomic_ulong _loopCount;
		std::atomic_uint _sampOffset;
		std::atomic_uint _quantiseSamps;
		std::atomic_ulong _seedSourceLengthSamps;
		std::atomic<QuantisationType> _quantisation;
	};
}
