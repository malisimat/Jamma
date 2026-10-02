#pragma once

#include <algorithm>
#include <array>
#include <numeric>
#include <optional>
#include <vector>
#include "MidiLoop.h"
#include "LoopGridGeometry.h"

namespace midi
{
	// UI-thread source identities follow the same transform and stable ordering
	// as playback. Pairing follows ExtractSpans, including retriggers and the seam.
	struct MidiGridTargets
	{
		struct Note
		{
			std::uint32_t Start, End;
			std::uint8_t Pitch;
			std::size_t On, Off;
			bool Ambiguous;
		};
		struct Target
		{
			std::uint32_t Start = 0u, End = 0u;
			std::uint8_t Pitch = 0u;
			std::optional<std::size_t> NoteIndex;
		};
		std::vector<Note> Notes;
		static MidiGridTargets Build(const MidiLoop::EditState& source)
		{
			MidiGridTargets result;
			const auto count = source.EventCount;
			if (count == 0u) return result;
			std::vector<MidiEvent> displayed(count);
			MidiQuantisation::BuildQuantisedPlaybackEvents(source.Events.data(), count,
				source.LoopLengthSamps, source.Quantisation, source.QuantisationTransportStartSamps,
				displayed.data(), false);
			std::vector<std::size_t> order(count), off(count, count);
			std::vector<bool> ambiguous(count, false);
			std::array<std::size_t, MidiEvent::PairingSlotCount> head, tail;
			head.fill(count); tail.fill(count);
			std::vector<std::size_t> next(count, count);
			for (std::size_t i = 0; i < count; ++i)
			{
				const auto& ev = source.Events[i];
				if (ev.sampleOffset >= source.LoopLengthSamps) continue;
				const auto slot = ev.PairingSlot();

				if (ev.IsNoteOn())
				{
					if (head[slot] != count)
					{
						ambiguous[i] = true;
						for (auto j = head[slot]; j != count; j = next[j]) ambiguous[j] = true;
					}
					if (head[slot] == count) head[slot] = i;
					else next[tail[slot]] = i;
					tail[slot] = i;
				}
				else if (ev.IsNoteOff() && head[slot] != count)
				{
					off[head[slot]] = i;
					head[slot] = next[head[slot]];
				}
			}
			std::iota(order.begin(), order.end(), 0u);
			const auto priority = [](const MidiEvent& e) { return e.IsNoteOff() ? 0 : e.IsNoteOn() ? 2 : 1; };
			std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
				return displayed[a].sampleOffset != displayed[b].sampleOffset
					? displayed[a].sampleOffset < displayed[b].sampleOffset
					: priority(displayed[a]) < priority(displayed[b]);
			});
			std::array<std::size_t, MidiEvent::PairingSlotCount> active;
			active.fill(count);
			const auto emit = [&](std::size_t on, std::uint32_t end, std::size_t displayedOff) {
				const auto& ev = displayed[on];
				if (end > ev.sampleOffset)
					result.Notes.push_back({ev.sampleOffset, end, ev.data1, on, off[on],
						ambiguous[on] || off[on] != displayedOff});
			};
			for (auto i : order)
			{
				const auto& ev = displayed[i];
				if (ev.sampleOffset >= source.LoopLengthSamps) continue;
				auto& on = active[ev.PairingSlot()];
				if (ev.IsNoteOn())
				{
					if (on != count) emit(on, ev.sampleOffset, count);
					on = i;
				}
				else if (ev.IsNoteOff() && on != count)
				{
					emit(on, ev.sampleOffset, i);
					on = count;
				}
			}
			for (auto on : active) if (on != count) emit(on, source.LoopLengthSamps, count);
			return result;
		}
		Target Resolve(std::uint32_t sample, std::uint8_t pitch,
			const LoopGridGeometry* grid) const
		{
			Target result{sample, sample + 1u, pitch, std::nullopt};
			// Last rendered span wins for visible channel overlaps.
			for (std::size_t i = 0; i < Notes.size(); ++i)
				if (Notes[i].Pitch == pitch && Notes[i].Start <= sample && sample < Notes[i].End)
					result = {Notes[i].Start, Notes[i].End, pitch, i};
			if (!result.NoteIndex && grid)
			{
				const auto cell = grid->CellAt(sample);
				result.Start = grid->Boundaries[cell]; result.End = grid->Boundaries[cell + 1u];
			}
			return result;
		}
	};
}
