#include "stdafx.h"
#include "NinjamAudioTimingCommand.h"

namespace ninjam
{
	void NinjamDesiredTransportStateMailbox::Publish(
		const NinjamDesiredTransportState& desired) noexcept
	{
		const auto writingSequence = _sequence.fetch_add(1u, std::memory_order_acq_rel) + 1u;
		_version.store(desired.Version, std::memory_order_relaxed);
		_sessionEpoch.store(desired.SessionEpoch, std::memory_order_relaxed);
		_generation.store(desired.Generation, std::memory_order_relaxed);
		_intent.store(desired.Intent, std::memory_order_relaxed);
		_localFollowPolicy.store(desired.LocalFollowPolicy, std::memory_order_relaxed);
		_hasRemoteTiming.store(desired.HasRemoteTiming, std::memory_order_relaxed);
		_remoteMasterIntervalLengthSamps.store(desired.RemoteMasterIntervalLengthSamps, std::memory_order_relaxed);
		_remoteGridStepSamps.store(desired.RemoteGridStepSamps, std::memory_order_relaxed);
		_beatsPerInterval.store(desired.BeatsPerInterval, std::memory_order_relaxed);
		_tempoBpm.store(desired.TempoBpm, std::memory_order_relaxed);
		_quantisation.store(desired.Quantisation, std::memory_order_relaxed);
		_remoteMasterPhaseSamps.store(desired.RemoteMasterPhaseSamps, std::memory_order_relaxed);
		_hasRemotePhaseDeviceSample.store(desired.HasRemotePhaseDeviceSample, std::memory_order_relaxed);
		_remotePhaseDeviceSample.store(desired.RemotePhaseDeviceSample, std::memory_order_relaxed);
		_sequence.store(writingSequence + 1u, std::memory_order_release);
		_hasPublication.store(true, std::memory_order_release);
	}

	std::optional<NinjamDesiredTransportState>
		NinjamDesiredTransportStateMailbox::ReadLatest() const noexcept
	{
		if (!_hasPublication.load(std::memory_order_acquire))
			return std::nullopt;

		for (unsigned int attempt = 0u; attempt < _MaxReadAttempts; ++attempt)
		{
			const auto before = _sequence.load(std::memory_order_acquire);
			if ((before & 1u) != 0u)
				continue;
			NinjamDesiredTransportState desired;
			desired.Version = _version.load(std::memory_order_relaxed);
			desired.SessionEpoch = _sessionEpoch.load(std::memory_order_relaxed);
			desired.Generation = _generation.load(std::memory_order_relaxed);
			desired.Intent = _intent.load(std::memory_order_relaxed);
			desired.LocalFollowPolicy = _localFollowPolicy.load(std::memory_order_relaxed);
			desired.HasRemoteTiming = _hasRemoteTiming.load(std::memory_order_relaxed);
			desired.RemoteMasterIntervalLengthSamps = _remoteMasterIntervalLengthSamps.load(std::memory_order_relaxed);
			desired.RemoteGridStepSamps = _remoteGridStepSamps.load(std::memory_order_relaxed);
			desired.BeatsPerInterval = _beatsPerInterval.load(std::memory_order_relaxed);
			desired.TempoBpm = _tempoBpm.load(std::memory_order_relaxed);
			desired.Quantisation = _quantisation.load(std::memory_order_relaxed);
			desired.RemoteMasterPhaseSamps = _remoteMasterPhaseSamps.load(std::memory_order_relaxed);
			desired.HasRemotePhaseDeviceSample = _hasRemotePhaseDeviceSample.load(std::memory_order_relaxed);
			desired.RemotePhaseDeviceSample = _remotePhaseDeviceSample.load(std::memory_order_relaxed);
			const auto after = _sequence.load(std::memory_order_acquire);
			if (before == after)
				return desired;
		}

		return std::nullopt;
	}
}
