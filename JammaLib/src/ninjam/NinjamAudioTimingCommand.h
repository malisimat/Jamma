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
		void Publish(const NinjamDesiredTransportState& desired) noexcept;
		std::optional<NinjamDesiredTransportState> ReadLatest() const noexcept;

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

}
