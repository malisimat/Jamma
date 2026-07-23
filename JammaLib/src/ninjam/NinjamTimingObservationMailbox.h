#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "NinjamTiming.h"

namespace ninjam
{
	// Single audio-writer/job-reader handoff. An even sequence brackets a
	// complete observation; an odd sequence means the writer is in progress.
	class NinjamTimingObservationMailbox
	{
	public:
		void Publish(const NinjamTiming& timing) noexcept
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
			_localBlockStartSample.store(timing.LocalBlockStartSample, std::memory_order_relaxed);
			_audioBlockStartSample.store(timing.AudioBlockStartSample, std::memory_order_relaxed);
			_sequence.store(sequence + 2u, std::memory_order_release);
			_hasPublication.store(true, std::memory_order_release);
		}

		std::optional<NinjamTiming> ReadLatest() const noexcept
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
				timing.LocalBlockStartSample = _localBlockStartSample.load(std::memory_order_relaxed);
				timing.AudioBlockStartSample = _audioBlockStartSample.load(std::memory_order_relaxed);

				const auto after = _sequence.load(std::memory_order_acquire);
				if (before == after)
					return timing;
			}

			return std::nullopt;
		}

	private:
		static constexpr unsigned int _MaxReadAttempts = 3u;
		std::atomic<std::uint64_t> _sequence{ 0u };
		std::atomic_bool _hasPublication{ false };
		std::atomic_bool _isConnected{ false };
		std::atomic_bool _isValid{ false };
		std::atomic<unsigned int> _intervalLengthSamps{ 0u };
		std::atomic<unsigned int> _intervalPositionSamps{ 0u };
		std::atomic<unsigned int> _deviceSampleRate{ 0u };
		std::atomic<unsigned int> _sourceSampleRate{ 0u };
		std::atomic<float> _bpm{ 0.0f };
		std::atomic<unsigned int> _bpi{ 0u };
		std::atomic<std::uint64_t> _generation{ 0u };
		std::atomic<unsigned long> _remoteWrapCount{ 0ul };
		std::atomic<std::uint64_t> _observationSequence{ 0u };
		std::atomic<std::uint64_t> _localBlockStartSample{ 0u };
		std::atomic<std::uint64_t> _audioBlockStartSample{ 0u };
	};
}