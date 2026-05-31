#pragma once

#include <cstdint>

namespace engine
{
	// POD MIDI event, sample-relative timestamp.
	//
	// `sampleOffset` is interpreted by the consumer:
	//   - in the ingress queue: samples since the device callback's reference point
	//   - in a MidiLoop:        samples since loop start (0 .. loopLength-1)
	//
	// Channel voice messages encode the channel in the low nibble of `status`.
	// Status nibble (high) is the message type (0x8 NoteOff, 0x9 NoteOn, etc.).
	struct MidiEvent
	{
		std::uint32_t sampleOffset;
		std::uint8_t  status;
		std::uint8_t  data1;
		std::uint8_t  data2;
		std::uint8_t  _pad; // explicit padding for predictable layout

		static constexpr std::uint8_t StatusMask  = 0xF0;
		static constexpr std::uint8_t ChannelMask = 0x0F;

		static constexpr std::uint8_t NoteOff = 0x80;
		static constexpr std::uint8_t NoteOn  = 0x90;

		constexpr std::uint8_t MessageType() const noexcept { return status & StatusMask; }
		constexpr std::uint8_t Channel()     const noexcept { return status & ChannelMask; }

		constexpr bool IsNoteOn()  const noexcept { return MessageType() == NoteOn && data2 != 0; }
		constexpr bool IsNoteOff() const noexcept
		{
			// Many sources represent NoteOff as NoteOn with velocity 0.
			return MessageType() == NoteOff || (MessageType() == NoteOn && data2 == 0);
		}

		static constexpr MidiEvent MakeNoteOn(std::uint32_t offset,
		                                      std::uint8_t channel,
		                                      std::uint8_t note,
		                                      std::uint8_t velocity) noexcept
		{
			return MidiEvent{ offset,
			                  static_cast<std::uint8_t>(NoteOn | (channel & ChannelMask)),
			                  note,
			                  velocity,
			                  0 };
		}

		static constexpr MidiEvent MakeNoteOff(std::uint32_t offset,
		                                       std::uint8_t channel,
		                                       std::uint8_t note,
		                                       std::uint8_t velocity = 0) noexcept
		{
			return MidiEvent{ offset,
			                  static_cast<std::uint8_t>(NoteOff | (channel & ChannelMask)),
			                  note,
			                  velocity,
			                  0 };
		}
	};

	static_assert(sizeof(MidiEvent) == 8, "MidiEvent layout must stay POD-tight");
}
