#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "MidiEvent.h"

namespace engine
{
	struct MidiNoteSpan
	{
		std::uint32_t StartSample;
		std::uint32_t DurationSamples;
		std::uint8_t Channel;
		std::uint8_t Note;
		std::uint8_t Velocity;
	};

	std::vector<MidiNoteSpan> ExtractMidiNoteSpans(const MidiEvent* events,
	                                                std::size_t eventCount,
	                                                std::uint32_t loopLengthSamps);
}