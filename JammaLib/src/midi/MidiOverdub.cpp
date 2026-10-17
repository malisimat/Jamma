#include "MidiOverdub.h"

#include <algorithm>
#include <array>

#include "MidiNote.h"

using namespace midi;

static constexpr std::size_t MaxPunchWindows = 128u;
static constexpr std::size_t MaxSourceSpans = 4096u;

struct MidiOverdubNormalizedWindow
{
	std::uint32_t Start = 0u;
	std::uint32_t End = 0u;
};

struct MidiOverdubActiveNote
{
	bool IsActive = false;
	std::uint32_t Start = 0u;
	std::uint8_t Velocity = 0u;
	std::uint8_t Flags = 0u;
};

struct MidiOverdubSourceSpan
{
	std::uint32_t Start = 0u;
	std::uint64_t End = 0u;
	std::uint8_t Channel = 0u;
	std::uint8_t Note = 0u;
	std::uint8_t Velocity = 0u;
	std::uint8_t Flags = 0u;
};

static bool IsInsideWindow(std::uint32_t sample,
	const MidiOverdubNormalizedWindow* windows,
	std::size_t windowCount) noexcept
{
	for (std::size_t i = 0u; i < windowCount; ++i)
	{
		const auto& window = windows[i];
		if (sample >= window.Start && sample < window.End)
			return true;
	}

	return false;
}

static std::size_t NormalizePunchWindows(const MidiPunchWindow* windows,
	std::size_t windowCount,
	std::uint32_t targetLength,
	MidiOverdubNormalizedWindow* outWindows,
	std::size_t outCapacity) noexcept
{
	if (!windows || !outWindows || outCapacity == 0u || targetLength == 0u)
		return 0u;

	std::size_t count = 0u;
	for (std::size_t i = 0u; i < windowCount; ++i)
	{
		auto start = windows[i].StartSample;
		auto end = windows[i].EndSample;
		if (end < start)
			std::swap(start, end);

		if (start >= targetLength)
			continue;
		if (end > targetLength)
			end = targetLength;
		if (end <= start)
			continue;

		if (count >= outCapacity)
			break;

		outWindows[count].Start = start;
		outWindows[count].End = end;
		++count;
	}

	if (count <= 1u)
		return count;

	for (std::size_t i = 1u; i < count; ++i)
	{
		const auto current = outWindows[i];
		std::size_t j = i;
		while (j > 0u && current.Start < outWindows[j - 1u].Start)
		{
			outWindows[j] = outWindows[j - 1u];
			--j;
		}
		outWindows[j] = current;
	}

	std::size_t merged = 0u;
	for (std::size_t i = 0u; i < count; ++i)
	{
		const auto& window = outWindows[i];
		if (merged == 0u)
		{
			outWindows[merged++] = window;
			continue;
		}

		auto& prev = outWindows[merged - 1u];
		if (window.Start <= prev.End)
		{
			if (window.End > prev.End)
				prev.End = window.End;
			continue;
		}

		outWindows[merged++] = window;
	}

	return merged;
}

static std::size_t BuildSourceSpans(const MidiEvent* sourceEvents,
	std::size_t sourceEventCount,
	std::uint32_t sourceLoopLength,
	MidiOverdubSourceSpan* outSpans,
	std::size_t outCapacity) noexcept
{
	if (!sourceEvents || !outSpans || outCapacity == 0u || sourceLoopLength == 0u)
		return 0u;
	if (std::any_of(sourceEvents, sourceEvents + sourceEventCount,
		[](const auto& event) { return (event.flags & MidiEvent::TriggerSeamEndpoint) != 0u; }))
	{
		const auto notes = MidiNote::ExtractSpans(sourceEvents, sourceEventCount, sourceLoopLength);
		const auto count = (std::min)(notes.size(), outCapacity);
		for (std::size_t i = 0u; i < count; ++i)
		{
			const auto& note = notes[i];
			outSpans[i] = {note.StartSample, static_cast<std::uint64_t>(note.StartSample) + note.DurationSamples,
				note.Channel, note.Note, note.Velocity, note.Flags};
		}
		return count;
	}

	std::array<MidiOverdubActiveNote, MidiEvent::PairingSlotCount> activeNotes{};
	std::size_t spanCount = 0u;

	const auto emitSpan = [&](std::uint32_t start,
		std::uint32_t end,
		std::uint8_t channel,
		std::uint8_t note,
		std::uint8_t velocity, std::uint8_t flags) noexcept
	{
		if (end <= start || spanCount >= outCapacity)
			return;

		outSpans[spanCount].Start = start;
		outSpans[spanCount].End = end;
		outSpans[spanCount].Channel = static_cast<std::uint8_t>(channel & MidiEvent::ChannelMask);
		outSpans[spanCount].Note = static_cast<std::uint8_t>(note & 0x7F);
		outSpans[spanCount].Velocity = velocity;
		outSpans[spanCount].Flags = flags;
		++spanCount;
	};

	for (std::size_t i = 0u; i < sourceEventCount; ++i)
	{
		const auto& event = sourceEvents[i];
		if (event.sampleOffset >= sourceLoopLength)
			continue;

		const auto channel = event.Channel();
		const auto note = static_cast<std::uint8_t>(event.data1 & 0x7F);
		auto& active = activeNotes[event.PairingSlot()];

		if (event.IsNoteOn())
		{
			if (active.IsActive)
				emitSpan(active.Start, event.sampleOffset, channel, note, active.Velocity, active.Flags);

			active.IsActive = true;
			active.Start = event.sampleOffset;
			active.Velocity = event.data2;
			active.Flags = event.flags;
		}
		else if (event.IsNoteOff())
		{
			if (!active.IsActive)
				continue;

			emitSpan(active.Start, event.sampleOffset, channel, note, active.Velocity, active.Flags);
			active.IsActive = false;
			active.Start = 0u;
			active.Velocity = 0u;
		}
	}

	for (std::size_t slot = 0u; slot < activeNotes.size(); ++slot)
	{
		const auto& active = activeNotes[slot];
		if (!active.IsActive)
			continue;

		const auto channel = static_cast<std::uint8_t>((slot >> 7) & MidiEvent::ChannelMask);
		const auto note = static_cast<std::uint8_t>(slot & 0x7Fu);
		emitSpan(active.Start, sourceLoopLength, channel, note, active.Velocity, active.Flags);
	}

	return spanCount;
}

static bool AppendEvent(const MidiEvent& event,
	MidiEvent* outEvents,
	std::size_t outCapacity,
	std::size_t& outCount) noexcept
{
	if (outCount >= outCapacity)
		return false;

	outEvents[outCount++] = event;
	return true;
}

std::size_t midi::BuildMidiOverdubBaseEvents(const MidiOverdubRenderParams& params,
	MidiEvent* outEvents,
	std::size_t outCapacity) noexcept
{
	if (!outEvents || outCapacity == 0u)
		return 0u;
	if (!params.SourceEvents || params.SourceEventCount == 0u)
		return 0u;
	if (params.SourceLoopLengthSamps == 0u || params.TargetLoopLengthSamps == 0u)
		return 0u;

	std::array<MidiOverdubNormalizedWindow, MaxPunchWindows> windows{};
	const auto windowCount = NormalizePunchWindows(params.PunchWindows,
		params.PunchWindowCount,
		params.TargetLoopLengthSamps,
		windows.data(),
		windows.size());

	std::array<MidiOverdubSourceSpan, MaxSourceSpans> spans{};
	const auto spanCount = BuildSourceSpans(params.SourceEvents,
		params.SourceEventCount,
		params.SourceLoopLengthSamps,
		spans.data(),
		spans.size());

	std::size_t outCount = 0u;

	for (std::uint64_t repeatStart = 0u;
		repeatStart < params.TargetLoopLengthSamps;
		repeatStart += params.SourceLoopLengthSamps)
	{
		for (std::size_t i = 0u; i < params.SourceEventCount; ++i)
		{
			const auto& sourceEvent = params.SourceEvents[i];
			if (sourceEvent.sampleOffset >= params.SourceLoopLengthSamps)
				continue;
			if (sourceEvent.IsNoteOn() || sourceEvent.IsNoteOff())
				continue;

			auto targetOffset = repeatStart + sourceEvent.sampleOffset;
			if (targetOffset >= params.TargetLoopLengthSamps)
				continue;
			if (IsInsideWindow(static_cast<std::uint32_t>(targetOffset), windows.data(), windowCount))
				continue;

			MidiEvent copied = sourceEvent;
			copied.sampleOffset = static_cast<std::uint32_t>(targetOffset);
			if (!AppendEvent(copied, outEvents, outCapacity, outCount))
				goto finalize;
		}

		for (std::size_t i = 0u; i < spanCount; ++i)
		{
			const auto& span = spans[i];
			if (span.Flags & MidiEvent::TriggerSeamEndpoint)
			{
				// Keep one complete physical note across the target seam. Project
				// punch windows onto the unwrapped span before baking endpoints.
				const bool wrap = params.TargetLoopLengthSamps % params.SourceLoopLengthSamps == 0u;
				const auto emit = [&](std::uint64_t first, std::uint64_t end)
				{
					if (first >= end) return true;
					const auto onset = static_cast<std::uint32_t>(first % params.TargetLoopLengthSamps);
					const auto release = static_cast<std::uint32_t>(wrap ? end % params.TargetLoopLengthSamps : end);
					auto flags = static_cast<std::uint8_t>(span.Flags & ~MidiEvent::TriggerSeamEndpoint);
					if (wrap && onset + end - first >= params.TargetLoopLengthSamps) flags |= MidiEvent::TriggerSeamEndpoint;
					return AppendEvent(MidiEvent::MakeNoteOn(onset, span.Channel, span.Note, span.Velocity).WithFlags(flags), outEvents, outCapacity, outCount)
						&& AppendEvent(MidiEvent::MakeNoteOff(release, span.Channel, span.Note).WithFlags(flags), outEvents, outCapacity, outCount);
				};
				const auto render = [&](std::uint64_t cursor, std::uint64_t stop)
				{
					for (unsigned int pass = 0u; pass < 2u && cursor < stop; ++pass)
					for (std::size_t w = 0u; w < windowCount && cursor < stop; ++w)
					{
						const auto first = static_cast<std::uint64_t>(windows[w].Start) + static_cast<std::uint64_t>(pass) * params.TargetLoopLengthSamps;
						const auto end = static_cast<std::uint64_t>(windows[w].End) + static_cast<std::uint64_t>(pass) * params.TargetLoopLengthSamps;
						if (end <= cursor || first >= stop) continue;
						if (first > cursor && !emit(cursor, (std::min)(first, stop))) return false;
						cursor = (std::min)((std::max)(cursor, end), stop);
					}
					return emit(cursor, stop);
				};
				if (!wrap && repeatStart == 0u && !render(0u,
					(std::min)(span.End - params.SourceLoopLengthSamps, static_cast<std::uint64_t>(params.TargetLoopLengthSamps)))) goto finalize;
				const auto stop = wrap ? repeatStart + span.End
					: (std::min)(repeatStart + span.End, static_cast<std::uint64_t>(params.TargetLoopLengthSamps));
				if (!render(repeatStart + span.Start, stop)) goto finalize;
				continue;
			}
			auto spanStart = repeatStart + span.Start;
			auto spanEnd = repeatStart + span.End;
			if (spanStart >= params.TargetLoopLengthSamps)
				continue;
			if (spanEnd > params.TargetLoopLengthSamps)
				spanEnd = params.TargetLoopLengthSamps;
			if (spanEnd <= spanStart)
				continue;

			auto cursor = static_cast<std::uint32_t>(spanStart);
			const auto stop = static_cast<std::uint32_t>(spanEnd);

			for (std::size_t w = 0u; w < windowCount; ++w)
			{
				const auto& window = windows[w];
				if (window.End <= cursor)
					continue;
				if (window.Start >= stop)
					break;

				if (window.Start > cursor)
				{
					const auto segStart = cursor;
					const auto segEnd = (window.Start < stop) ? window.Start : stop;
					if (segEnd > segStart)
					{
						if (!AppendEvent(MidiEvent::MakeNoteOn(segStart, span.Channel, span.Note, span.Velocity).WithFlags(span.Flags), outEvents, outCapacity, outCount))
							goto finalize;
						if (!AppendEvent(MidiEvent::MakeNoteOff(segEnd, span.Channel, span.Note).WithFlags(span.Flags), outEvents, outCapacity, outCount))
							goto finalize;
					}
				}

				if (window.End >= stop)
				{
					cursor = stop;
					break;
				}

				cursor = window.End;
			}

			if (cursor < stop)
			{
				if (!AppendEvent(MidiEvent::MakeNoteOn(cursor, span.Channel, span.Note, span.Velocity).WithFlags(span.Flags), outEvents, outCapacity, outCount))
					goto finalize;
				if (!AppendEvent(MidiEvent::MakeNoteOff(stop, span.Channel, span.Note).WithFlags(span.Flags), outEvents, outCapacity, outCount))
					goto finalize;
			}
		}
	}

finalize:
	MidiNote::SortMidiEvents(outEvents, outCount);
	return outCount;
}
