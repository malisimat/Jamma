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
		void Publish(const NinjamTiming& timing) noexcept;
		std::optional<NinjamTiming> ReadLatest() const noexcept;

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
		std::atomic_bool _hasLocalTransport{ false };
		std::atomic<std::uint64_t> _localMasterLengthSamps{ 0u };
		std::atomic<std::uint64_t> _localMasterPhaseSamps{ 0u };
		std::atomic<std::uint64_t> _localLoopCount{ 0u };
		std::atomic<std::uint64_t> _localAbsoluteSamplePos{ 0u };
		std::atomic<std::uint64_t> _localSceneSamplePos{ 0u };
		std::atomic_bool _hasDeviceAudioSampleAtObservation{ false };
		std::atomic<std::uint64_t> _localMasterAbsoluteSampleAtObservation{ 0u };
		std::atomic<std::uint64_t> _deviceAudioSampleAtObservation{ 0u };
		std::atomic<std::uint64_t> _observationAgeSamps{ 0u };
	};
}
