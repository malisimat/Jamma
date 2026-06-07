#include <array>
#include <cstring>
#include <vector>
#include "MidiQuantisation.h"
#include "MidiNote.h"

using namespace midi;

namespace
{
	static constexpr std::size_t TotalNoteSlots = 16u * 128u;

	constexpr std::size_t NoteSlot(std::uint8_t channel, std::uint8_t note) noexcept
	{
		return (static_cast<std::size_t>(channel & MidiEvent::ChannelMask) << 7) | (note & 0x7F);
	}
}

int MidiQuantisation::DragSteps(int deltaY) noexcept
{
	const auto absDelta = deltaY < 0 ? -deltaY : deltaY;
	const auto steps = (absDelta + (MidiQuantisationDragPixelsPerStep / 2)) /
		MidiQuantisationDragPixelsPerStep;
	return deltaY < 0 ? steps : -steps;
}

MidiQuantisationFraction MidiQuantisation::ResolveDragFraction(MidiQuantisationFraction startFraction,
	int deltaY) noexcept
{
	const auto startIndex = FractionIndex(startFraction);
	return ClampFractionIndex(startIndex + DragSteps(deltaY));
}

MidiQuantisationSettings MidiQuantisation::ApplyGesture(const MidiQuantisationSettings& current,
	MidiQuantisationGesture gesture,
	MidiQuantisationFraction fraction,
	std::uint32_t resolvedGrainSamps) noexcept
{
	MidiQuantisationSettings updated = current;
	updated.Enabled = (MidiQuantisationGesture::Toggle == gesture) ? !current.Enabled : true;
	updated.Fraction = fraction;

	if (updated.Enabled && 0u == updated.GrainSamps && resolvedGrainSamps > 0u)
		updated.GrainSamps = resolvedGrainSamps;

	return updated;
}

std::uint32_t MidiQuantisation::ResolveGestureGrain(const MidiQuantisationGrainCandidates& candidates) noexcept
{
	if (candidates.FirstPlayableMidiLoopSamps > 0u)
		return candidates.FirstPlayableMidiLoopSamps;
	if (candidates.FirstAudioLoopSamps > 0u)
		return candidates.FirstAudioLoopSamps;
	if (candidates.MidiVisualLoopSamps > 0u)
		return candidates.MidiVisualLoopSamps;

	return candidates.RecordedSamps;
}

MidiQuantisationSettings MidiQuantisation::ApplyGuiPayload(const MidiQuantisationSettings& current,
	const int* values,
	std::size_t valueCount) noexcept
{
	MidiQuantisationSettings updated = current;
	if (nullptr == values || 0u == valueCount)
		return updated;

	updated.Enabled = (0 != values[0]);
	if (valueCount >= 2u)
		updated.Fraction = ClampFractionIndex(values[1]);
	if (valueCount >= 3u && values[2] > 0)
		updated.GrainSamps = static_cast<std::uint32_t>(values[2]);

	return updated;
}

std::uint32_t MidiQuantisation::QuantiseSampleOffset(std::uint32_t offset,
	std::uint32_t step,
	std::uint32_t loopLength) noexcept
{
	if (0u == step || 0u == loopLength)
		return offset;

	// Round-to-nearest: snapped = ((offset + step/2) / step) * step
	const auto halfStep = step / 2u;
	const auto snapped = ((offset + halfStep) / step) * step;

	// Wrap into [0, loopLength) so notes near the loop end snap back to 0
	// rather than escaping the loop window.
	return snapped % loopLength;
}

void MidiQuantisation::QuantiseEvents(const MidiEvent* src,
	std::size_t eventCount,
	std::uint32_t loopLength,
	std::uint32_t stepSamps,
	MidiEvent* dst) noexcept
{
	if (nullptr == src || nullptr == dst || 0u == eventCount)
		return;

	if (0u == stepSamps || 0u == loopLength)
	{
		std::memcpy(dst, src, eventCount * sizeof(MidiEvent));
		return;
	}

	// Track pending NoteOn shifts per (channel, note) slot in FIFO order so
	// overlapping same-pitch notes pair each NoteOff with the earliest unmatched
	// NoteOn.
	std::array<std::vector<std::int64_t>, TotalNoteSlots> pendingDeltas;
	std::array<std::size_t, TotalNoteSlots> pendingReadIndex{};

	for (std::size_t i = 0; i < eventCount; ++i)
	{
		MidiEvent ev = src[i];
		const auto slot = NoteSlot(ev.Channel(), ev.data1);

		if (ev.IsNoteOn())
		{
			if (ev.sampleOffset < loopLength)
			{
				const auto quantised = QuantiseSampleOffset(ev.sampleOffset, stepSamps, loopLength);
				pendingDeltas[slot].push_back(static_cast<std::int64_t>(quantised) -
					static_cast<std::int64_t>(ev.sampleOffset));
				ev.sampleOffset = quantised;
			}
		}
		else if (ev.IsNoteOff())
		{
			auto& deltas = pendingDeltas[slot];
			auto& readIndex = pendingReadIndex[slot];

			if (readIndex < deltas.size())
			{
				const auto delta = deltas[readIndex++];
				const auto shifted = static_cast<std::int64_t>(ev.sampleOffset) + delta;
				const auto loopEnd = static_cast<std::int64_t>(loopLength);

				if (shifted < 0)
				{
					ev.sampleOffset = 0u;
				}
				else if (shifted >= loopEnd)
				{
					// Clamp to last sample of the loop so the NoteOff still fires.
					ev.sampleOffset = static_cast<std::uint32_t>(loopEnd - 1);
				}
				else
				{
					ev.sampleOffset = static_cast<std::uint32_t>(shifted);
				}
			}
		}

		dst[i] = ev;
	}
}

void MidiQuantisation::BuildQuantisedPlaybackEvents(const MidiEvent* src,
	std::size_t eventCount,
	std::uint32_t loopLength,
	std::uint32_t stepSamps,
	MidiEvent* dst) noexcept
{
	QuantiseEvents(src, eventCount, loopLength, stepSamps, dst);
	MidiNote::SortMidiEvents(dst, eventCount);
}
