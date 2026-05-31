#include "MidiNoteSpan.h"

#include <array>

using namespace engine;

namespace
{
	static constexpr std::size_t TotalNoteSlots = 16u * 128u;

	constexpr std::size_t NoteSlot(std::uint8_t channel, std::uint8_t note) noexcept
	{
		return (static_cast<std::size_t>(channel & MidiEvent::ChannelMask) << 7) | (note & 0x7F);
	}

	struct ActiveNote
	{
		bool IsActive = false;
		std::uint32_t StartSample = 0;
		std::uint8_t Velocity = 0;
	};

	void AddSpan(std::vector<MidiNoteSpan>& spans,
	             std::uint32_t startSample,
	             std::uint32_t endSample,
	             std::uint8_t channel,
	             std::uint8_t note,
	             std::uint8_t velocity)
	{
		if (endSample <= startSample)
			return;

		spans.push_back(MidiNoteSpan{
			startSample,
			endSample - startSample,
			static_cast<std::uint8_t>(channel & MidiEvent::ChannelMask),
			static_cast<std::uint8_t>(note & 0x7F),
			velocity
		});
	}
}

std::vector<MidiNoteSpan> engine::ExtractMidiNoteSpans(const MidiEvent* events,
                                                       std::size_t eventCount,
                                                       std::uint32_t loopLengthSamps)
{
	std::vector<MidiNoteSpan> spans;
	if (nullptr == events || 0u == eventCount || 0u == loopLengthSamps)
		return spans;

	spans.reserve(eventCount / 2u);

	std::array<ActiveNote, TotalNoteSlots> activeNotes{};

	for (std::size_t i = 0; i < eventCount; ++i)
	{
		const auto& ev = events[i];
		if (ev.sampleOffset >= loopLengthSamps)
			continue;

		const auto channel = ev.Channel();
		const auto note = static_cast<std::uint8_t>(ev.data1 & 0x7F);
		const auto slot = NoteSlot(channel, note);
		auto& active = activeNotes[slot];

		if (ev.IsNoteOn())
		{
			if (active.IsActive)
			{
				AddSpan(spans, active.StartSample, ev.sampleOffset, channel, note, active.Velocity);
			}

			active.IsActive = true;
			active.StartSample = ev.sampleOffset;
			active.Velocity = ev.data2;
		}
		else if (ev.IsNoteOff())
		{
			if (!active.IsActive)
				continue;

			AddSpan(spans, active.StartSample, ev.sampleOffset, channel, note, active.Velocity);
			active.IsActive = false;
		}
	}

	for (std::size_t slot = 0; slot < activeNotes.size(); ++slot)
	{
		const auto& active = activeNotes[slot];
		if (!active.IsActive)
			continue;

		const auto channel = static_cast<std::uint8_t>((slot >> 7) & MidiEvent::ChannelMask);
		const auto note = static_cast<std::uint8_t>(slot & 0x7F);
		AddSpan(spans, active.StartSample, loopLengthSamps, channel, note, active.Velocity);
	}

	return spans;
}
