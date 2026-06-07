#pragma once

#include <cstdint>
#include <ostream>

namespace midi
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

		static const char* Direction(const MidiEvent& event) noexcept
		{
			if (event.IsNoteOn())  return " Down";
			if (event.IsNoteOff()) return " Up";
			return "";
		}

		static void LogDetail(std::ostream& out, std::uint8_t deviceSlot, const MidiEvent& event)
		{
			constexpr std::uint8_t CC            = 0xB0;
			constexpr std::uint8_t ProgramChange = 0xC0;

			out << "dev: " << (deviceSlot + 1) << ", chan " << (event.Channel() + 1) << ", ";

			switch (event.MessageType())
			{
			case NoteOn:
				out << (event.data2 != 0 ? "noteon" : "noteoff") << ": " << static_cast<int>(event.data1);
				break;
			case NoteOff:
				out << "noteoff: " << static_cast<int>(event.data1);
				break;
			case CC:
				out << "cc " << static_cast<int>(event.data1) << ": " << static_cast<int>(event.data2);
				break;
			case ProgramChange:
				out << "pc: " << static_cast<int>(event.data1);
				break;
			default:
				out << "0x" << std::hex << std::uppercase << static_cast<int>(event.status) << std::dec;
				break;
			}
		}
	};

	static_assert(sizeof(MidiEvent) == 8, "MidiEvent layout must stay POD-tight");
}
