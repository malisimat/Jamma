#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <iosfwd>
#include "NinjamTimingCoordinator.h"

namespace ninjam
{
	struct NinjamPlaybackTakeDiagnostic
	{
		std::string Station;
		std::string Take;
		unsigned long MidiPhaseSamps = 0ul;
		unsigned long MidiLengthSamps = 0ul;
		std::int32_t MidiAnchorCorrectionSamps = 0;
		unsigned long RecordedSamps = 0ul;
		unsigned long RestoreBeforeSamps = 0ul;
		unsigned long RestoreTargetSamps = 0ul;
		std::uint64_t RestoreCount = 0u;
		std::uint64_t RestoreChangedCount = 0u;
		unsigned int LargestRestoreBackstepSamps = 0u;
	};

	// Job-thread presentation only; diagnostic capture and timing authority stay
	// with NinjamTimingCoordinator. The caller applies the verbose-event gate.
	class NinjamTimingDiagnosticPresenter
	{
	public:
		void Present(const NinjamTimingDiagnostics& diagnostics, std::ostream& output);
		bool PlaybackTraceDue(std::chrono::steady_clock::time_point now) noexcept;
		void PresentPlayback(const NinjamTiming& timing, const NinjamDesiredTimingReceipt& receipt,
			std::span<const NinjamPlaybackTakeDiagnostic> takes, std::ostream& output);

	private:
		std::optional<std::chrono::steady_clock::time_point> _lastPlaybackTraceAt;
		std::optional<NinjamTiming> _lastPlaybackTiming;
		std::uint64_t _lastPlaybackEpoch = 0u;
		std::uint64_t _lastPresentedSequence = 0u;
		std::uint64_t _lastPresentedOverflowCount = 0u;
	};
}
