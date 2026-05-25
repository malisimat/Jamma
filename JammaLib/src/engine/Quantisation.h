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
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
	};

	// Returns the minimum seed length in samples enforced by the policy grain floor.
	unsigned int MinSeedSamps(unsigned int sampleRate, const QuantisationPolicy& policy);

	// Converts a Ninjam-style BPM/BPI tempo to the corresponding interval length in samples.
	unsigned int IntervalSampsFromTempo(float bpm, unsigned int bpi, unsigned int sampleRate);

	// Computes full timing given an already-known seed and master length.
	// Use when a seed length has already been selected and only BPM/BPI metadata
	// needs to be derived.
	std::optional<QuantisationTiming> TimingFromSeedAndMaster(unsigned int seedSamps,
		unsigned long masterSamps,
		unsigned int sampleRate);

	// Deduces the seed by halving the master loop until it falls within the
	// policy's target grain range and represents the BPM directly.  BPI is the
	// resulting number of seeds in the master loop.
	std::optional<QuantisationTiming> DeduceSeedTiming(unsigned long masterLoopSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& policy);

	// Converts a tap-derived beat gap into a seed while respecting the policy
	// seed-size floor. Use when tap tempo is active but no master loop exists yet.
	std::optional<QuantisationTiming> DeduceTapSeedTiming(unsigned long requestedSeedSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& policy);

	// Snaps a tap-derived gap to the nearest whole divisor of masterLoopSamps
	// (N * seed == masterLoopSamps) without enforcing any seed-size limits.
	// Use when the master loop is already established and a tap tempo is being
	// applied to quantise subsequent loop lengths against it.
	std::optional<QuantisationTiming> DeduceTapSeedTimingFromMaster(unsigned long tapGapSamps,
		unsigned long masterLoopSamps,
		unsigned int sampleRate);

	// Accumulates tap events and maintains a running average beat-gap estimate.
	class TapTempoTracker
	{
	public:
		// Discards all tap history; call before starting a new tap-tempo session.
		void Clear() noexcept;

		// Records a tap at the given sample position and returns updated timing if
		// at least two taps have been received.  Call on each tap button press.
		std::optional<QuantisationTiming> TapAtSample(std::uint64_t samplePosition,
			unsigned int sampleRate,
			unsigned long masterLoopSamps,
			const QuantisationPolicy& policy);

		// Returns the current timing estimate without advancing the tracker. With a
		// master loop, the estimate snaps to a clean master divisor; without one,
		// it is treated as a standalone tap-derived seed.
		std::optional<QuantisationTiming> CurrentTiming(unsigned long masterLoopSamps,
			unsigned int sampleRate,
			const QuantisationPolicy& policy) const;

		// Returns true once at least two taps have been received and a gap estimate exists.
		bool HasEstimate() const noexcept;

	private:
		std::optional<std::uint64_t> _lastTapSample;
		std::optional<double> _estimatedGapSamps;
	};
}