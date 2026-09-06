#pragma once

#include <cstdint>
#include <optional>
#include "NinjamTiming.h"

namespace ninjam
{
	struct NinjamTimingObservation
	{
		unsigned int IntervalLengthSamps = 0u;
		unsigned int IntervalPositionSamps = 0u;
		std::uint64_t LocalMasterAbsoluteSampleAtObservation = 0u;
	};

	enum class NinjamTimingEventType
	{
		GenerationChanged,
		Wrap,
		Join
	};

	struct NinjamTimingEvent
	{
		NinjamTimingEventType Type = NinjamTimingEventType::GenerationChanged;
		std::uint64_t Generation = 0u;
		unsigned long RemoteWrapCount = 0ul;
		unsigned int IntervalLengthSamps = 0u;
		unsigned int RemotePositionSamps = 0u;
		long long RemoteMasterPhaseCorrectionSamps = 0;
	};

	struct NinjamTimingTrackerDiagnostics
	{
		std::uint64_t ObservationsAccepted = 0u;
		std::uint64_t ObservationsRejected = 0u;
		std::uint64_t DuplicateObservations = 0u;
		std::uint64_t GenerationChanges = 0u;
		std::uint64_t WrapEvents = 0u;
		std::uint64_t JoinEvents = 0u;
	};

	class NinjamTimingTracker
	{
	public:
		void Connect() noexcept;
		void Disconnect() noexcept;
		bool IsConnected() const noexcept { return _connected; }
		std::optional<NinjamTimingEvent> Observe(const NinjamTimingObservation& observation);
		void BeginJoinAlignment(unsigned long localMasterPositionSamps) noexcept;
		std::uint64_t Generation() const noexcept { return _generation; }
		unsigned long RemoteWrapCount() const noexcept { return _remoteWrapCount; }
		NinjamTimingTrackerDiagnostics Diagnostics() const noexcept { return _diagnostics; }

	private:
		bool _connected = false;
		bool _hasLastPosition = false;
		unsigned int _lastPositionSamps = 0u;
		unsigned int _intervalLengthSamps = 0u;
		std::uint64_t _generation = 0u;
		unsigned long _remoteWrapCount = 0ul;
		bool _joinPending = false;
		long long _remoteMasterPhaseCorrectionSamps = 0;
		std::uint64_t _joinGeneration = 0u;
		NinjamTimingTrackerDiagnostics _diagnostics;
	};
}
