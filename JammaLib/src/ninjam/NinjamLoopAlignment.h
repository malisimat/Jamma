#pragma once

#include <cstdint>

namespace ninjam
{
	constexpr std::uint64_t PositiveModulo(std::int64_t value, std::uint64_t modulus) noexcept
	{
		if (modulus == 0u)
			return 0u;
		const auto signedModulus = static_cast<std::int64_t>(modulus);
		auto normalized = value % signedModulus;
		if (normalized < 0)
			normalized += signedModulus;
		return static_cast<std::uint64_t>(normalized);
	}

	constexpr std::uint64_t CaptureSceneAnchor(std::uint64_t sceneCoordinate,
		std::uint64_t phase, std::uint64_t loopLength) noexcept
	{
		return PositiveModulo(static_cast<std::int64_t>(sceneCoordinate)
			- static_cast<std::int64_t>(phase), loopLength);
	}

	constexpr std::uint64_t RestoreScenePhase(std::uint64_t sceneCoordinate,
		std::uint64_t anchor, std::uint64_t loopLength) noexcept
	{
		return PositiveModulo(static_cast<std::int64_t>(sceneCoordinate)
			- static_cast<std::int64_t>(anchor), loopLength);
	}

	constexpr std::uint64_t RestoreScenePhaseAfterDelta(std::uint64_t sceneCoordinate,
		std::int64_t delta, std::uint64_t anchor, std::uint64_t loopLength) noexcept
	{
		const auto restored = RestoreScenePhase(sceneCoordinate, anchor, loopLength);
		return PositiveModulo(static_cast<std::int64_t>(restored)
			+ (delta % static_cast<std::int64_t>(loopLength)), loopLength);
	}

	constexpr std::uint64_t SceneAlignmentResidual(std::uint64_t phase,
		std::uint64_t sceneCoordinate, std::uint64_t anchor, std::uint64_t loopLength) noexcept
	{
		return PositiveModulo(static_cast<std::int64_t>(phase)
			- static_cast<std::int64_t>(RestoreScenePhase(sceneCoordinate, anchor, loopLength)), loopLength);
	}

	constexpr bool IsRemoteTimingCompatible(std::uint64_t loopLength,
		std::uint64_t grainLength, std::uint64_t intervalLength) noexcept
	{
		return loopLength != 0u && grainLength != 0u && intervalLength != 0u
			&& (loopLength % grainLength) == 0u && (intervalLength % loopLength) == 0u;
	}
}