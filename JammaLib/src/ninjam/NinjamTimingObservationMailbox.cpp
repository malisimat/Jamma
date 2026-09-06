#include "stdafx.h"
#include "NinjamTimingObservationMailbox.h"

namespace ninjam
{
	void NinjamTimingObservationMailbox::Publish(const NinjamTiming& timing) noexcept
	{
		const auto sequence = _sequence.load(std::memory_order_relaxed);
		_sequence.store(sequence + 1u, std::memory_order_release);
		_isConnected.store(timing.IsConnected, std::memory_order_relaxed);
		_isValid.store(timing.IsValid, std::memory_order_relaxed);
		_intervalLengthSamps.store(timing.IntervalLengthSamps, std::memory_order_relaxed);
		_intervalPositionSamps.store(timing.IntervalPositionSamps, std::memory_order_relaxed);
		_deviceSampleRate.store(timing.DeviceSampleRate, std::memory_order_relaxed);
		_sourceSampleRate.store(timing.SourceSampleRate, std::memory_order_relaxed);
		_bpm.store(timing.Bpm, std::memory_order_relaxed);
		_bpi.store(timing.Bpi, std::memory_order_relaxed);
		_generation.store(timing.Generation, std::memory_order_relaxed);
		_remoteWrapCount.store(timing.RemoteWrapCount, std::memory_order_relaxed);
		_observationSequence.store(timing.ObservationSequence, std::memory_order_relaxed);
		_hasLocalTransport.store(timing.HasLocalTransport, std::memory_order_relaxed);
		_localMasterLengthSamps.store(timing.LocalTransport.MasterLengthSamps, std::memory_order_relaxed);
		_localMasterPhaseSamps.store(timing.LocalTransport.MasterPhaseSamps, std::memory_order_relaxed);
		_localLoopCount.store(timing.LocalTransport.LoopCount, std::memory_order_relaxed);
		_localAbsoluteSamplePos.store(timing.LocalTransport.AbsoluteSamplePos, std::memory_order_relaxed);
		_localSceneSamplePos.store(timing.LocalTransport.SceneSamplePos, std::memory_order_relaxed);
		_hasDeviceAudioSampleAtObservation.store(timing.HasDeviceAudioSampleAtObservation, std::memory_order_relaxed);
		_localMasterAbsoluteSampleAtObservation.store(timing.LocalMasterAbsoluteSampleAtObservation, std::memory_order_relaxed);
		_deviceAudioSampleAtObservation.store(timing.DeviceAudioSampleAtObservation, std::memory_order_relaxed);
		_observationAgeSamps.store(timing.ObservationAgeSamps, std::memory_order_relaxed);
		_sequence.store(sequence + 2u, std::memory_order_release);
		_hasPublication.store(true, std::memory_order_release);
	}

	std::optional<NinjamTiming> NinjamTimingObservationMailbox::ReadLatest() const noexcept
	{
		if (!_hasPublication.load(std::memory_order_acquire))
			return std::nullopt;

		for (unsigned int attempt = 0u; attempt < _MaxReadAttempts; ++attempt)
		{
			const auto before = _sequence.load(std::memory_order_acquire);
			if ((before & 1u) != 0u)
				continue;

			NinjamTiming timing;
			timing.IsConnected = _isConnected.load(std::memory_order_relaxed);
			timing.IsValid = _isValid.load(std::memory_order_relaxed);
			timing.IntervalLengthSamps = _intervalLengthSamps.load(std::memory_order_relaxed);
			timing.IntervalPositionSamps = _intervalPositionSamps.load(std::memory_order_relaxed);
			timing.DeviceSampleRate = _deviceSampleRate.load(std::memory_order_relaxed);
			timing.SourceSampleRate = _sourceSampleRate.load(std::memory_order_relaxed);
			timing.Bpm = _bpm.load(std::memory_order_relaxed);
			timing.Bpi = _bpi.load(std::memory_order_relaxed);
			timing.Generation = _generation.load(std::memory_order_relaxed);
			timing.RemoteWrapCount = _remoteWrapCount.load(std::memory_order_relaxed);
			timing.ObservationSequence = _observationSequence.load(std::memory_order_relaxed);
			timing.HasLocalTransport = _hasLocalTransport.load(std::memory_order_relaxed);
			timing.LocalTransport.MasterLengthSamps = _localMasterLengthSamps.load(std::memory_order_relaxed);
			timing.LocalTransport.MasterPhaseSamps = _localMasterPhaseSamps.load(std::memory_order_relaxed);
			timing.LocalTransport.LoopCount = _localLoopCount.load(std::memory_order_relaxed);
			timing.LocalTransport.AbsoluteSamplePos = _localAbsoluteSamplePos.load(std::memory_order_relaxed);
			timing.LocalTransport.SceneSamplePos = _localSceneSamplePos.load(std::memory_order_relaxed);
			timing.HasDeviceAudioSampleAtObservation = _hasDeviceAudioSampleAtObservation.load(std::memory_order_relaxed);
			timing.LocalMasterAbsoluteSampleAtObservation = _localMasterAbsoluteSampleAtObservation.load(std::memory_order_relaxed);
			timing.DeviceAudioSampleAtObservation = _deviceAudioSampleAtObservation.load(std::memory_order_relaxed);
			timing.ObservationAgeSamps = _observationAgeSamps.load(std::memory_order_relaxed);

			const auto after = _sequence.load(std::memory_order_acquire);
			if (before == after)
				return timing;
		}

		return std::nullopt;
	}
}
