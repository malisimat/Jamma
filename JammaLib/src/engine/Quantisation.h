#pragma once

#include <cstdint>
#include <optional>
#include "../include/Constants.h"

namespace engine
{
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
		unsigned int BeatsPerSeed = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
	};

	unsigned int MinSeedSamps(unsigned int sampleRate, const QuantisationPolicy& policy);
	unsigned int IntervalSampsFromTempo(float bpm, unsigned int bpi, unsigned int sampleRate);
	std::optional<QuantisationTiming> DeduceSeedTiming(unsigned long masterLoopSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& policy);
	std::optional<QuantisationTiming> DeduceTapSeedTiming(unsigned long requestedSeedSamps,
		unsigned long masterLoopSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& policy);

	class TapTempoTracker
	{
	public:
		void Clear() noexcept;
		std::optional<QuantisationTiming> TapAtSample(std::uint64_t samplePosition,
			unsigned int sampleRate,
			unsigned long masterLoopSamps,
			const QuantisationPolicy& policy);
		std::optional<QuantisationTiming> CurrentTiming(unsigned long masterLoopSamps,
			unsigned int sampleRate,
			const QuantisationPolicy& policy) const;
		bool HasEstimate() const noexcept;

	private:
		std::optional<std::uint64_t> _lastTapSample;
		std::optional<double> _estimatedGapSamps;
	};
}