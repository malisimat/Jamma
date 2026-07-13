#pragma once

#include <cstdint>

namespace midi
{
	enum class MidiBlockSamplePosition
	{
		Late,
		Due,
		Future
	};

	constexpr std::int32_t MidiSampleDelta(std::uint32_t eventSample,
		std::uint32_t blockStartSample) noexcept
	{
		return static_cast<std::int32_t>(eventSample - blockStartSample);
	}

	constexpr MidiBlockSamplePosition ClassifyMidiSampleInBlock(std::uint32_t eventSample,
		std::uint32_t blockStartSample,
		std::uint32_t numSamples) noexcept
	{
		const auto delta = MidiSampleDelta(eventSample, blockStartSample);
		if (delta < 0)
			return MidiBlockSamplePosition::Late;
		if (static_cast<std::uint32_t>(delta) < numSamples)
			return MidiBlockSamplePosition::Due;
		return MidiBlockSamplePosition::Future;
	}

	constexpr std::uint32_t RebaseMidiSampleForVstBlock(std::uint32_t eventSample,
		std::uint32_t rawBlockStartSample,
		std::uint32_t shiftedBlockStartSample) noexcept
	{
		const auto delta = MidiSampleDelta(eventSample, rawBlockStartSample);
		return delta < 0
			? shiftedBlockStartSample
			: shiftedBlockStartSample + static_cast<std::uint32_t>(delta);
	}
}