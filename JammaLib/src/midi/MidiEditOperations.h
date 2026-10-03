#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "MidiLoop.h"
#include "MidiNote.h"
#include "MidiGridTargets.h"

namespace midi
{
	// Pure source transforms. The caller edits a detached revision snapshot and
	// publishes the whole gesture once through LoopTake::PublishMidiEdit.
	struct MidiEditOperations
	{
		// Delete complete displayed notes under the destination across channels,
		// then move with remapped endpoint identities in one detached transaction.
		static bool MoveSnappedReplacingOverlaps(MidiLoop::EditState& state,
			std::size_t onIndex, std::size_t offIndex, std::uint32_t start,
			std::uint32_t end, std::uint8_t pitch)
		{
			if (onIndex >= state.EventCount || offIndex > state.EventCount) return false;
			const auto targets = MidiGridTargets::Build(state);
			std::vector<bool> removed(state.EventCount, false);
			for (const auto& note : targets.Notes)
			{
				if (note.On == onIndex || note.Pitch != pitch
					|| note.Start >= end || note.End <= start) continue;
				if (note.Ambiguous) return false;
				removed[note.On] = true;
				if (note.Off < state.EventCount) removed[note.Off] = true;
			}
			auto candidate = state;
			std::size_t kept = 0u, movedOn = 0u, movedOff = 0u;
			for (std::size_t i = 0u; i < state.EventCount; ++i)
			{
				if (i == onIndex) movedOn = kept;
				if (i == offIndex) movedOff = kept;
				if (!removed[i]) candidate.Events[kept++] = state.Events[i];
			}
			candidate.EventCount = kept;
			if (offIndex == state.EventCount) movedOff = kept;
			if (!MoveSnapped(candidate, movedOn, movedOff, start, end, pitch)) return false;
			state = candidate;
			return true;
		}

		static bool SetVelocity(MidiLoop::EditState& state, std::size_t on, int velocity) noexcept
		{
			if (on >= state.EventCount || !state.Events[on].IsNoteOn() || velocity < 1 || velocity > 127) return false;
			state.Events[on].data2 = static_cast<std::uint8_t>(velocity);
			return true;
		}

		// Preserve endpoint identity and raw duration; every other event must project
		// identically after the move. Work remains detached until all checks pass.
		static bool MoveSnapped(MidiLoop::EditState& state, std::size_t onIndex,
			std::size_t offIndex, std::uint32_t start, std::uint32_t end, std::uint8_t pitch)
		{
			if (onIndex >= state.EventCount || !state.Events[onIndex].IsNoteOn()
				|| start >= end || end > state.LoopLengthSamps || pitch > 127u) return false;
			if (!LoopGridGeometry::Resolve(state.LoopLengthSamps, state.Quantisation, state.QuantisationTransportStartSamps)) return false;
			const auto original = state;
			const auto on = original.Events[onIndex];
			const auto priorTargets = MidiGridTargets::Build(original);
			const auto captured = std::find_if(priorTargets.Notes.begin(), priorTargets.Notes.end(), [&](const auto& n) { return n.On == onIndex; });
			if (captured == priorTargets.Notes.end() || captured->Ambiguous || captured->Off != offIndex) return false;
			const bool hasOff = offIndex < original.EventCount;
			if (hasOff && (!original.Events[offIndex].IsNoteOff()
				|| original.Events[offIndex].PairingSlot() != on.PairingSlot())) return false;
			const auto duration = (hasOff ? original.Events[offIndex].sampleOffset : original.LoopLengthSamps) - on.sampleOffset;
			const auto& q = original.Quantisation;
			const auto interval = static_cast<std::uint64_t>(q.HasRemoteGrid() ? q.RemoteIntervalSamps : q.GrainSamps);
			const auto divisions = static_cast<std::uint64_t>(q.HasRemoteGrid() ? q.RemoteBpi : 1u) * MidiQuantisation::Divisor(q.Fraction);
			if (!q.Enabled || !interval || !divisions) return false;
			const auto transport = static_cast<std::int64_t>(original.QuantisationTransportStartSamps);
			const auto origin = q.HasRemoteGrid() ? q.RemoteOriginSamps : 0ll;
			const auto first = MidiQuantisation::NearestBoundaryIndex(transport - origin, interval, divisions);
			// Exact-timing endpoints already have their inverse: validate it once.
			const auto last = on.HasExactTiming() ? first
				: MidiQuantisation::NearestBoundaryIndex(transport - origin + original.LoopLengthSamps - 1u, interval, divisions);
			if (last < first || last - first > 8192) return false;
			std::vector<MidiEvent> displayed(original.EventCount);
			MidiQuantisation::BuildQuantisedPlaybackEvents(original.Events.data(), original.EventCount,
				original.LoopLengthSamps, q, original.QuantisationTransportStartSamps, displayed.data(), false);
			for (auto boundary = first; boundary <= last; ++boundary)
			{
				const auto localBoundary = origin + MidiQuantisation::BoundarySampleAt(boundary, interval, divisions) - transport + q.PhaseOffsetSamps;
				const auto wrapped = (localBoundary % original.LoopLengthSamps + original.LoopLengthSamps) % original.LoopLengthSamps;
				if (!on.HasExactTiming() && wrapped != start) continue;
				const auto raw = on.HasExactTiming() ? static_cast<std::int64_t>(start)
					: std::clamp<std::int64_t>(origin + MidiQuantisation::BoundarySampleAt(boundary, interval, divisions) - transport,
						0, original.LoopLengthSamps - 1u);
				if (duration > original.LoopLengthSamps - raw || (hasOff && raw + duration >= original.LoopLengthSamps)
					|| (!hasOff && raw + duration != original.LoopLengthSamps)) continue;
				auto candidate = original;
				candidate.Events[onIndex].sampleOffset = static_cast<std::uint32_t>(raw);
				candidate.Events[onIndex].data1 = pitch;
				if (hasOff) { candidate.Events[offIndex].sampleOffset = static_cast<std::uint32_t>(raw + duration); candidate.Events[offIndex].data1 = pitch; }
				std::vector<std::size_t> order(candidate.EventCount);
				std::iota(order.begin(), order.end(), 0u);
				const auto priority = [](const MidiEvent& e) { return e.IsNoteOff() ? 0 : e.IsNoteOn() ? 2 : 1; };
				std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
					return candidate.Events[a].sampleOffset != candidate.Events[b].sampleOffset
						? candidate.Events[a].sampleOffset < candidate.Events[b].sampleOffset : priority(candidate.Events[a]) < priority(candidate.Events[b]); });
				auto sorted = candidate;
				std::size_t movedOn = 0;
				for (std::size_t i = 0; i < order.size(); ++i) { sorted.Events[i] = candidate.Events[order[i]]; if (order[i] == onIndex) movedOn = i; }
				std::vector<MidiEvent> projected(sorted.EventCount);
				MidiQuantisation::BuildQuantisedPlaybackEvents(sorted.Events.data(), sorted.EventCount,
					sorted.LoopLengthSamps, q, sorted.QuantisationTransportStartSamps, projected.data(), false);
				bool unchanged = true;
				for (std::size_t i = 0; i < order.size(); ++i)
				{
					if (order[i] == onIndex || order[i] == offIndex) continue;
					if (projected[i].sampleOffset != displayed[order[i]].sampleOffset) unchanged = false;
				}
				const auto targets = MidiGridTargets::Build(sorted);
				const auto moved = std::find_if(targets.Notes.begin(), targets.Notes.end(), [&](const auto& n) { return n.On == movedOn; });
				if (!unchanged || moved == targets.Notes.end() || moved->Ambiguous || moved->Start != start || moved->End != end || moved->Pitch != pitch) continue;
				// A destination overlap must not truncate any other displayed note.
				const auto& prior = priorTargets;
				if (targets.Notes.size() != prior.Notes.size()) continue;
				for (const auto& n : prior.Notes)
				{
					if (n.On == onIndex) continue;
					const auto index = static_cast<std::size_t>(std::find(order.begin(), order.end(), n.On) - order.begin());
					const auto other = std::find_if(targets.Notes.begin(), targets.Notes.end(), [&](const auto& t) { return t.On == index; });
					if (other == targets.Notes.end() || other->Start != n.Start || other->End != n.End || other->Ambiguous != n.Ambiguous) unchanged = false;
				}
				if (!unchanged) continue;
				state = sorted; return true;
			}
			return false;
		}

		static bool Create(MidiLoop::EditState& state, std::uint32_t start,
			std::uint32_t duration, std::uint8_t channel, std::uint8_t pitch,
			std::uint8_t velocity = 96u) noexcept
		{
			if (state.LoopLengthSamps == 0u || duration == 0u
				|| start >= state.LoopLengthSamps
				|| duration > state.LoopLengthSamps - start
				|| state.EventCount > MidiLoop::DefaultCapacity
					- (start + duration == state.LoopLengthSamps ? 1u : 2u)
				|| channel >= 16u || pitch >= 128u || velocity == 0u)
				return false;
			const auto end = start + duration;
			state.Events[state.EventCount++] = MidiEvent::MakeNoteOn(start, channel, pitch, velocity);
			// ExtractSpans extends an unmatched on to the seam. Playback flushes
			// it there, preserving a true one-sample final cell.
			if (end < state.LoopLengthSamps)
				state.Events[state.EventCount++] = MidiEvent::MakeNoteOff(end, channel, pitch);
			MidiNote::SortMidiEvents(state.Events.data(), state.EventCount);
			return true;
		}

		static bool RemoveNote(MidiLoop::EditState& state, std::size_t onIndex) noexcept
		{
			if (onIndex >= state.EventCount || !state.Events[onIndex].IsNoteOn()) return false;
			const auto on = state.Events[onIndex];
			std::size_t off = state.EventCount;
			// Any still active predecessor makes this source identity ambiguous.
			bool active = false;
			for (std::size_t i = 0; i < onIndex; ++i)
				if (state.Events[i].PairingSlot() == on.PairingSlot())
				{
					if (state.Events[i].IsNoteOn()) active = true;
					if (state.Events[i].IsNoteOff()) active = false;
				}
			if (active) return false;
			for (std::size_t i = onIndex + 1u; i < state.EventCount; ++i)
			{
				const auto& ev = state.Events[i];
				if (ev.sampleOffset >= state.LoopLengthSamps) break;
				if (ev.PairingSlot() != on.PairingSlot()) continue;
				if (ev.IsNoteOn()) return false;
				if (ev.IsNoteOff()) { off = i; break; }
			}
			std::size_t kept = 0;
			for (std::size_t i = 0; i < state.EventCount; ++i)
				if (i != onIndex && i != off) state.Events[kept++] = state.Events[i];
			state.EventCount = kept;
			return true;
		}

		// Exact editor timing represents every physical cell, including seam
		// fragments with no inverse snapped onset. Both endpoints carry the flag.
		static bool CreateExact(MidiLoop::EditState& state, std::uint32_t start,
			std::uint32_t end, std::uint8_t channel, std::uint8_t pitch) noexcept
		{
			if (start >= end || end > state.LoopLengthSamps
				|| channel >= 16u || pitch >= 128u) return false;
			if (state.EventCount > MidiLoop::DefaultCapacity - (end == state.LoopLengthSamps ? 1u : 2u))
				return false;
			state.Events[state.EventCount++] = MidiEvent::MakeNoteOn(start, channel, pitch, 96u)
				.WithFlags(MidiEvent::ExactTiming);
			if (end < state.LoopLengthSamps)
				state.Events[state.EventCount++] = MidiEvent::MakeNoteOff(end, channel, pitch)
					.WithFlags(MidiEvent::ExactTiming);
			MidiNote::SortMidiEvents(state.Events.data(), state.EventCount);
			return true;
		}

		// Event index identifies the exact NoteOn in this revision. Reject an
		// overlapping duplicate whose NoteOff pairing is ambiguous.
		static bool MoveOrTrim(MidiLoop::EditState& state, std::size_t onIndex,
			std::uint32_t newStart, std::uint32_t newEnd,
			std::uint8_t newPitch) noexcept
		{
			if (onIndex >= state.EventCount || !state.Events[onIndex].IsNoteOn()
				|| newPitch >= 128u || newStart >= newEnd
				|| newEnd > state.LoopLengthSamps) return false;
			const auto on = state.Events[onIndex];
			std::size_t offIndex = state.EventCount;
			for (std::size_t i = onIndex + 1u; i < state.EventCount; ++i)
			{
				const auto& ev = state.Events[i];
				if (ev.PairingSlot() != on.PairingSlot()) continue;
				if (ev.IsNoteOn()) return false;
				if (ev.IsNoteOff()) { offIndex = i; break; }
			}
			if (offIndex == state.EventCount && state.EventCount == MidiLoop::DefaultCapacity
				&& newEnd < state.LoopLengthSamps) return false;
			state.Events[onIndex].sampleOffset = newStart;
			state.Events[onIndex].data1 = newPitch;
			if (offIndex < state.EventCount)
			{
				if (newEnd == state.LoopLengthSamps)
				{
					for (std::size_t i = offIndex + 1u; i < state.EventCount; ++i)
						state.Events[i - 1u] = state.Events[i];
					--state.EventCount;
				}
				else
				{
					state.Events[offIndex].sampleOffset = newEnd;
					state.Events[offIndex].data1 = newPitch;
				}
			}
			else if (newEnd < state.LoopLengthSamps)
				state.Events[state.EventCount++] = MidiEvent::MakeNoteOff(newEnd, on.Channel(), newPitch)
					.WithFlags(on.flags);
			MidiNote::SortMidiEvents(state.Events.data(), state.EventCount);
			return true;
		}

		// A quantised cell maps to one source note only when its displayed
		// occupancy is unambiguous. Splits retain that note's velocity/channel.
		// The caller applies cells to a detached state and discards it on failure.
		static bool SetCell(MidiLoop::EditState& state,
			std::uint32_t cellStart, std::uint32_t cellEnd,
			std::uint8_t pitch, std::uint8_t channel, bool fill,
			const MidiQuantisationSettings& quantisation,
			std::uint64_t transportStart) noexcept
		{
			if (!quantisation.Enabled || (!quantisation.GrainSamps && !quantisation.HasRemoteGrid())
				|| cellStart >= cellEnd || cellEnd > state.LoopLengthSamps
				|| pitch >= 128u || channel >= 16u) return false;
			const auto midpoint = cellStart + (cellEnd - cellStart) / 2u;
			std::size_t matchingOn = state.EventCount;
			std::size_t matchingOff = state.EventCount;
			std::uint32_t displayedStart = 0u;
			std::uint32_t displayedEnd = 0u;
			std::size_t matches = 0u;
			for (std::size_t onIndex = 0u; onIndex < state.EventCount; ++onIndex)
			{
				const auto& on = state.Events[onIndex];
				if (!on.IsNoteOn() || on.data1 != pitch
					|| on.sampleOffset >= state.LoopLengthSamps) continue;
				std::size_t offIndex = state.EventCount;
				for (std::size_t i = onIndex + 1u; i < state.EventCount; ++i)
				{
					const auto& ev = state.Events[i];
					if (ev.Channel() != on.Channel() || ev.data1 != pitch) continue;
					if (ev.IsNoteOn()) return false; // duplicate pairing is ambiguous
					if (ev.IsNoteOff()) { offIndex = i; break; }
				}
				std::uint32_t start = 0u;
				std::uint32_t end = 0u;
				if (!DisplayedSpan(on, offIndex < state.EventCount
					? &state.Events[offIndex] : nullptr, state.LoopLengthSamps,
					quantisation, transportStart, start, end)) return false;
				if (start <= midpoint && midpoint < end)
				{
					++matches;
					matchingOn = onIndex;
					matchingOff = offIndex;
					displayedStart = start;
					displayedEnd = end;
				}
			}
			if (fill)
			{
				if (matches != 0u) return false;
				const auto length = static_cast<std::int64_t>(state.LoopLengthSamps);
				const auto shifted = static_cast<std::int64_t>(cellStart)
					- quantisation.PhaseOffsetSamps;
				const auto rawStart = static_cast<std::uint32_t>((shifted % length + length) % length);
				const auto duration = cellEnd - cellStart;
				if (cellEnd < state.LoopLengthSamps
					&& duration > state.LoopLengthSamps - rawStart) return false;
				const auto rawEnd = cellEnd == state.LoopLengthSamps
					? state.LoopLengthSamps : rawStart + duration;
				const auto on = MidiEvent::MakeNoteOn(rawStart, channel, pitch, 96u);
				const auto off = MidiEvent::MakeNoteOff(rawEnd, channel, pitch);
				std::uint32_t start = 0u, end = 0u;
				if (!DisplayedSpan(on, rawEnd < state.LoopLengthSamps ? &off : nullptr,
					state.LoopLengthSamps, quantisation, transportStart, start, end)
					|| start != cellStart || end != cellEnd) return false;
				return Create(state, rawStart, rawEnd - rawStart, channel, pitch);
			}
			if (matches != 1u || cellStart < displayedStart || cellEnd > displayedEnd)
				return false;
			const auto on = state.Events[matchingOn];
			const auto rawEnd = matchingOff < state.EventCount
				? state.Events[matchingOff].sampleOffset : state.LoopLengthSamps;
			const auto shift = static_cast<std::int64_t>(displayedStart)
				- static_cast<std::int64_t>(on.sampleOffset);
			const auto cutStart = cellStart == displayedStart
				? static_cast<std::int64_t>(on.sampleOffset)
				: static_cast<std::int64_t>(cellStart) - shift;
			const auto cutEnd = cellEnd == displayedEnd
				? static_cast<std::int64_t>(rawEnd)
				: static_cast<std::int64_t>(cellEnd) - shift;
			if (cutStart < on.sampleOffset || cutEnd > rawEnd || cutStart >= cutEnd)
				return false;
			// Verify both fragments project to exactly the coverage outside this
			// cell. Rounding or a remote origin may make the inverse ambiguous.
			if (cutStart > on.sampleOffset)
			{
				const auto off = MidiEvent::MakeNoteOff(static_cast<std::uint32_t>(cutStart),
					on.Channel(), pitch);
				std::uint32_t start = 0u, end = 0u;
				if (!DisplayedSpan(on, &off, state.LoopLengthSamps,
					quantisation, transportStart, start, end)
					|| start != displayedStart || end != cellStart) return false;
			}
			if (cutEnd < rawEnd)
			{
				const auto rightOn = MidiEvent::MakeNoteOn(static_cast<std::uint32_t>(cutEnd),
					on.Channel(), pitch, on.data2);
				const auto rightOff = MidiEvent::MakeNoteOff(rawEnd, on.Channel(), pitch);
				std::uint32_t start = 0u, end = 0u;
				if (!DisplayedSpan(rightOn, rawEnd < state.LoopLengthSamps ? &rightOff : nullptr,
					state.LoopLengthSamps, quantisation, transportStart, start, end)
					|| start != cellEnd || end != displayedEnd) return false;
			}
			auto next = state;
			for (std::size_t i = next.EventCount; i-- > 0u;)
			{
				if (i != matchingOn && i != matchingOff) continue;
				for (std::size_t j = i + 1u; j < next.EventCount; ++j)
					next.Events[j - 1u] = next.Events[j];
				--next.EventCount;
			}
			if (cutStart > on.sampleOffset && !Create(next, on.sampleOffset,
				static_cast<std::uint32_t>(cutStart) - on.sampleOffset,
				on.Channel(), pitch, on.data2)) return false;
			if (cutEnd < rawEnd && !Create(next, static_cast<std::uint32_t>(cutEnd),
				rawEnd - static_cast<std::uint32_t>(cutEnd),
				on.Channel(), pitch, on.data2)) return false;
			state = next;
			return true;
		}

	private:
		static bool DisplayedSpan(const MidiEvent& on, const MidiEvent* off,
			std::uint32_t length, const MidiQuantisationSettings& settings,
			std::uint64_t transportStart, std::uint32_t& start,
			std::uint32_t& end) noexcept
		{
			std::array<MidiEvent, 2u> raw{ on, off ? *off : MidiEvent{} };
			std::array<MidiEvent, 2u> quantised{};
			const auto count = off ? 2u : 1u;
			MidiQuantisation::BuildQuantisedPlaybackEvents(raw.data(), count,
				length, settings, transportStart, quantised.data());
			const auto spans = MidiNote::ExtractSpans(quantised.data(), count, length);
			if (spans.size() != 1u) return false;
			start = spans[0].StartSample;
			end = start + spans[0].DurationSamples;
			return true;
		}
	};
}
