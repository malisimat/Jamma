#include <array>
#include <cstring>
#include <vector>
#include "MidiQuantisation.h"
#include "MidiNote.h"

using namespace midi;

constexpr std::size_t MidiQuantisation::NoteSlot(std::uint8_t channel, std::uint8_t note) noexcept
{
	return (static_cast<std::size_t>(channel & MidiEvent::ChannelMask) << 7) | (note & 0x7F);
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
	const auto startIndex = FractionDisplayIndex(startFraction);
	return ClampFractionDisplayIndex(startIndex + DragSteps(deltaY));
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
	if (candidates.ResolvedTakeGrainSamps > 0u)
		return candidates.ResolvedTakeGrainSamps;
	if (candidates.PublishedSceneGrainSamps > 0u)
		return candidates.PublishedSceneGrainSamps;
	if (candidates.SingleGrainLoopSamps > 0u)
		return candidates.SingleGrainLoopSamps;

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
	std::uint32_t loopLength,
	std::int32_t phaseOffsetSamps) noexcept
{
	if (0u == step || 0u == loopLength)
		return offset;

	const auto loopLength64 = static_cast<std::int64_t>(loopLength);
	const auto phase64 = static_cast<std::int64_t>(phaseOffsetSamps);
	const auto phaseWrapped = static_cast<std::uint32_t>(((phase64 % loopLength64) + loopLength64) % loopLength64);

	// Shift into the grid's frame so snap points land at k*step.
	const auto shifted = (offset + loopLength - phaseWrapped) % loopLength;

	// Round-to-nearest in the shifted frame.
	const auto halfStep = step / 2u;
	const auto snapped = ((shifted + halfStep) / step) * step;

	// Map back into the loop-local frame; modulo wraps a tie at the cycle edge
	// back to the grid origin.
	return (snapped + phaseWrapped) % loopLength;
}

void MidiQuantisation::QuantiseEvents(const MidiEvent* src,
	std::size_t eventCount,
	std::uint32_t loopLength,
	std::uint32_t stepSamps,
	MidiEvent* dst,
	std::int32_t phaseOffsetSamps) noexcept
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
	std::array<std::vector<std::int64_t>, MidiEvent::PairingSlotCount> pendingDeltas;
	std::array<std::size_t, MidiEvent::PairingSlotCount> pendingReadIndex{};

	for (std::size_t i = 0; i < eventCount; ++i)
	{
		MidiEvent ev = src[i];
		const auto slot = ev.PairingSlot();

		if (ev.IsNoteOn())
		{
			if (ev.HasExactTiming()) pendingDeltas[slot].push_back(0);
			else if (ev.sampleOffset < loopLength)
			{
				const auto quantised = QuantiseSampleOffset(ev.sampleOffset,
					stepSamps,
					loopLength,
					phaseOffsetSamps);
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
	MidiEvent* dst,
	std::int32_t phaseOffsetSamps) noexcept
{
	QuantiseEvents(src, eventCount, loopLength, stepSamps, dst, phaseOffsetSamps);
	MidiNote::SortMidiEvents(dst, eventCount);
}

std::int64_t MidiQuantisation::NearestBoundaryIndex(std::int64_t relativeSamps,
	std::uint64_t intervalSamps, std::uint64_t divisions) noexcept
{
	if (intervalSamps == 0u || divisions == 0u)
		return 0;
	const auto interval = static_cast<std::int64_t>(intervalSamps);
	auto wholeIntervals = relativeSamps / interval;
	auto remainder = relativeSamps % interval;
	if (remainder < 0)
	{
		remainder += interval;
		--wholeIntervals;
	}
	const auto cell = (static_cast<std::uint64_t>(remainder) * divisions + intervalSamps / 2u) / intervalSamps;
	return wholeIntervals * static_cast<std::int64_t>(divisions) + static_cast<std::int64_t>(cell);
}

std::int64_t MidiQuantisation::BoundarySampleAt(std::int64_t index,
	std::uint64_t intervalSamps, std::uint64_t divisions) noexcept
{
	if (intervalSamps == 0u || divisions == 0u)
		return 0;
	const auto denominator = static_cast<std::int64_t>(divisions);
	const auto numerator = index * static_cast<std::int64_t>(intervalSamps) + denominator / 2;
	auto quotient = numerator / denominator;
	if (numerator < 0 && (numerator % denominator) != 0)
		--quotient;
	return quotient;
}

void MidiQuantisation::BuildQuantisedPlaybackEvents(const MidiEvent* src,
	std::size_t eventCount, std::uint32_t loopLength,
	const MidiQuantisationSettings& settings, std::uint64_t transportStartSamps,
	MidiEvent* dst, bool sort) noexcept
{
	if (nullptr == src || nullptr == dst || eventCount == 0u || loopLength == 0u || !settings.Enabled)
	{
		if (src != nullptr && dst != nullptr && eventCount > 0u)
			std::memcpy(dst, src, eventCount * sizeof(MidiEvent));
		return;
	}
	const auto remote = settings.HasRemoteGrid();
	const auto divisions = settings.GridDivisions();
	const auto interval = settings.GridInterval();
	const auto origin = remote ? settings.RemoteOriginSamps : 0ll;
	if (divisions == 0u || interval == 0u)
	{
		std::memcpy(dst, src, eventCount * sizeof(MidiEvent));
		return;
	}
	// Both local and remote grids use one absolute origin and rounded rational
	// boundaries. A loop whose length is a whole number of intervals repeats on
	// exactly the same boundaries, even when interval/divisions is fractional.
	std::array<std::vector<std::int64_t>, MidiEvent::PairingSlotCount> pendingDeltas;
	std::array<std::size_t, MidiEvent::PairingSlotCount> pendingReadIndex{};
	for (std::size_t i = 0u; i < eventCount; ++i)
	{
		auto event = src[i];
		const auto slot = event.PairingSlot();
		if (event.IsNoteOn() && event.HasExactTiming())
			pendingDeltas[slot].push_back(0);
		else if (event.IsNoteOn() && event.sampleOffset < loopLength)
		{
			const auto absolute = static_cast<std::int64_t>(transportStartSamps) + event.sampleOffset;
			const auto index = NearestBoundaryIndex(absolute - origin,
				interval, divisions);
			const auto boundary = origin + BoundarySampleAt(index, interval, divisions);
			const auto local = boundary - static_cast<std::int64_t>(transportStartSamps) + settings.PhaseOffsetSamps;
			const auto wrapped = ((local % static_cast<std::int64_t>(loopLength)) + loopLength) % loopLength;
			event.sampleOffset = static_cast<std::uint32_t>(wrapped);
			pendingDeltas[slot].push_back(static_cast<std::int64_t>(event.sampleOffset)
				- static_cast<std::int64_t>(src[i].sampleOffset));
		}
		else if (event.IsNoteOff())
		{
			auto& deltas = pendingDeltas[slot];
			auto& readIndex = pendingReadIndex[slot];
			if (readIndex < deltas.size())
			{
				const auto shifted = static_cast<std::int64_t>(event.sampleOffset) + deltas[readIndex++];
				const auto loopEnd = static_cast<std::int64_t>(loopLength);
				event.sampleOffset = shifted < 0 ? 0u : static_cast<std::uint32_t>(
					shifted >= loopEnd ? loopEnd - 1 : shifted);
			}
		}
		dst[i] = event;
	}
	if (sort) MidiNote::SortMidiEvents(dst, eventCount);
}
