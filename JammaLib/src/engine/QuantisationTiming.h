#pragma once

#include <cstdint>
#include <optional>

#include "../include/Constants.h"

namespace midi
{
	enum class MidiQuantisationFraction : std::uint8_t;
}

namespace engine
{
	// Exact, frozen audio geometry for locally recorded loops. This deliberately
	// does not describe the musical grid: a grid cell may be fractional in samples.
	struct LocalAudioGeometry
	{
		unsigned long OriginalMasterBufferLengthSamps = 0ul;
		unsigned long MasterLengthSamps = 0ul;
		unsigned int GrainSamps = 0u;
		unsigned int BPI = 0u;

		static std::optional<LocalAudioGeometry> Create(unsigned long originalLength,
			unsigned long masterLength, unsigned int grainSamps, unsigned int bpi) noexcept
		{
			if (originalLength == 0ul || masterLength == 0ul || grainSamps == 0u || bpi == 0u
				|| masterLength > originalLength)
				return std::nullopt;
			const auto product = static_cast<std::uint64_t>(grainSamps) * bpi;
			if (product != masterLength)
				return std::nullopt;
			return LocalAudioGeometry{ originalLength, masterLength, grainSamps, bpi };
		}

		bool IsValid() const noexcept
		{
			return Create(OriginalMasterBufferLengthSamps, MasterLengthSamps, GrainSamps, BPI).has_value();
		}
	};

	enum class QuantisationGridSource : std::uint8_t { Default, Tap, Remote };

	struct QuantisationGrid
	{
		unsigned int DivisionCount = 0u;
		QuantisationGridSource Source = QuantisationGridSource::Default;

		bool IsValid(unsigned long intervalLengthSamps) const noexcept
		{
			return intervalLengthSamps > 0ul && DivisionCount > 0u;
		}

		// Direct evaluation avoids cumulative rounding drift. Ties round upward.
		unsigned long SampleAt(unsigned int index, unsigned long intervalLengthSamps) const noexcept
		{
			if (!IsValid(intervalLengthSamps)) return 0ul;
			if (index >= DivisionCount) return intervalLengthSamps;
			const auto numerator = static_cast<std::uint64_t>(index) * intervalLengthSamps;
			return static_cast<unsigned long>((numerator + DivisionCount / 2u) / DivisionCount);
		}
	};

	struct RemoteTransportGeometry
	{
		unsigned long IntervalLengthSamps = 0ul;
		unsigned int BPI = 0u;
		unsigned int PhaseSamps = 0u;
		float BPM = 0.0f;
		std::uint64_t Generation = 0u;
	};

	struct QuantisationParams
	{
		unsigned int SeedSamps = 0u;
		unsigned int MasterSamps = 0u;
	};

	struct QuantisationLoopTakeVisual
	{
		unsigned long LoopLengthSamps = 0ul;
		std::uint32_t GrainSamps = 0u;
		std::uint32_t LoopGrains = 0u;
		double LoopIndexFrac = 0.0;
		float YCenter = 0.0f;
		float HalfHeight = 0.0f;
		float Radius = 0.0f;
		midi::MidiQuantisationFraction Fraction;
		std::int32_t PhaseOffsetSamps = 0;
	};

	struct QuantisationPolicy
	{
		unsigned int SeedGrainMinMs = constants::DefaultSeedGrainMinMs;
		unsigned int SeedGrainTargetMaxMs = constants::DefaultSeedGrainTargetMaxMs;
		unsigned int SeedBpmMin = constants::DefaultSeedBpmMin;
		bool SeedUsesPowers = true;
	};

	struct QuantisationTiming
	{
		unsigned int SeedSamps = 0u;
		unsigned int MasterLoopSamps = 0u;
		unsigned int SeedCount = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
	};
}
