#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "MidiEvent.h"

namespace midi
{
	// Fractional grid resolutions for MIDI start-time quantisation, expressed as
	// fractions of the current grain size. Playback boundaries are rounded from
	//   boundary(k) = round(k * grainSamps / divisor)
	// so fractional sample spacing does not accumulate drift across grains.
	enum class MidiQuantisationFraction : std::uint8_t
	{
		Whole = 0,        // 1   * grain
		Half = 1,         // 1/2 * grain
		Quarter = 2,      // 1/4 * grain
		Eighth = 3,       // 1/8 * grain
		Sixteenth = 4,    // 1/16 * grain
		ThirtySecond = 5, // 1/32 * grain
		// Append only: these ordinals are stored in packed settings and sessions.
		Third = 6,        // 1/3 * grain
		Sixth = 7,        // 1/6 * grain
		Twelfth = 8,      // 1/12 * grain
	};

	static constexpr std::uint8_t MidiQuantisationFractionCount = 9u;
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
	private:
		static constexpr std::size_t TotalNoteSlots = 16u * 128u;
		static constexpr std::size_t NoteSlot(std::uint8_t channel, std::uint8_t note) noexcept;

	public:
		// Stored ordinals, including GUI payloads; use display helpers for presentation.
		static constexpr int FractionIndex(MidiQuantisationFraction fraction) noexcept
		{
			return static_cast<int>(fraction);
		}

		static constexpr MidiQuantisationFraction ClampFractionIndex(int index) noexcept
		{
			if (index < 0)
				index = 0;
			else if (index >= static_cast<int>(MidiQuantisationFractionCount))
				return MidiQuantisationFraction::ThirtySecond; // Preserve legacy invalid-value fallback.

			return static_cast<MidiQuantisationFraction>(index);
		}

		// Display/drag order follows increasing density, independent of stored ordinals.
		static constexpr MidiQuantisationFraction FractionDisplayOrder[] = {
			MidiQuantisationFraction::Whole, MidiQuantisationFraction::Half,
			MidiQuantisationFraction::Third, MidiQuantisationFraction::Quarter,
			MidiQuantisationFraction::Sixth, MidiQuantisationFraction::Eighth,
			MidiQuantisationFraction::Twelfth, MidiQuantisationFraction::Sixteenth,
			MidiQuantisationFraction::ThirtySecond
		};

		static constexpr int FractionDisplayIndex(MidiQuantisationFraction fraction) noexcept
		{
			for (int index = 0; index < MidiQuantisationFractionCount; ++index)
				if (FractionDisplayOrder[index] == fraction)
					return index;
			return 0;
		}

		static constexpr MidiQuantisationFraction ClampFractionDisplayIndex(int index) noexcept
		{
			return FractionDisplayOrder[index < 0 ? 0 :
				(index >= MidiQuantisationFractionCount ? MidiQuantisationFractionCount - 1 : index)];
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
			case MidiQuantisationFraction::Third:        return 3u;
			case MidiQuantisationFraction::Sixth:        return 6u;
			case MidiQuantisationFraction::Twelfth:      return 12u;
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
			case MidiQuantisationFraction::Third:        return "1/3";
			case MidiQuantisationFraction::Sixth:        return "1/6";
			case MidiQuantisationFraction::Twelfth:      return "1/12";
			}
			return "?";
		}

		// Integer approximation used by the legacy sample-offset helper and UI.
		// Playback uses rounded rational boundaries instead.
		static constexpr std::uint32_t StepSamps(const MidiQuantisationSettings& settings) noexcept;
		static std::int64_t NearestBoundaryIndex(std::int64_t relativeSamps,
			std::uint64_t intervalSamps, std::uint64_t divisions) noexcept;
		static std::int64_t BoundarySampleAt(std::int64_t index,
			std::uint64_t intervalSamps, std::uint64_t divisions) noexcept;

		static MidiQuantisationSettings ApplyGesture(const MidiQuantisationSettings& current,
			MidiQuantisationGesture gesture,
			MidiQuantisationFraction fraction,
			std::uint32_t resolvedGrainSamps) noexcept;
		static std::uint32_t ResolveGestureGrain(const MidiQuantisationGrainCandidates& candidates) noexcept;
		static MidiQuantisationSettings ApplyGuiPayload(const MidiQuantisationSettings& current,
			const int* values,
			std::size_t valueCount) noexcept;

		// Snap `offset` to the nearest grid point at
		//   phaseOffsetSamps + (k * step)
		// then wrap into [0, loopLength).
		// `step == 0` or `loopLength == 0` returns `offset` unchanged.
		static std::uint32_t QuantiseSampleOffset(std::uint32_t offset,
			std::uint32_t step,
			std::uint32_t loopLength,
			std::int32_t phaseOffsetSamps = 0) noexcept;

		// Build a quantised view of a raw event stream into `dst`. The source
		// array is treated as recorded order (NoteOn precedes its matching NoteOff
		// for the same channel+note pair). For each NoteOn:
		//   - NoteOn offset is snapped to the nearest phase-shifted `step`
		//     multiple, wrapped.
		//   - The matching NoteOff is shifted by the same delta as its NoteOn,
		//     preserving the recorded duration.
		//   - If the shifted NoteOff would land at or past `loopLength`, it is
		//     clamped to `loopLength - 1` so playback stays inside the loop.
		// Non-note events (and unpaired note events) keep their original
		// timestamps.
		// `dst` must have capacity for `eventCount` entries; allocation is the
		// caller's responsibility. Pairing scratch may allocate; use this routine
		// only from non-realtime publication paths.
		static void QuantiseEvents(const MidiEvent* src,
			std::size_t eventCount,
			std::uint32_t loopLength,
			std::uint32_t stepSamps,
			MidiEvent* dst,
			std::int32_t phaseOffsetSamps = 0) noexcept;

		// Build the canonical quantised event view used by playback and rendering.
		// This is intended for non-realtime publication paths; callers still own
		// the destination storage.
		static void BuildQuantisedPlaybackEvents(const MidiEvent* src,
			std::size_t eventCount,
			std::uint32_t loopLength,
			std::uint32_t stepSamps,
			MidiEvent* dst,
			std::int32_t phaseOffsetSamps = 0) noexcept;

		static void BuildQuantisedPlaybackEvents(const MidiEvent* src,
			std::size_t eventCount,
			std::uint32_t loopLength,
			const MidiQuantisationSettings& settings,
			std::uint64_t transportStartSamps,
			// False retains source event indices for UI target attribution.
			MidiEvent* dst, bool sort = true) noexcept;
	};

	// Per-LoopTake / per-MidiLoop quantisation settings. Non-destructive: applied
	// when reading events; the underlying recorded events are never modified.
	struct MidiQuantisationSettings
	{
		bool Enabled = false;
		MidiQuantisationFraction Fraction = MidiQuantisationFraction::Quarter;
		std::uint32_t GrainSamps = 0u;
		// Take-local phase offset in samples. LoopTake composes this with inherited
		// station/global offsets before publishing settings to MidiLoop.
		std::int32_t PhaseOffsetSamps = 0;
		// Live tap base grid, independent of construction grain and persistence.
		std::uint32_t BaseIntervalSamps = 0u;
		std::uint32_t BaseDivisions = 0u;
		constexpr bool HasBaseGrid() const noexcept { return BaseIntervalSamps > 0u && BaseDivisions > 0u; }
		constexpr std::uint64_t GridInterval() const noexcept { return HasRemoteGrid() ? RemoteIntervalSamps : (HasBaseGrid() ? BaseIntervalSamps : GrainSamps); }
		constexpr std::uint64_t GridDivisions() const noexcept { return static_cast<std::uint64_t>(HasRemoteGrid() ? (HasBaseGrid() ? BaseDivisions : RemoteBpi) : (HasBaseGrid() ? BaseDivisions : 1u)) * MidiQuantisation::Divisor(Fraction); }
		// Live NINJAM transport snapshot. These are not persisted in Pack().
		std::uint32_t RemoteIntervalSamps = 0u;
		std::uint32_t RemoteBpi = 0u;
		std::int64_t RemoteOriginSamps = 0;

		constexpr bool HasRemoteGrid() const noexcept
		{
			return RemoteIntervalSamps > 0u && RemoteBpi > 0u;
		}

		constexpr std::uint64_t Pack() const noexcept
		{
			const auto fraction = static_cast<std::uint64_t>(Fraction);
			const auto phaseMin = static_cast<std::int64_t>((std::numeric_limits<std::int16_t>::lowest)());
			const auto phaseMax = static_cast<std::int64_t>((std::numeric_limits<std::int16_t>::max)());
			const auto clampedPhase = (PhaseOffsetSamps < phaseMin) ? phaseMin :
				(PhaseOffsetSamps > phaseMax ? phaseMax : static_cast<std::int64_t>(PhaseOffsetSamps));
			const auto zigzagPhase = static_cast<std::uint64_t>((clampedPhase << 1) ^ (clampedPhase >> 15));
			return (Enabled ? 1ull : 0ull)
				| (fraction << 8u)
				| (static_cast<std::uint64_t>(GrainSamps) << 16u)
				| (zigzagPhase << 48u);
		}

		static constexpr MidiQuantisationSettings Unpack(std::uint64_t packed) noexcept
		{
			MidiQuantisationSettings settings;
			settings.Enabled = (packed & 1ull) != 0ull;
			settings.Fraction = MidiQuantisation::ClampFractionIndex(static_cast<int>((packed >> 8u) & 0xffull));
			settings.GrainSamps = static_cast<std::uint32_t>((packed >> 16u) & 0xffffffffull);
			const auto zigzagPhase = static_cast<std::int64_t>((packed >> 48u) & 0xffffull);
			settings.PhaseOffsetSamps = static_cast<std::int32_t>((zigzagPhase >> 1) ^ (-(zigzagPhase & 1ll)));
			return settings;
		}

		constexpr bool operator==(const MidiQuantisationSettings& o) const noexcept
		{
			return Enabled == o.Enabled
				&& Fraction == o.Fraction
				&& GrainSamps == o.GrainSamps
				&& PhaseOffsetSamps == o.PhaseOffsetSamps
				&& BaseIntervalSamps == o.BaseIntervalSamps
				&& BaseDivisions == o.BaseDivisions
				&& RemoteIntervalSamps == o.RemoteIntervalSamps
				&& RemoteBpi == o.RemoteBpi
				&& RemoteOriginSamps == o.RemoteOriginSamps;
		}
		constexpr bool operator!=(const MidiQuantisationSettings& o) const noexcept
		{
			return !(*this == o);
		}
	};

	struct MidiQuantisationGrainCandidates
	{
		std::uint32_t ResolvedTakeGrainSamps = 0u;
		std::uint32_t PublishedSceneGrainSamps = 0u;
		std::uint32_t SingleGrainLoopSamps = 0u;
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
