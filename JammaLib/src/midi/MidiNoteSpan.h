#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "MidiEvent.h"

namespace engine
{
	struct MidiNote
	{
		std::uint32_t StartSample;
		std::uint32_t DurationSamples;
		std::uint8_t Channel;
		std::uint8_t Note;
		std::uint8_t Velocity;

		static constexpr std::size_t TotalNoteSlots = 16u * 128u;
		static constexpr std::size_t NoteSlot(std::uint8_t channel, std::uint8_t note) noexcept
		{
			return (static_cast<std::size_t>(channel & MidiEvent::ChannelMask) << 7) | (note & 0x7F);
		}
	};

	// Back-compat alias retained while call sites migrate to MidiNote naming.
	using MidiNoteSpan = MidiNote;

	struct MidiNoteSnapshot
	{
		std::bitset<MidiNote::TotalNoteSlots> Held{};
		std::array<std::uint8_t, MidiNote::TotalNoteSlots> Velocity{};

		void Set(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity) noexcept
		{
			const auto slot = MidiNote::NoteSlot(channel, note);
			Held.set(slot);
			Velocity[slot] = velocity;
		}

		void Clear(std::uint8_t channel, std::uint8_t note) noexcept
		{
			const auto slot = MidiNote::NoteSlot(channel, note);
			Held.reset(slot);
			Velocity[slot] = 0u;
		}
	};

	std::vector<MidiNote> ExtractMidiNoteSpans(const MidiEvent* events,
	                                                std::size_t eventCount,
	                                                std::uint32_t loopLengthSamps);
}
