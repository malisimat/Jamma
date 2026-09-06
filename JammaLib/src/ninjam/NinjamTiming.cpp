#include "stdafx.h"
#include "NinjamTiming.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ninjam
{
	bool IsValidNinjamTempo(float bpm, std::int64_t bpi, float minBpm,
		float maxBpm, unsigned int minBpi, unsigned int maxBpi) noexcept
	{
		return std::isfinite(bpm)
			&& bpm >= minBpm && bpm <= maxBpm
			&& bpi >= static_cast<std::int64_t>(minBpi)
			&& bpi <= static_cast<std::int64_t>(maxBpi);
	}

	bool IsValidRemoteTiming(unsigned int intervalLengthSamps,
		unsigned int sourceSampleRate, float bpm, unsigned int bpi,
		float minBpm, float maxBpm, unsigned int minBpi, unsigned int maxBpi) noexcept
	{
		return intervalLengthSamps > 0u
			&& sourceSampleRate > 0u
			&& IsValidNinjamTempo(bpm, bpi, minBpm, maxBpm, minBpi, maxBpi);
	}

	unsigned int ScaleSampleRate(unsigned int samples, unsigned int sourceSampleRate,
		unsigned int targetSampleRate) noexcept
	{
		if (sourceSampleRate == 0u || targetSampleRate == 0u
			|| sourceSampleRate == targetSampleRate)
			return samples;

		const auto scaled = (static_cast<std::uint64_t>(samples) * targetSampleRate
			+ (sourceSampleRate / 2u)) / sourceSampleRate;
		return scaled > (std::numeric_limits<unsigned int>::max)()
			? (std::numeric_limits<unsigned int>::max)()
			: static_cast<unsigned int>(scaled);
	}

	unsigned int ScaleWrappedSampleRate(unsigned int samples,
		unsigned int sourceLengthSamps, unsigned int sourceSampleRate,
		unsigned int targetSampleRate, unsigned int targetLengthSamps) noexcept
	{
		if (sourceLengthSamps == 0u || targetLengthSamps == 0u)
			return 0u;

		const auto sourcePhase = samples % sourceLengthSamps;
		const auto scaled = ScaleSampleRate(sourcePhase, sourceSampleRate, targetSampleRate);
		return std::min(scaled, targetLengthSamps - 1u);
	}

	long long SignedCircularDifference(unsigned int currentOffset,
		unsigned int targetOffset, unsigned int intervalLength) noexcept
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

	NinjamBoundaryTimingReplacement ResolveBoundaryTimingReplacement(
		unsigned long oldMasterLengthSamps, unsigned int oldMasterPhaseSamps,
		unsigned int newRemoteMasterIntervalLengthSamps, unsigned int observedRemoteMasterPhaseSamps,
		const std::optional<std::uint64_t>& remotePhaseDeviceSample,
		std::uint64_t boundaryDeviceAudioSample) noexcept
	{
		if (newRemoteMasterIntervalLengthSamps == 0u)
			return {};

		const auto observedPhase = observedRemoteMasterPhaseSamps % newRemoteMasterIntervalLengthSamps;
		const auto elapsed = remotePhaseDeviceSample.has_value()
			&& boundaryDeviceAudioSample >= remotePhaseDeviceSample.value()
			? (boundaryDeviceAudioSample - remotePhaseDeviceSample.value()) % newRemoteMasterIntervalLengthSamps
			: 0u;
		const auto remotePhase = static_cast<unsigned int>(
			(static_cast<std::uint64_t>(observedPhase) + elapsed) % newRemoteMasterIntervalLengthSamps);
		if (oldMasterLengthSamps == 0ul)
			return { remotePhase, 0 };

		const auto oldMasterLength = static_cast<unsigned int>(oldMasterLengthSamps);
		const auto baseLocalDelta = SignedCircularDifference(oldMasterPhaseSamps,
			remotePhase % oldMasterLength, oldMasterLength);
		return { remotePhase, baseLocalDelta };
	}

	unsigned int IntervalSampsFromTempo(float bpm, unsigned int bpi,
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

	NinjamTiming ToDeviceTiming(const NinjamRemoteTiming& remote, bool isConnected,
		unsigned int deviceSampleRate, std::uint64_t generation,
		unsigned long remoteWrapCount, std::uint64_t observationSequence) noexcept
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
		timing.HasDeviceAudioSampleAtObservation = remote.HasDeviceAudioSampleAtObservation;
		timing.DeviceAudioSampleAtObservation = remote.DeviceAudioSampleAtObservation;
		return timing;
	}

	NinjamTiming ToDeviceTiming(const NinjamRemoteTiming& remote, bool isConnected,
		unsigned int deviceSampleRate, std::uint64_t generation,
		unsigned long remoteWrapCount, std::uint64_t observationSequence,
		std::uint64_t, std::uint64_t) noexcept
	{
		// Anchor presence belongs to the job-owned remote tuple. The trailing
		// arguments are retained only for the existing snapshot conversion call;
		// numeric zero never means absence.
		return ToDeviceTiming(remote, isConnected, deviceSampleRate,
			generation, remoteWrapCount, observationSequence);
	}

	NinjamTiming ProjectTimingToAudioSample(NinjamTiming timing,
		std::uint64_t deviceAudioSampleAtObservation) noexcept
	{
		if (!timing.HasDeviceAudioSampleAtObservation
			|| timing.IntervalLengthSamps == 0u
			|| deviceAudioSampleAtObservation < timing.DeviceAudioSampleAtObservation)
		{
			timing.HasDeviceAudioSampleAtObservation = false;
			timing.ObservationAgeSamps = 0u;
			return timing;
		}

		const auto observationAge = deviceAudioSampleAtObservation - timing.DeviceAudioSampleAtObservation;
		const auto elapsed = observationAge % timing.IntervalLengthSamps;
		timing.IntervalPositionSamps = static_cast<unsigned int>(
			(static_cast<std::uint64_t>(timing.IntervalPositionSamps) + elapsed)
			% timing.IntervalLengthSamps);
		timing.DeviceAudioSampleAtObservation = deviceAudioSampleAtObservation;
		timing.ObservationAgeSamps = observationAge;
		return timing;
	}
}
