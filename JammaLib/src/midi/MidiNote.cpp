#include "MidiNote.h"

#include <algorithm>
#include <array>

using namespace midi;

void MidiNote::AddSpan(std::vector<MidiNote>& spans,
	std::uint32_t startSample,
	std::uint32_t endSample,
	std::uint8_t channel,
	std::uint8_t note,
	std::uint8_t velocity,
	std::uint8_t flags)
{
	if (endSample <= startSample)
		return;

	spans.push_back(MidiNote{
		startSample,
		endSample - startSample,
		static_cast<std::uint8_t>(channel & MidiEvent::ChannelMask),
		static_cast<std::uint8_t>(note & 0x7F),
		velocity,
		flags
	});
}

std::vector<MidiNote> MidiNote::ExtractSpans(const MidiEvent* events,
	std::size_t eventCount,
	std::uint32_t loopLengthSamps)
{
	std::vector<MidiNote> spans;
	if (nullptr == events || 0u == eventCount || 0u == loopLengthSamps)
		return spans;

	spans.reserve(eventCount / 2u);

	std::array<ActiveNote, MidiEvent::PairingSlotCount> activeNotes{};
	std::vector<std::vector<const MidiEvent*>> crossingOn(MidiEvent::PairingSlotCount);
	std::vector<std::vector<const MidiEvent*>> crossingOff(MidiEvent::PairingSlotCount);

	for (std::size_t i = 0; i < eventCount; ++i)
	{
		const auto& ev = events[i];
		if (ev.sampleOffset >= loopLengthSamps)
			continue;

		const auto channel = ev.Channel();
		const auto note = static_cast<std::uint8_t>(ev.data1 & 0x7F);
		const auto slot = ev.PairingSlot();
		if (ev.flags & MidiEvent::TriggerSeamEndpoint)
		{
			if (ev.IsNoteOn()) crossingOn[slot].push_back(&ev);
			else if (ev.IsNoteOff()) crossingOff[slot].push_back(&ev);
			continue;
		}
		auto& active = activeNotes[slot];

		if (ev.IsNoteOn())
		{
			if (active.IsActive)
			{
				AddSpan(spans, active.StartSample, ev.sampleOffset, channel, note, active.Velocity, active.Flags);
			}

			active.IsActive = true;
			active.StartSample = ev.sampleOffset;
			active.Velocity = ev.data2;
			active.Flags = ev.flags;
		}
		else if (ev.IsNoteOff())
		{
			if (!active.IsActive)
				continue;

			AddSpan(spans, active.StartSample, ev.sampleOffset, channel, note, active.Velocity, active.Flags);
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
		AddSpan(spans, active.StartSample, loopLengthSamps, channel, note, active.Velocity, active.Flags);
	}

	for (std::size_t slot = 0u; slot < crossingOn.size(); ++slot)
		for (std::size_t i = 0u; i < (std::min)(crossingOn[slot].size(), crossingOff[slot].size()); ++i)
		{
			const auto& on = *crossingOn[slot][i];
			const auto& off = *crossingOff[slot][i];
			const auto duration = static_cast<std::uint64_t>(loopLengthSamps) - on.sampleOffset + off.sampleOffset;
			if (duration == 0u || duration > loopLengthSamps) continue;
			spans.push_back(MidiNote{on.sampleOffset, static_cast<std::uint32_t>(duration),
				on.Channel(), on.data1, on.data2, on.flags});
		}
	return spans;
}

void MidiNote::SortMidiEvents(MidiEvent* events,
	std::size_t eventCount, std::uint8_t* unmatchedStarts, std::uint16_t* continuationRoots) noexcept
{
	if (nullptr == events || eventCount < 2u)
		return;

	const auto priority = [](const MidiEvent& ev) noexcept
	{
		if (ev.IsNoteOff())
			return 0;
		if (ev.IsNoteOn())
			return 2;
		return 1;
	};

	const auto shouldPrecede = [&](const MidiEvent& lhs, const MidiEvent& rhs) noexcept
	{
		if (lhs.sampleOffset != rhs.sampleOffset)
			return lhs.sampleOffset < rhs.sampleOffset;

		return priority(lhs) < priority(rhs);
	};

	for (std::size_t i = 1u; i < eventCount; ++i)
	{
		const auto current = events[i];
		const auto unmatched = unmatchedStarts ? unmatchedStarts[i] : 0u;
		const auto root = continuationRoots ? continuationRoots[i] : 0u;
		std::size_t j = i;
		while (j > 0u && shouldPrecede(current, events[j - 1u]))
		{
			events[j] = events[j - 1u];
			if (unmatchedStarts) unmatchedStarts[j] = unmatchedStarts[j - 1u];
			if (continuationRoots) continuationRoots[j] = continuationRoots[j - 1u];
			--j;
		}
		events[j] = current;
		if (unmatchedStarts) unmatchedStarts[j] = static_cast<std::uint8_t>(unmatched);
		if (continuationRoots) continuationRoots[j] = static_cast<std::uint16_t>(root);
	}
}
