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

	// Off-callback formatting of caller-sampled values. Mutable trace summaries are
	// job-owned; diagnostic capture and timing authority stay with the coordinator.
	// The caller applies verbose-event gates and serializes presentation.
	class NinjamTimingDiagnosticPresenter
	{
	public:
		void Present(const NinjamTimingDiagnostics& diagnostics, std::ostream& output);
		static void PresentConnect(std::uint64_t join, std::uint64_t request,
			const NinjamTempoJoinOptions& options, const std::optional<engine::QuantisationTiming>& localTiming,
			std::ostream& output);
		static void PresentUpdate(const NinjamTimingUpdate& update, std::chrono::steady_clock::time_point now,
			const NinjamLocalTransportObservation& clock, unsigned int grain, bool pendingPrompt, std::ostream& output);
		static void PresentTempoState(TempoRequestState state, const NinjamTimingDiagnostics& diagnostics,
			std::uint64_t join, std::uint64_t request, std::ostream& output);
		static void PresentSessionEdge(const NinjamSessionTimingStatus& status, bool hasLocalContent,
			const std::optional<engine::QuantisationTiming>& localTiming, std::ostream& output);
		void ObserveJobTick(std::chrono::steady_clock::time_point now) noexcept;
		void ObservePumpDuration(std::chrono::steady_clock::duration elapsed) noexcept;
		void ObserveJobQueueDepth(std::size_t depth) noexcept { _jobQueueDepth = depth; }
		void PresentJobTrace(bool hasLocalContent, const std::optional<engine::QuantisationTiming>& localTiming,
			TempoRequestState requestState, bool pendingPrompt, const NinjamRemoteTiming* raw, std::ostream& output);
		bool PlaybackTraceDue(std::chrono::steady_clock::time_point now) noexcept;
		void PresentPlayback(const NinjamTiming& timing, const NinjamDesiredTimingReceipt& receipt,
			std::span<const NinjamPlaybackTakeDiagnostic> takes, std::ostream& output);

	private:
		std::optional<std::chrono::steady_clock::time_point> _lastJobAt;
		long long _maxJobGapMs = 0;
		long long _maxPumpUs = 0;
		std::size_t _jobQueueDepth = 0u;
		std::optional<std::chrono::steady_clock::time_point> _lastPlaybackTraceAt;
		std::optional<NinjamTiming> _lastPlaybackTiming;
		std::uint64_t _lastPlaybackEpoch = 0u;
		std::uint64_t _lastPresentedSequence = 0u;
		std::uint64_t _lastPresentedOverflowCount = 0u;
	};
}
