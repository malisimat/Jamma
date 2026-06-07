#pragma once

#include <cstddef>
#include <cstdint>

#include "MidiEvent.h"

namespace midi
{
	// Fractional grid resolutions for MIDI start-time quantisation, expressed as
	// fractions of the current grain size. The numeric value is the divisor of
	// the grain (1, 2, 4, 8, 16, 32) so the snap step in samples is
	//   step = grainSamps / divisor
	enum class MidiQuantisationFraction : std::uint8_t
	{
		Whole = 0,        // 1   * grain
		Half = 1,         // 1/2 * grain
		Quarter = 2,      // 1/4 * grain
		Eighth = 3,       // 1/8 * grain
		Sixteenth = 4,    // 1/16 * grain
		ThirtySecond = 5, // 1/32 * grain
	};

	static constexpr std::uint8_t MidiQuantisationFractionCount = 6u;
	static constexpr int MidiQuantisationDragPixelsPerStep = 32;

	enum class MidiQuantisationGesture : std::uint8_t
	{
		Toggle,
		DragFraction
	};

	struct MidiQuantisationSettings;
	struct MidiQuantisationGrainCandidates;

	struct MidiQuantisation
	{
		static constexpr int FractionIndex(MidiQuantisationFraction fraction) noexcept
		{
			return static_cast<int>(fraction);
		}

		static constexpr MidiQuantisationFraction ClampFractionIndex(int index) noexcept
		{
			if (index < 0)
				index = 0;
			else if (index >= static_cast<int>(MidiQuantisationFractionCount))
				index = static_cast<int>(MidiQuantisationFractionCount) - 1;

			return static_cast<MidiQuantisationFraction>(index);
		}

		static int DragSteps(int deltaY) noexcept;
		static MidiQuantisationFraction ResolveDragFraction(MidiQuantisationFraction startFraction,
		                                                   int deltaY) noexcept;

		static constexpr std::uint32_t Divisor(MidiQuantisationFraction fraction) noexcept
		{
			switch (fraction)
			{
			case MidiQuantisationFraction::Whole:        return 1u;
			case MidiQuantisationFraction::Half:         return 2u;
			case MidiQuantisationFraction::Quarter:      return 4u;
			case MidiQuantisationFraction::Eighth:       return 8u;
			case MidiQuantisationFraction::Sixteenth:    return 16u;
			case MidiQuantisationFraction::ThirtySecond: return 32u;
			}
			return 1u;
		}

		static constexpr const char* FractionLabel(MidiQuantisationFraction fraction) noexcept
		{
			switch (fraction)
			{
			case MidiQuantisationFraction::Whole:        return "1";
			case MidiQuantisationFraction::Half:         return "1/2";
			case MidiQuantisationFraction::Quarter:      return "1/4";
			case MidiQuantisationFraction::Eighth:       return "1/8";
			case MidiQuantisationFraction::Sixteenth:    return "1/16";
			case MidiQuantisationFraction::ThirtySecond: return "1/32";
			}
			return "?";
		}

		// Snap-step in samples for the given settings. Returns 0 when quantisation
		// is inactive (disabled or grain not yet known) — callers must treat 0 as
		// a no-op signal.
		static constexpr std::uint32_t StepSamps(const MidiQuantisationSettings& settings) noexcept;

		static MidiQuantisationSettings ApplyGesture(const MidiQuantisationSettings& current,
			MidiQuantisationGesture gesture,
			MidiQuantisationFraction fraction,
			std::uint32_t resolvedGrainSamps) noexcept;
		static std::uint32_t ResolveGestureGrain(const MidiQuantisationGrainCandidates& candidates) noexcept;
		static MidiQuantisationSettings ApplyGuiPayload(const MidiQuantisationSettings& current,
			const int* values,
			std::size_t valueCount) noexcept;

		// Snap `offset` to the nearest multiple of `step`, then wrap into
		// [0, loopLength).
		// `step == 0` or `loopLength == 0` returns `offset` unchanged.
		static std::uint32_t QuantiseSampleOffset(std::uint32_t offset,
			std::uint32_t step,
			std::uint32_t loopLength) noexcept;

		// Build a quantised view of a raw event stream into `dst`. The source
		// array is treated as recorded order (NoteOn precedes its matching NoteOff
		// for the same channel+note pair). For each NoteOn:
		//   - NoteOn offset is snapped to the nearest `step` multiple, wrapped.
		//   - The matching NoteOff is shifted by the same delta as its NoteOn,
		//     preserving the recorded duration.
		//   - If the shifted NoteOff would land at or past `loopLength`, it is
		//     clamped to `loopLength - 1` so playback stays inside the loop.
		// Non-note events (and unpaired note events) keep their original
		// timestamps.
		// `dst` must have capacity for `eventCount` entries; allocation is the
		// caller's responsibility. This routine performs no heap allocation and is
		// safe to call from non-realtime threads.
		static void QuantiseEvents(const MidiEvent* src,
			std::size_t eventCount,
			std::uint32_t loopLength,
			std::uint32_t stepSamps,
			MidiEvent* dst) noexcept;

		// Build the canonical quantised event view used by playback and rendering.
		// This is intended for non-realtime publication paths; callers still own
		// the destination storage.
		static void BuildQuantisedPlaybackEvents(const MidiEvent* src,
			std::size_t eventCount,
			std::uint32_t loopLength,
			std::uint32_t stepSamps,
			MidiEvent* dst) noexcept;
	};

	// Per-LoopTake / per-MidiLoop quantisation settings. Non-destructive: applied
	// when reading events; the underlying recorded events are never modified.
	struct MidiQuantisationSettings
	{
		bool Enabled = false;
		MidiQuantisationFraction Fraction = MidiQuantisationFraction::Whole;
		std::uint32_t GrainSamps = 0u;

		constexpr std::uint64_t Pack() const noexcept
		{
			const auto fraction = static_cast<std::uint64_t>(Fraction);
			return (Enabled ? 1ull : 0ull)
				| (fraction << 8u)
				| (static_cast<std::uint64_t>(GrainSamps) << 16u);
		}

		static constexpr MidiQuantisationSettings Unpack(std::uint64_t packed) noexcept
		{
			MidiQuantisationSettings settings;
			settings.Enabled = (packed & 1ull) != 0ull;
			settings.Fraction = MidiQuantisation::ClampFractionIndex(static_cast<int>((packed >> 8u) & 0xffull));
			settings.GrainSamps = static_cast<std::uint32_t>((packed >> 16u) & 0xffffffffull);
			return settings;
		}

		constexpr bool operator==(const MidiQuantisationSettings& o) const noexcept
		{
			return Enabled == o.Enabled && Fraction == o.Fraction && GrainSamps == o.GrainSamps;
		}
		constexpr bool operator!=(const MidiQuantisationSettings& o) const noexcept
		{
			return !(*this == o);
		}
	};

	struct MidiQuantisationGrainCandidates
	{
		std::uint32_t FirstPlayableMidiLoopSamps = 0u;
		std::uint32_t FirstAudioLoopSamps = 0u;
		std::uint32_t MidiVisualLoopSamps = 0u;
		std::uint32_t RecordedSamps = 0u;
	};

	constexpr std::uint32_t MidiQuantisation::StepSamps(const MidiQuantisationSettings& settings) noexcept
	{
		if (!settings.Enabled || 0u == settings.GrainSamps)
			return 0u;
		const auto divisor = Divisor(settings.Fraction);
		return settings.GrainSamps / divisor;
	}
}
