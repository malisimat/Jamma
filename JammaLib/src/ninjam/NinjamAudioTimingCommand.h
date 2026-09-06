#pragma once

// Complete latest-value contract from job-side timing authority to AudioHost;
// the mailbox transports coherent state but does not become a second owner.

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
		unsigned long RemoteMasterIntervalLengthSamps = 0ul;
		unsigned int RemoteGridStepSamps = 0u;
		unsigned int BeatsPerInterval = 0u;
		float TempoBpm = 0.0f;
		utils::Timer::QuantisationType Quantisation = utils::Timer::QUANTISE_OFF;
		unsigned int RemoteMasterPhaseSamps = 0u;
		bool HasRemotePhaseDeviceSample = false;
		std::uint64_t RemotePhaseDeviceSample = 0u;
	};

	// Coherent audio-boundary acknowledgement of the latest complete desired
	// transport value. AudioHost publishes this existing receipt without logging;
	// the job owner may correlate it with desired state off the callback.
	struct NinjamDesiredTimingReceipt
	{
		std::uint64_t Version = 0u;
		std::uint64_t SessionEpoch = 0u;
		std::uint64_t Generation = 0u;
		NinjamDesiredTimingIntent Intent = NinjamDesiredTimingIntent::NoSync;
		NinjamLocalFollowPolicy Policy = NinjamLocalFollowPolicy::NoSync;
		std::uint64_t SceneCoordinateSamps = 0u;
		long long LocalSourceCorrectionSamps = 0;
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
		std::atomic<unsigned long> _remoteMasterIntervalLengthSamps{ 0ul };
		std::atomic<unsigned int> _remoteGridStepSamps{ 0u };
		std::atomic<unsigned int> _beatsPerInterval{ 0u };
		std::atomic<float> _tempoBpm{ 0.0f };
		std::atomic<utils::Timer::QuantisationType> _quantisation{ utils::Timer::QUANTISE_OFF };
		std::atomic<unsigned int> _remoteMasterPhaseSamps{ 0u };
		std::atomic_bool _hasRemotePhaseDeviceSample{ false };
		std::atomic<std::uint64_t> _remotePhaseDeviceSample{ 0u };
	};

}
