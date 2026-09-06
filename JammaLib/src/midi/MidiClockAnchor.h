#pragma once

#include <atomic>
#include <cstdint>

namespace midi
{
	struct MidiClockAnchorSnapshot
	{
		std::uint64_t Sample = 0u;
		std::int64_t SteadyMicros = 0;
	};

	struct MidiClockAnchor
	{
		std::atomic<std::uint32_t> Sequence{ 0u };
		std::atomic<std::uint64_t> Sample{ 0u };
		std::atomic<std::int64_t> SteadyMicros{ 0 };
	};

	inline void PublishMidiClockAnchor(MidiClockAnchor& anchor,
		std::uint64_t sample,
		std::int64_t steadyMicros) noexcept
	{
		anchor.Sequence.fetch_add(1u, std::memory_order_relaxed);
		anchor.Sample.store(sample, std::memory_order_relaxed);
		anchor.SteadyMicros.store(steadyMicros, std::memory_order_relaxed);
		anchor.Sequence.fetch_add(1u, std::memory_order_release);
	}

	inline MidiClockAnchorSnapshot ReadMidiClockAnchor(const MidiClockAnchor& anchor,
		MidiClockAnchorSnapshot fallback) noexcept
	{
		for (unsigned int attempt = 0u; attempt < 2u; ++attempt)
		{
			const auto before = anchor.Sequence.load(std::memory_order_acquire);
			if ((before & 1u) != 0u)
				continue;

			const MidiClockAnchorSnapshot snapshot{
				anchor.Sample.load(std::memory_order_relaxed),
				anchor.SteadyMicros.load(std::memory_order_relaxed)
			};
			const auto after = anchor.Sequence.load(std::memory_order_acquire);
			if (before == after)
				return snapshot;
		}

		return fallback;
	}
}