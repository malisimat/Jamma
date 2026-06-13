#pragma once

#include <atomic>
#include <chrono>
#include <tuple>

typedef std::chrono::time_point<std::chrono::steady_clock> Time;

namespace utils
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

		unsigned int SampOffset() const noexcept { return _sampOffset.load(std::memory_order_relaxed); }
		unsigned long LoopCount() const noexcept { return _loopCount.load(std::memory_order_relaxed); }

		// Returns (LoopCount * SeedSourceLength) + SampOffset, the absolute timeline
		// position in samples since the clock was seeded.  Returns `fallback` when
		// the clock has not yet been seeded (SeedSourceLength == 0).
		unsigned long AbsoluteSamplePos(unsigned long fallback = 0ul) const noexcept
		{
			const auto loopLength = _seedSourceLengthSamps.load(std::memory_order_acquire);
			if (loopLength == 0ul)
				return fallback;
			return _loopCount.load(std::memory_order_relaxed) * loopLength
				+ static_cast<unsigned long>(_sampOffset.load(std::memory_order_relaxed));
		}

	private:
		std::atomic_ulong _loopCount;
		std::atomic_uint _sampOffset;
		std::atomic_uint _quantiseSamps;
		std::atomic_ulong _seedSourceLengthSamps;
		std::atomic<QuantisationType> _quantisation;
	};
}
