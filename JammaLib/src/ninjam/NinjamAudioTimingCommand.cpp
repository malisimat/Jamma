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
		_intervalLengthSamps.store(desired.IntervalLengthSamps, std::memory_order_relaxed);
		_remoteGridStepSamps.store(desired.RemoteGridStepSamps, std::memory_order_relaxed);
		_beatsPerInterval.store(desired.BeatsPerInterval, std::memory_order_relaxed);
		_tempoBpm.store(desired.TempoBpm, std::memory_order_relaxed);
		_quantisation.store(desired.Quantisation, std::memory_order_relaxed);
		_remotePhaseSamps.store(desired.RemotePhaseSamps, std::memory_order_relaxed);
		_hasObservationSample.store(desired.HasObservationSample, std::memory_order_relaxed);
		_observationSample.store(desired.ObservationSample, std::memory_order_relaxed);
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
			desired.IntervalLengthSamps = _intervalLengthSamps.load(std::memory_order_relaxed);
			desired.RemoteGridStepSamps = _remoteGridStepSamps.load(std::memory_order_relaxed);
			desired.BeatsPerInterval = _beatsPerInterval.load(std::memory_order_relaxed);
			desired.TempoBpm = _tempoBpm.load(std::memory_order_relaxed);
			desired.Quantisation = _quantisation.load(std::memory_order_relaxed);
			desired.RemotePhaseSamps = _remotePhaseSamps.load(std::memory_order_relaxed);
			desired.HasObservationSample = _hasObservationSample.load(std::memory_order_relaxed);
			desired.ObservationSample = _observationSample.load(std::memory_order_relaxed);
			const auto after = _sequence.load(std::memory_order_acquire);
			if (before == after)
				return desired;
		}

		return std::nullopt;
	}
}
