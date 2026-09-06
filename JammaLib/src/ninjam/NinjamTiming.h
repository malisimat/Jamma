#pragma once

#include <cstdint>
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
		bool HasDeviceAudioSampleAtObservation = false;
		std::uint64_t DeviceAudioSampleAtObservation = 0u;
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
		bool HasDeviceAudioSampleAtObservation = false;
		std::uint64_t LocalMasterAbsoluteSampleAtObservation = 0u;
		std::uint64_t DeviceAudioSampleAtObservation = 0u;
		std::uint64_t ObservationAgeSamps = 0u;
	};

	bool IsValidNinjamTempo(float bpm,
		std::int64_t bpi,
		float minBpm = 20.0f,
		float maxBpm = 400.0f,
		unsigned int minBpi = 1u,
		unsigned int maxBpi = 32u) noexcept;

	// Shared validity boundary for remote timing. Used for both the live NJClient
	// query and the periodic snapshot so a placeholder tempo the snapshot rejects
	// cannot slip through the live path (§2.9). Bounds default to the plausible
	// NINJAM tempo range (mirrors constants::*PlausibleNinjam*); callers may pass
	// tighter values. Kept self-contained so the test project (which does not add
	// JammaLib\include to its search path) can include this header freely.
	bool IsValidRemoteTiming(unsigned int intervalLengthSamps,
		unsigned int sourceSampleRate,
		float bpm,
		unsigned int bpi,
		float minBpm = 20.0f,
		float maxBpm = 400.0f,
		unsigned int minBpi = 1u,
		unsigned int maxBpi = 32u) noexcept;

	unsigned int ScaleSampleRate(unsigned int samples,
		unsigned int sourceSampleRate,
		unsigned int targetSampleRate) noexcept;

	unsigned int ScaleWrappedSampleRate(unsigned int samples,
		unsigned int sourceLengthSamps,
		unsigned int sourceSampleRate,
		unsigned int targetSampleRate,
		unsigned int targetLengthSamps) noexcept;

	long long SignedCircularDifference(unsigned int currentOffset,
		unsigned int targetOffset,
		unsigned int intervalLength) noexcept;

	struct NinjamBoundaryTimingReplacement
	{
		unsigned int RemoteMasterPhaseSamps = 0u;
		long long RemoteMasterPhaseCorrectionSamps = 0;
	};

	NinjamBoundaryTimingReplacement ResolveBoundaryTimingReplacement(
		unsigned long oldMasterLengthSamps,
		unsigned int oldMasterPhaseSamps,
		unsigned int newRemoteMasterIntervalLengthSamps,
		unsigned int observedRemoteMasterPhaseSamps,
		const std::optional<std::uint64_t>& remotePhaseDeviceSample,
		std::uint64_t boundaryDeviceAudioSample) noexcept;

	unsigned int IntervalSampsFromTempo(float bpm,
		unsigned int bpi,
		unsigned int sampleRate) noexcept;

	NinjamTiming ToDeviceTiming(const NinjamRemoteTiming& remote,
		bool isConnected,
		unsigned int deviceSampleRate,
		std::uint64_t generation,
		unsigned long remoteWrapCount,
		std::uint64_t observationSequence) noexcept;

	NinjamTiming ToDeviceTiming(const NinjamRemoteTiming& remote,
		bool isConnected,
		unsigned int deviceSampleRate,
		std::uint64_t generation,
		unsigned long remoteWrapCount,
		std::uint64_t observationSequence,
		std::uint64_t,
		std::uint64_t) noexcept;

	NinjamTiming ProjectTimingToAudioSample(NinjamTiming timing,
		std::uint64_t deviceAudioSampleAtObservation) noexcept;
}
