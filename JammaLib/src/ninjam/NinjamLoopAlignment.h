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

	// Maps elapsed device-rate remote time into the ruler used by the local loop
	// buffers. Keep this integer formulation shared by correction and restore so
	// a remote interval boundary always maps to an exact local boundary.
	constexpr std::uint64_t MapRemoteElapsedToLocal(std::uint64_t elapsed,
		std::uint64_t localMasterLength, std::uint64_t remoteMasterLength) noexcept
	{
		if (localMasterLength == 0u || remoteMasterLength == 0u)
			return 0u;
		const auto intervals = elapsed / remoteMasterLength;
		const auto remainder = elapsed % remoteMasterLength;
		return intervals * localMasterLength
			+ (remainder * localMasterLength + (remoteMasterLength / 2u)) / remoteMasterLength;
	}

	// The local-source phase that will reach zero exactly when the remote phase
	// reaches its next interval boundary. Deriving it from the remaining elapsed
	// time avoids an independent rounding disagreement.
	constexpr std::uint64_t SourcePhaseAtRemotePhase(std::uint64_t remotePhase,
		std::uint64_t localMasterLength, std::uint64_t remoteMasterLength) noexcept
	{
		if (localMasterLength == 0u || remoteMasterLength == 0u)
			return 0u;
		const auto phase = remotePhase % remoteMasterLength;
		const auto remaining = (remoteMasterLength - phase) % remoteMasterLength;
		const auto elapsed = MapRemoteElapsedToLocal(remaining, localMasterLength, remoteMasterLength)
			% localMasterLength;
		return (localMasterLength - elapsed) % localMasterLength;
	}

	// Audio-thread-owned geometry for one remote-to-local ruler map. Keeping the
	// origin with the geometry makes subsequent rounded rebases exact.
	struct SyncPhaseMap
	{
		unsigned long SourceLengthSamps = 0ul;
		unsigned long RemoteLengthSamps = 0ul;
		std::uint64_t SceneOriginSamps = 0u;
		unsigned long SourcePhaseAtOrigin = 0ul;

		constexpr bool IsActive() const noexcept
		{
			return SourceLengthSamps > 0ul && RemoteLengthSamps > 0ul;
		}

		constexpr unsigned long SourcePhaseAt(std::uint64_t sceneCoordinateSamps) const noexcept
		{
			if (!IsActive() || sceneCoordinateSamps < SceneOriginSamps)
				return 0ul;
			const auto elapsed = MapRemoteElapsedToLocal(sceneCoordinateSamps - SceneOriginSamps,
				SourceLengthSamps, RemoteLengthSamps);
			return static_cast<unsigned long>((SourcePhaseAtOrigin + elapsed) % SourceLengthSamps);
		}

		constexpr void Rebase(std::uint64_t sceneCoordinateSamps, unsigned long sourcePhase) noexcept
		{
			SceneOriginSamps = sceneCoordinateSamps;
			SourcePhaseAtOrigin = SourceLengthSamps == 0ul ? 0ul : sourcePhase % SourceLengthSamps;
		}
	};

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
