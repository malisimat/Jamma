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

		enum class CommandType : std::uint8_t
		{
			ReplaceTiming,
			PhaseCorrection,
			Invalidate
		};

		struct Command
		{
			CommandType Type = CommandType::Invalidate;
			std::uint64_t Generation = 0u;
			unsigned long SeedLengthSamps = 0ul;
			unsigned int QuantiseSamps = 0u;
			QuantisationType Quantisation = QUANTISE_OFF;
			long long PhaseDeltaSamps = 0;
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
		void PublishCommand(const Command& command) noexcept;
		bool ConsumePendingCommand() noexcept;
		// Applies a command directly on the audio thread (no mailbox handoff).
		// Used by the unified audio-boundary transport fan-out so the Timer and
		// every active local take consume one coherent command in the same block.
		// Returns true when the command was observed (even if superseded by an
		// older generation); generation filtering matches ConsumePendingCommand.
		bool ApplyCommand(const Command& command) noexcept;
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
		std::uint64_t SceneSamplePos() const noexcept
		{
			return _sceneSamplePos.load(std::memory_order_relaxed);
		}

	private:
		std::atomic_ulong _loopCount;
		std::atomic<std::uint64_t> _sceneSamplePos{ 0u };
		std::atomic_uint _sampOffset;
		std::atomic_uint _quantiseSamps;
		std::atomic_ulong _seedSourceLengthSamps;
		std::atomic<QuantisationType> _quantisation;
		std::atomic<std::uint64_t> _commandSequence{ 0u };
		std::atomic<std::uint64_t> _commandConsumedSequence{ 0u };
		std::atomic<std::uint64_t> _commandGeneration{ 0u };
		std::atomic<unsigned long> _commandSeedLengthSamps{ 0ul };
		std::atomic<unsigned int> _commandQuantiseSamps{ 0u };
		std::atomic<QuantisationType> _commandQuantisation{ QUANTISE_OFF };
		std::atomic<long long> _commandPhaseDeltaSamps{ 0 };
		std::atomic<CommandType> _commandType{ CommandType::Invalidate };
		std::uint64_t _audioGeneration = 0u;
	};
}
