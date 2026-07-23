#pragma once

#include <cstdint>
#include <limits>

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
		std::uint64_t LocalBlockStartSample = 0u;
		std::uint64_t AudioBlockStartSample = 0u;
	};

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
			&& bpm >= minBpm && bpm <= maxBpm
			&& bpi >= minBpi && bpi <= maxBpi;
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
		std::uint64_t observationAudioSample,
		std::uint64_t boundaryAudioSample) noexcept
	{
		if (newIntervalLengthSamps == 0u)
			return {};

		const auto observedPhase = observedRemotePhaseSamps % newIntervalLengthSamps;
		const auto elapsed = observationAudioSample != 0u && boundaryAudioSample >= observationAudioSample
			? (boundaryAudioSample - observationAudioSample) % newIntervalLengthSamps
			: 0u;
		const auto remotePhase = static_cast<unsigned int>(
			(static_cast<std::uint64_t>(observedPhase) + elapsed) % newIntervalLengthSamps);
		if (oldMasterLengthSamps == 0ul)
			return { remotePhase, 0 };

		const auto oldMasterLength = static_cast<unsigned int>(oldMasterLengthSamps);
		return { remotePhase, SignedCircularDifference(oldMasterPhaseSamps,
			remotePhase % oldMasterLength, oldMasterLength) };
	}

	inline unsigned int IntervalSampsFromTempo(float bpm,
		unsigned int bpi,
		unsigned int sampleRate) noexcept
	{
		if (bpm <= 0.0f || bpi == 0u || sampleRate == 0u)
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
		std::uint64_t observationSequence,
		std::uint64_t localBlockStartSample,
		std::uint64_t audioBlockStartSample) noexcept
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
		timing.IntervalPositionSamps = ScaleSampleRate(
			remote.IntervalPositionSamps % remote.IntervalLengthSamps,
			remote.SourceSampleRate,
			deviceSampleRate) % timing.IntervalLengthSamps;
		timing.DeviceSampleRate = deviceSampleRate;
		timing.SourceSampleRate = remote.SourceSampleRate;
		timing.Bpm = remote.Bpm;
		timing.Bpi = remote.Bpi;
		timing.Generation = generation;
		timing.RemoteWrapCount = remoteWrapCount;
		timing.ObservationSequence = observationSequence;
		timing.LocalBlockStartSample = localBlockStartSample;
		timing.AudioBlockStartSample = audioBlockStartSample;
		return timing;
	}
}