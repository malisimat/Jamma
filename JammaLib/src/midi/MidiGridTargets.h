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
			std::uint8_t Flags = 0u;
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
			for (const auto& projected : MidiLoop::BuildEditorNotes(source))
			{
				const auto& note = projected.Note;
				result.Notes.push_back({note.StartSample, note.StartSample + note.DurationSamples,
					note.Note, projected.On, projected.Off, projected.Ambiguous, note.Flags});
			}
			return result;
		}
		Target Resolve(std::uint32_t sample, std::uint8_t pitch,
			const LoopGridGeometry* grid) const
		{
			Target result{sample, sample + 1u, pitch, std::nullopt};
			// Last rendered span wins for visible channel overlaps.
			for (std::size_t i = 0; i < Notes.size(); ++i)
			{
				const auto& note = Notes[i];
				const auto span = grid ? grid->EditorSpan(note.Start, note.End)
					: std::pair{note.Start, note.End};
				const auto displayed = grid && sample < span.first && span.second > grid->Boundaries.back()
					? static_cast<std::uint64_t>(sample) + grid->Boundaries.back() : sample;
				if (note.Pitch == pitch && span.first <= displayed && displayed < span.second)
					result = {note.Start, note.End, pitch, i};
			}
			if (!result.NoteIndex && grid)
			{
				const auto cell = grid->CellAt(sample);
				const auto span = grid->EditorSpan(grid->Boundaries[cell], grid->Boundaries[cell + 1u]);
				result.Start = span.first; result.End = span.second;
			}
			return result;
		}
	};
}
