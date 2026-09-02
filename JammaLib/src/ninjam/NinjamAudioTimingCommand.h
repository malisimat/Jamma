#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "../utils/Timer.h"

namespace ninjam
{
	enum class NinjamLocalFollowPolicy : std::uint8_t
	{
		ContinuousSync,
		BlockSync,
		NoSync
	};

	enum class NinjamDesiredTimingIntent : std::uint8_t
	{
		NoSync,
		Replacement,
		JoinAlignment,
		PhaseDiscipline
	};

	// Complete, latest-substitutable remote transport authority. Every active
	// publication carries the full device-rate geometry and timestamped remote
	// phase; NoSync carries the epoch/version but no remote authority.
	struct NinjamDesiredTransportState
	{
		std::uint64_t Version = 0u;
		std::uint64_t SessionEpoch = 0u;
		std::uint64_t Generation = 0u;
		NinjamDesiredTimingIntent Intent = NinjamDesiredTimingIntent::NoSync;
		NinjamLocalFollowPolicy LocalFollowPolicy = NinjamLocalFollowPolicy::NoSync;
		bool HasRemoteTiming = false;
		unsigned long IntervalLengthSamps = 0ul;
		unsigned int GrainSamps = 0u;
		unsigned int BeatsPerInterval = 0u;
		float TempoBpm = 0.0f;
		utils::Timer::QuantisationType Quantisation = utils::Timer::QUANTISE_OFF;
		unsigned int RemotePhaseSamps = 0u;
		bool HasObservationSample = false;
		std::uint64_t ObservationSample = 0u;
	};

	// The integration owner is the sole writer and the audio callback is the sole
	// reader. AudioHost compares the complete latest value with its applied value.
	class NinjamDesiredTransportStateMailbox
	{
	public:
		void Publish(const NinjamDesiredTransportState& desired) noexcept
		{
			const auto writingSequence = _sequence.fetch_add(1u, std::memory_order_acq_rel) + 1u;
			_version.store(desired.Version, std::memory_order_relaxed);
			_sessionEpoch.store(desired.SessionEpoch, std::memory_order_relaxed);
			_generation.store(desired.Generation, std::memory_order_relaxed);
			_intent.store(desired.Intent, std::memory_order_relaxed);
			_localFollowPolicy.store(desired.LocalFollowPolicy, std::memory_order_relaxed);
			_hasRemoteTiming.store(desired.HasRemoteTiming, std::memory_order_relaxed);
			_intervalLengthSamps.store(desired.IntervalLengthSamps, std::memory_order_relaxed);
			_grainSamps.store(desired.GrainSamps, std::memory_order_relaxed);
			_beatsPerInterval.store(desired.BeatsPerInterval, std::memory_order_relaxed);
			_tempoBpm.store(desired.TempoBpm, std::memory_order_relaxed);
			_quantisation.store(desired.Quantisation, std::memory_order_relaxed);
			_remotePhaseSamps.store(desired.RemotePhaseSamps, std::memory_order_relaxed);
			_hasObservationSample.store(desired.HasObservationSample, std::memory_order_relaxed);
			_observationSample.store(desired.ObservationSample, std::memory_order_relaxed);
			_sequence.store(writingSequence + 1u, std::memory_order_release);
			_hasPublication.store(true, std::memory_order_release);
		}

		std::optional<NinjamDesiredTransportState> ReadLatest() const noexcept
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
				desired.GrainSamps = _grainSamps.load(std::memory_order_relaxed);
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

	private:
		static constexpr unsigned int _MaxReadAttempts = 4u;
		mutable std::atomic<std::uint64_t> _sequence{ 0u };
		std::atomic_bool _hasPublication{ false };
		std::atomic<std::uint64_t> _version{ 0u };
		std::atomic<std::uint64_t> _sessionEpoch{ 0u };
		std::atomic<std::uint64_t> _generation{ 0u };
		std::atomic<NinjamDesiredTimingIntent> _intent{ NinjamDesiredTimingIntent::NoSync };
		std::atomic<NinjamLocalFollowPolicy> _localFollowPolicy{ NinjamLocalFollowPolicy::NoSync };
		std::atomic_bool _hasRemoteTiming{ false };
		std::atomic<unsigned long> _intervalLengthSamps{ 0ul };
		std::atomic<unsigned int> _grainSamps{ 0u };
		std::atomic<unsigned int> _beatsPerInterval{ 0u };
		std::atomic<float> _tempoBpm{ 0.0f };
		std::atomic<utils::Timer::QuantisationType> _quantisation{ utils::Timer::QUANTISE_OFF };
		std::atomic<unsigned int> _remotePhaseSamps{ 0u };
		std::atomic_bool _hasObservationSample{ false };
		std::atomic<std::uint64_t> _observationSample{ 0u };
	};

	// Single-writer (UI thread) / single-reader (audio thread) latest-value
	// mailbox for the local-only normalized transport offset. It deliberately
	// coalesces drag events: the audio thread needs only the newest absolute
	// target, including an explicit zero publication.
	class LocalTransportOffsetLoopFracMailbox
	{
	public:
		void Publish(double normalizedLoopFrac) noexcept
		{
			const auto writingSequence = _sequence.fetch_add(1u, std::memory_order_acq_rel) + 1u;
			_normalizedLoopFrac.store(normalizedLoopFrac, std::memory_order_relaxed);
			_sequence.store(writingSequence + 1u, std::memory_order_release);
			_hasPublication.store(true, std::memory_order_release);
		}

		std::optional<double> ConsumeLatest() noexcept
		{
			if (!_hasPublication.load(std::memory_order_acquire))
				return std::nullopt;

			for (unsigned int attempt = 0u; attempt < _MaxReadAttempts; ++attempt)
			{
				const auto before = _sequence.load(std::memory_order_acquire);
				if ((before & 1u) != 0u)
					continue;
				if (before == _consumedSequence)
					return std::nullopt;

				const auto normalizedLoopFrac = _normalizedLoopFrac.load(std::memory_order_relaxed);
				const auto after = _sequence.load(std::memory_order_acquire);
				if (before == after)
				{
					_consumedSequence = before;
					return normalizedLoopFrac;
				}
			}

			return std::nullopt;
		}

	private:
		static constexpr unsigned int _MaxReadAttempts = 4u;
		std::atomic<std::uint64_t> _sequence{ 0u };
		std::atomic_bool _hasPublication{ false };
		std::atomic<double> _normalizedLoopFrac{ 0.0 };
		std::uint64_t _consumedSequence = 0u;
	};
}
