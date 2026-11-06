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

	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Whole;
	settings.RemoteIntervalSamps = stepSamps;
	settings.RemoteBpi = 1u;
	settings.RemoteOriginSamps = phaseOffsetSamps;
	BuildPlaybackEvents(src, eventCount, loopLength, settings, 0u, dst, false);
}

std::vector<MidiQuantisation::NoteSpan> MidiQuantisation::ProjectNotes(const MidiEvent* source,
	std::size_t count, std::uint32_t length, const MidiQuantisationSettings& settings, std::uint64_t transport)
{
	std::vector<NoteSpan> spans;
	if (!source || !length) return spans;
	std::vector<std::vector<std::size_t>> pending(MidiEvent::PairingSlotCount),
		crossing(MidiEvent::PairingSlotCount), releases(MidiEvent::PairingSlotCount);
	std::array<std::size_t, MidiEvent::PairingSlotCount> read{};
	for (std::size_t i = 0; i < count; ++i)
	{
		const auto& event = source[i];
		const auto slot = event.PairingSlot();
		if (event.IsNoteOn())
		{
			if (event.flags & MidiEvent::TriggerSeamEndpoint) crossing[slot].push_back(spans.size());
			else
			{
				const bool overlap = read[slot] < pending[slot].size();
				for (auto j = read[slot]; j < pending[slot].size(); ++j)
					spans[pending[slot][j]].Ambiguous = true;
				pending[slot].push_back(spans.size());
				spans.push_back({i, count, event.sampleOffset, length, !event.HasExactTiming(), overlap});
				continue;
			}
			spans.push_back({i, count, event.sampleOffset, length, !event.HasExactTiming()});
		}
		else if (event.IsNoteOff() && (event.flags & MidiEvent::TriggerSeamEndpoint)) releases[slot].push_back(i);
		else if (event.IsNoteOff() && read[slot] < pending[slot].size())
		{
			auto& span = spans[pending[slot][read[slot]++]];
			span.Off = i; span.End = event.sampleOffset;
			span.BoundaryHeld = (event.flags & MidiEvent::BoundaryHeld) != 0u;
			if (span.BoundaryHeld) ++span.End;
		}
	}
	for (std::size_t slot = 0; slot < crossing.size(); ++slot)
		for (std::size_t i = 0; i < (std::min)(crossing[slot].size(), releases[slot].size()); ++i)
		{
			auto& span = spans[crossing[slot][i]];
			span.Off = releases[slot][i];
			span.End = static_cast<std::int64_t>(length) + source[span.Off].sampleOffset;
			span.BoundaryHeld = (source[span.Off].flags & MidiEvent::BoundaryHeld) != 0u;
			if (span.BoundaryHeld) ++span.End;
		}
	for (auto& span : spans)
	{
		const auto original = span.Start;
		if (settings.Enabled && settings.GridInterval() && settings.GridDivisions()
			&& !source[span.On].HasExactTiming())
		{
			const auto limit = (std::numeric_limits<std::int64_t>::max)() / 4;
			const auto origin = settings.HasRemoteGrid() ? settings.RemoteOriginSamps : 0ll;
			const auto interval = settings.GridInterval(), divisions = settings.GridDivisions();
			// Invalid/extreme rulers keep source timing rather than overflowing a
			// signed clock or rational-boundary intermediate. Valid audio rulers
			// fit comfortably inside these bounds, even after years of playback.
			if (transport <= static_cast<std::uint64_t>(limit) && origin >= -limit && origin <= limit
				&& divisions <= static_cast<std::uint64_t>(limit) / interval)
			{
				const auto clock = static_cast<std::int64_t>(transport);
				const auto relative = clock + original - origin;
				const auto safeRelative = limit / static_cast<std::int64_t>(divisions);
				if (relative >= -safeRelative && relative <= safeRelative)
				{
					const auto index = NearestBoundaryIndex(relative, interval, divisions);
					if (index >= -limit / static_cast<std::int64_t>(interval)
						&& index <= limit / static_cast<std::int64_t>(interval))
						span.Start = origin + BoundarySampleAt(index, interval, divisions) - clock;
				}
			}
		}
		span.Start += settings.PhaseOffsetSamps;
		span.End += span.Start - original;
	}
	return spans;
}

void MidiQuantisation::BuildPlaybackEvents(const MidiEvent* src, std::size_t eventCount,
	std::uint32_t loopLength, const MidiQuantisationSettings& settings,
	std::uint64_t transportStartSamps, MidiEvent* dst, bool sort) noexcept
{
	if (!src || !dst || !eventCount) return;
	std::memcpy(dst, src, eventCount * sizeof(MidiEvent));
	if (!loopLength) return;
	const auto wrap = [loopLength](std::int64_t value) {
		return static_cast<std::uint32_t>((value % loopLength + loopLength) % loopLength);
	};
	for (std::size_t i = 0; i < eventCount; ++i)
		if (src[i].sampleOffset < loopLength) dst[i].sampleOffset = wrap(static_cast<std::int64_t>(src[i].sampleOffset) + settings.PhaseOffsetSamps);
	for (const auto& span : ProjectNotes(src, eventCount, loopLength, settings, transportStartSamps))
	{
		if (src[span.On].sampleOffset >= loopLength || span.End <= span.Start) continue;
		const auto start = wrap(span.Start);
		dst[span.On].sampleOffset = start;
		if (span.Off == eventCount) continue;
		// Unmerged capture suffixes retain their historical seam-release policy.
		if (src[span.Off].sampleOffset >= loopLength && settings.PhaseOffsetSamps == 0
			&& !(src[span.On].flags & MidiEvent::TriggerSeamEndpoint)) continue;
		dst[span.Off].sampleOffset = wrap(span.End);
		for (auto index : {span.On, span.Off})
		{
			dst[index].flags &= static_cast<std::uint8_t>(~MidiEvent::TriggerSeamEndpoint);
			if (start + span.End - span.Start >= loopLength) dst[index].flags |= MidiEvent::TriggerSeamEndpoint;
		}
	}
	if (sort) MidiNote::SortMidiEvents(dst, eventCount);
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
	BuildPlaybackEvents(src, eventCount, loopLength, settings, transportStartSamps, dst, sort);
}
