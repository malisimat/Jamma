#include "Quantisation.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace engine;

namespace
{
	constexpr double kTapTimeoutSecs = 2.5;

	unsigned int ClampToUInt(unsigned long value)
	{
		return value > std::numeric_limits<unsigned int>::max() ?
			std::numeric_limits<unsigned int>::max() :
			static_cast<unsigned int>(value);
	}

	unsigned int RoundedToUInt(double value)
	{
		if (value <= 0.0)
			return 0u;

		if (value >= static_cast<double>(std::numeric_limits<unsigned int>::max()))
			return std::numeric_limits<unsigned int>::max();

		return static_cast<unsigned int>(value + 0.5);
	}

	unsigned int SnapSeedToMaster(unsigned long requestedSeedSamps,
		unsigned long masterLoopSamps,
		unsigned int minSeedSamps)
	{
		if (masterLoopSamps == 0ul)
			return ClampToUInt(std::max<unsigned long>(requestedSeedSamps, minSeedSamps));

		if (requestedSeedSamps == 0ul)
			requestedSeedSamps = masterLoopSamps;

		// O(sqrt(N)): each divisor pair yields a large seed (low bpi) and small seed (high bpi).
		unsigned long bestSeed = 0ul;
		unsigned long bestDistance = std::numeric_limits<unsigned long>::max();

		for (unsigned long d = 1ul; d * d <= masterLoopSamps; ++d)
		{
			if ((masterLoopSamps % d) != 0ul)
				continue;

			const unsigned long candidates[2] = { masterLoopSamps / d, d };
			for (auto seed : candidates)
			{
				if (seed < static_cast<unsigned long>(minSeedSamps))
					continue;

				const auto distance = seed > requestedSeedSamps ?
					seed - requestedSeedSamps :
					requestedSeedSamps - seed;

				if (distance < bestDistance)
				{
					bestDistance = distance;
					bestSeed = seed;
				}
			}
		}

		if (bestSeed == 0ul)
			bestSeed = static_cast<unsigned long>(minSeedSamps);

		return ClampToUInt(bestSeed);
	}

	std::optional<QuantisationTiming> TimingFromSeed(unsigned int seedSamps,
		unsigned long masterLoopSamps,
		unsigned int sampleRate)
	{
		if ((seedSamps == 0u) || (sampleRate == 0u))
			return std::nullopt;

		if (masterLoopSamps == 0ul)
			masterLoopSamps = seedSamps;

		const auto seedCount = std::max(1ul, masterLoopSamps / seedSamps);
		if (seedCount > std::numeric_limits<unsigned int>::max())
			return std::nullopt;

		QuantisationTiming timing;
		timing.SeedSamps = seedSamps;
		timing.MasterLoopSamps = ClampToUInt(masterLoopSamps);
		timing.SeedCount = static_cast<unsigned int>(seedCount);
		timing.Bpm = (60.0f * static_cast<float>(sampleRate)) / static_cast<float>(seedSamps);
		timing.Bpi = timing.SeedCount;
		return timing;
	}
}

unsigned int engine::MinSeedSamps(unsigned int sampleRate, const QuantisationPolicy& policy)
{
	if (sampleRate == 0u)
		return 0u;

	const auto minMs = std::max(1u, policy.SeedGrainMinMs);
	return RoundedToUInt((static_cast<double>(sampleRate) * static_cast<double>(minMs)) / 1000.0);
}

unsigned int engine::IntervalSampsFromTempo(float bpm, unsigned int bpi, unsigned int sampleRate)
{
	if ((bpm <= 0.0f) || (bpi == 0u) || (sampleRate == 0u))
		return 0u;

	return RoundedToUInt((static_cast<double>(sampleRate) * 60.0 * static_cast<double>(bpi)) / static_cast<double>(bpm));
}

std::optional<QuantisationTiming> engine::TimingFromSeedAndMaster(unsigned int seedSamps,
	unsigned long masterSamps,
	unsigned int sampleRate,
	const QuantisationPolicy&)
{
	if ((seedSamps == 0u) || (masterSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	return TimingFromSeed(seedSamps, masterSamps, sampleRate);
}

std::optional<QuantisationTiming> engine::DeduceSeedTiming(unsigned long masterLoopSamps,
	unsigned int sampleRate,
	const QuantisationPolicy& policy)
{
	if ((masterLoopSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	auto seedSamps = masterLoopSamps;
	auto minSeed = static_cast<unsigned long>(MinSeedSamps(sampleRate, policy));
	if (minSeed == 0ul)
		return std::nullopt;

	const auto targetMaxMs = std::max(std::max(1u, policy.SeedGrainMinMs), policy.SeedGrainTargetMaxMs);
	const auto targetMaxSeed = static_cast<unsigned long>(std::max(static_cast<double>(minSeed),
		(static_cast<double>(sampleRate) * static_cast<double>(targetMaxMs)) / 1000.0));

	while ((seedSamps >= targetMaxSeed) && ((seedSamps / 2ul) >= minSeed))
		seedSamps /= 2ul;

	const auto minBpm = static_cast<float>(std::max(1u, policy.SeedBpmMin));
	while (((60.0f * static_cast<float>(sampleRate)) / static_cast<float>(seedSamps)) < minBpm
		&& ((seedSamps / 2ul) >= minSeed))
	{
		seedSamps /= 2ul;
	}

	if (seedSamps > std::numeric_limits<unsigned int>::max())
		return std::nullopt;

	return TimingFromSeed(static_cast<unsigned int>(seedSamps), masterLoopSamps, sampleRate);
}

std::optional<QuantisationTiming> engine::DeduceTapSeedTiming(unsigned long requestedSeedSamps,
	unsigned long masterLoopSamps,
	unsigned int sampleRate,
	const QuantisationPolicy& policy)
{
	if ((requestedSeedSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	const auto minSeed = MinSeedSamps(sampleRate, policy);
	if (minSeed == 0u)
		return std::nullopt;

	const auto seedSamps = SnapSeedToMaster(requestedSeedSamps, masterLoopSamps, minSeed);
	const auto timingMaster = masterLoopSamps == 0ul ? seedSamps : masterLoopSamps;
	return TimingFromSeed(seedSamps, timingMaster, sampleRate);
}

void TapTempoTracker::Clear() noexcept
{
	_lastTapSample.reset();
	_estimatedGapSamps.reset();
}

std::optional<QuantisationTiming> TapTempoTracker::TapAtSample(std::uint64_t samplePosition,
	unsigned int sampleRate,
	unsigned long masterLoopSamps,
	const QuantisationPolicy& policy)
{
	if (!_lastTapSample.has_value())
	{
		_lastTapSample = samplePosition;
		return std::nullopt;
	}

	if (samplePosition <= _lastTapSample.value())
	{
		std::cout << "[tap] rejected: non-increasing\n";
		_lastTapSample = samplePosition;
		return std::nullopt;
	}

	const auto gap = static_cast<double>(samplePosition - _lastTapSample.value());

	// Reset if the inter-tap gap exceeds the timeout; treat this tap as a fresh first tap.
	if (sampleRate > 0u && gap > kTapTimeoutSecs * static_cast<double>(sampleRate))
	{
		Clear();
		_lastTapSample = samplePosition;
		return std::nullopt;
	}

	_lastTapSample = samplePosition;
	_estimatedGapSamps = _estimatedGapSamps.has_value() ?
		((_estimatedGapSamps.value() * 0.5) + (gap * 0.5)) :
		gap;

	if (sampleRate > 0u)
	{
		const auto msPerSamp = 1000.0 / static_cast<double>(sampleRate);
		std::cout << "[tap] raw=" << static_cast<int>(gap * msPerSamp + 0.5)
			<< "ms smooth=" << static_cast<int>(_estimatedGapSamps.value() * msPerSamp + 0.5)
			<< "ms\n";
	}

	return CurrentTiming(masterLoopSamps, sampleRate, policy);
}

std::optional<QuantisationTiming> TapTempoTracker::CurrentTiming(unsigned long masterLoopSamps,
	unsigned int sampleRate,
	const QuantisationPolicy& policy) const
{
	if (!_estimatedGapSamps.has_value())
		return std::nullopt;

	return DeduceTapSeedTiming(static_cast<unsigned long>(_estimatedGapSamps.value() + 0.5),
		masterLoopSamps,
		sampleRate,
		policy);
}

bool TapTempoTracker::HasEstimate() const noexcept
{
	return _estimatedGapSamps.has_value();
}

std::optional<QuantisationTiming> engine::DeduceTapSeedTimingFromMaster(unsigned long tapGapSamps,
	unsigned long masterLoopSamps,
	unsigned int sampleRate,
	const QuantisationPolicy& policy)
{
	if ((tapGapSamps == 0ul) || (masterLoopSamps == 0ul) || (sampleRate == 0u))
		return std::nullopt;

	// Nearest divisor of masterLoopSamps to tapGapSamps; O(sqrt(N)), no min-size floor.
	unsigned long bestSeed = 0ul;
	unsigned long bestDist = std::numeric_limits<unsigned long>::max();

	for (unsigned long d = 1ul; d * d <= masterLoopSamps; ++d)
	{
		if ((masterLoopSamps % d) != 0ul)
			continue;

		const unsigned long seedLarge = masterLoopSamps / d;
		const unsigned long seedSmall = d;

		const auto distLarge = seedLarge > tapGapSamps
			? seedLarge - tapGapSamps
			: tapGapSamps - seedLarge;
		if (distLarge < bestDist)
		{
			bestDist = distLarge;
			bestSeed = seedLarge;
		}

		if (seedSmall != seedLarge)
		{
			const auto distSmall = seedSmall > tapGapSamps
				? seedSmall - tapGapSamps
				: tapGapSamps - seedSmall;
			if (distSmall < bestDist)
			{
				bestDist = distSmall;
				bestSeed = seedSmall;
			}
		}
	}

	if ((bestSeed == 0ul) || (bestSeed > std::numeric_limits<unsigned int>::max()))
		return std::nullopt;

	return TimingFromSeed(static_cast<unsigned int>(bestSeed), masterLoopSamps, sampleRate);
}