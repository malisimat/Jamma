#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace ninjam
{
	// Remote timing as reported by NJClient. All sample fields use SourceSampleRate.
	struct NinjamRemoteTiming
	{
		bool IsConnected = false;
		unsigned int IntervalLengthSamps = 0u;
		unsigned int IntervalPositionSamps = 0u;
		unsigned int SourceSampleRate = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
		bool IsValid = false;
		bool HasAudioBlockStartSample = false;
		std::uint64_t AudioBlockStartSample = 0u;
	};

	struct NinjamLocalTransportObservation
	{
		std::uint64_t MasterLengthSamps = 0u;
		std::uint64_t MasterPhaseSamps = 0u;
		std::uint64_t LoopCount = 0u;
		std::uint64_t AbsoluteSamplePos = 0u;
		std::uint64_t SceneSamplePos = 0u;
	};

	// Canonical connected timing. Interval fields always use DeviceSampleRate.
	struct NinjamTiming
	{
		bool IsConnected = false;
		bool IsValid = false;
		unsigned int IntervalLengthSamps = 0u;
		unsigned int IntervalPositionSamps = 0u;
		unsigned int DeviceSampleRate = 0u;
		unsigned int SourceSampleRate = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
		std::uint64_t Generation = 0u;
		unsigned long RemoteWrapCount = 0ul;
		std::uint64_t ObservationSequence = 0u;
		bool HasLocalTransport = false;
		NinjamLocalTransportObservation LocalTransport;
		bool HasAudioBlockStartSample = false;
		std::uint64_t LocalBlockStartSample = 0u;
		std::uint64_t AudioBlockStartSample = 0u;
		std::uint64_t ObservationAgeSamps = 0u;
	};

	inline bool IsValidNinjamTempo(float bpm,
		std::int64_t bpi,
		float minBpm = 20.0f,
		float maxBpm = 400.0f,
		unsigned int minBpi = 1u,
		unsigned int maxBpi = 32u) noexcept
	{
		return std::isfinite(bpm)
			&& bpm >= minBpm && bpm <= maxBpm
			&& bpi >= static_cast<std::int64_t>(minBpi)
			&& bpi <= static_cast<std::int64_t>(maxBpi);
	}

	// Shared validity boundary for remote timing. Used for both the live NJClient
	// query and the periodic snapshot so a placeholder tempo the snapshot rejects
	// cannot slip through the live path (§2.9). Bounds default to the plausible
	// NINJAM tempo range (mirrors constants::*PlausibleNinjam*); callers may pass
	// tighter values. Kept self-contained so the test project (which does not add
	// JammaLib\include to its search path) can include this header freely.
	inline bool IsValidRemoteTiming(unsigned int intervalLengthSamps,
		unsigned int sourceSampleRate,
		float bpm,
		unsigned int bpi,
		float minBpm = 20.0f,
		float maxBpm = 400.0f,
		unsigned int minBpi = 1u,
		unsigned int maxBpi = 32u) noexcept
	{
		return intervalLengthSamps > 0u
			&& sourceSampleRate > 0u
			&& IsValidNinjamTempo(bpm, bpi, minBpm, maxBpm, minBpi, maxBpi);
	}

	inline unsigned int ScaleSampleRate(unsigned int samples,
		unsigned int sourceSampleRate,
		unsigned int targetSampleRate) noexcept
	{
		if (sourceSampleRate == 0u || targetSampleRate == 0u || sourceSampleRate == targetSampleRate)
			return samples;

		const auto scaled = (static_cast<std::uint64_t>(samples) * targetSampleRate
			+ (sourceSampleRate / 2u)) / sourceSampleRate;
		return scaled > (std::numeric_limits<unsigned int>::max)()
			? (std::numeric_limits<unsigned int>::max)()
			: static_cast<unsigned int>(scaled);
	}

	inline unsigned int ScaleWrappedSampleRate(unsigned int samples,
		unsigned int sourceLengthSamps,
		unsigned int sourceSampleRate,
		unsigned int targetSampleRate,
		unsigned int targetLengthSamps) noexcept
	{
		if (sourceLengthSamps == 0u || targetLengthSamps == 0u)
			return 0u;

		const auto sourcePhase = samples % sourceLengthSamps;
		const auto scaled = ScaleSampleRate(sourcePhase, sourceSampleRate, targetSampleRate);
		return std::min(scaled, targetLengthSamps - 1u);
	}

	inline long long SignedCircularDifference(unsigned int currentOffset,
		unsigned int targetOffset,
		unsigned int intervalLength) noexcept
	{
		if (intervalLength == 0u)
			return 0;

		const auto current = currentOffset % intervalLength;
		const auto target = targetOffset % intervalLength;
		const auto length = static_cast<long long>(intervalLength);
		auto delta = static_cast<long long>(target) - static_cast<long long>(current);
		if (delta > length / 2)
			delta -= length;
		else if (delta < -(length / 2))
			delta += length;
		else if ((intervalLength % 2u) == 0u && delta == -(length / 2))
			delta = length / 2;
		return delta;
	}

	struct NinjamBoundaryTimingReplacement
	{
		unsigned int RemotePhaseSamps = 0u;
		long long LocalDeltaSamps = 0;
	};

	inline NinjamBoundaryTimingReplacement ResolveBoundaryTimingReplacement(
		unsigned long oldMasterLengthSamps,
		unsigned int oldMasterPhaseSamps,
		unsigned int newIntervalLengthSamps,
		unsigned int observedRemotePhaseSamps,
		const std::optional<std::uint64_t>& observationAudioSample,
		std::uint64_t boundaryAudioSample) noexcept
	{
		if (newIntervalLengthSamps == 0u)
			return {};

		const auto observedPhase = observedRemotePhaseSamps % newIntervalLengthSamps;
		const auto elapsed = observationAudioSample.has_value()
			&& boundaryAudioSample >= observationAudioSample.value()
			? (boundaryAudioSample - observationAudioSample.value()) % newIntervalLengthSamps
			: 0u;
		const auto remotePhase = static_cast<unsigned int>(
			(static_cast<std::uint64_t>(observedPhase) + elapsed) % newIntervalLengthSamps);
		if (oldMasterLengthSamps == 0ul)
			return { remotePhase, 0 };

		const auto oldMasterLength = static_cast<unsigned int>(oldMasterLengthSamps);
		const auto baseLocalDelta = SignedCircularDifference(oldMasterPhaseSamps,
			remotePhase % oldMasterLength, oldMasterLength);
		return { remotePhase, baseLocalDelta };
	}

	inline unsigned int IntervalSampsFromTempo(float bpm,
		unsigned int bpi,
		unsigned int sampleRate) noexcept
	{
		if (!IsValidNinjamTempo(bpm, bpi) || sampleRate == 0u)
			return 0u;

		const auto samples = (static_cast<double>(sampleRate) * 60.0 * static_cast<double>(bpi))
			/ static_cast<double>(bpm);
		if (samples >= static_cast<double>((std::numeric_limits<unsigned int>::max)()))
			return (std::numeric_limits<unsigned int>::max)();
		return static_cast<unsigned int>(samples + 0.5);
	}

	inline NinjamTiming ToDeviceTiming(const NinjamRemoteTiming& remote,
		bool isConnected,
		unsigned int deviceSampleRate,
		std::uint64_t generation,
		unsigned long remoteWrapCount,
		std::uint64_t observationSequence) noexcept
	{
		NinjamTiming timing;
		timing.IsConnected = isConnected;
		if (!isConnected || !remote.IsValid || remote.IntervalLengthSamps == 0u
			|| remote.SourceSampleRate == 0u || deviceSampleRate == 0u)
			return timing;

		timing.IntervalLengthSamps = ScaleSampleRate(remote.IntervalLengthSamps,
			remote.SourceSampleRate, deviceSampleRate);
		if (timing.IntervalLengthSamps == 0u)
			return timing;

		timing.IsValid = true;
		timing.IntervalPositionSamps = ScaleWrappedSampleRate(remote.IntervalPositionSamps,
			remote.IntervalLengthSamps, remote.SourceSampleRate,
			deviceSampleRate, timing.IntervalLengthSamps);
		timing.DeviceSampleRate = deviceSampleRate;
		timing.SourceSampleRate = remote.SourceSampleRate;
		timing.Bpm = remote.Bpm;
		timing.Bpi = remote.Bpi;
		timing.Generation = generation;
		timing.RemoteWrapCount = remoteWrapCount;
		timing.ObservationSequence = observationSequence;
		timing.HasAudioBlockStartSample = remote.HasAudioBlockStartSample;
		timing.AudioBlockStartSample = remote.AudioBlockStartSample;
		return timing;
	}

	inline NinjamTiming ToDeviceTiming(const NinjamRemoteTiming& remote,
		bool isConnected,
		unsigned int deviceSampleRate,
		std::uint64_t generation,
		unsigned long remoteWrapCount,
		std::uint64_t observationSequence,
		std::uint64_t,
		std::uint64_t) noexcept
	{
		// Anchor presence belongs to the job-owned remote tuple. The trailing
		// arguments are retained only for the existing snapshot conversion call;
		// numeric zero never means absence.
		return ToDeviceTiming(remote, isConnected, deviceSampleRate,
			generation, remoteWrapCount, observationSequence);
	}

	inline NinjamTiming ProjectTimingToAudioSample(NinjamTiming timing,
		std::uint64_t audioBlockStartSample) noexcept
	{
		if (!timing.HasAudioBlockStartSample
			|| timing.IntervalLengthSamps == 0u
			|| audioBlockStartSample < timing.AudioBlockStartSample)
		{
			timing.HasAudioBlockStartSample = false;
			timing.ObservationAgeSamps = 0u;
			return timing;
		}

		const auto observationAge = audioBlockStartSample - timing.AudioBlockStartSample;
		const auto elapsed = observationAge % timing.IntervalLengthSamps;
		timing.IntervalPositionSamps = static_cast<unsigned int>(
			(static_cast<std::uint64_t>(timing.IntervalPositionSamps) + elapsed)
			% timing.IntervalLengthSamps);
		timing.AudioBlockStartSample = audioBlockStartSample;
		timing.ObservationAgeSamps = observationAge;
		return timing;
	}
}
