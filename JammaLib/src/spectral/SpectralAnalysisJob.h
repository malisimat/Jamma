#pragma once

#include <atomic>
#include <memory>
#include <stop_token>
#include <string>
#include "SpectralDsp.h"
#include "SpectralEditState.h"
#include "SpectralPlaybackSnapshot.h"

namespace spectral
{
	// Metadata is immutable after construction. A single worker writes Result/
	// Reason before a release terminal-state publication; UI readers acquire it.
	// No callback uses this job, stop source, shared ownership or strings.
	class SpectralAnalysisJob
	{
	public:
		enum class State { Pending, Complete, Failed, Cancelled };
		SpectralAnalysisJob(std::shared_ptr<const SpectralSource> source,
			SpectralTransformConfig transform);
		const std::shared_ptr<const SpectralSource> Source;
		const SpectralTransformConfig Transform;
		State Status() const noexcept { return _state.load(std::memory_order_acquire); }
		void Cancel() noexcept; // Off audio; no waits for worker completion.
		std::shared_ptr<const SpectralAnalysis> Result() const noexcept;
		std::string Reason() const;
		std::shared_ptr<const SpectralEditState> EditState() const noexcept;
		std::shared_ptr<const SpectralPlaybackSnapshot> Playback() const noexcept;
		void Run(); // Dedicated worker only; exactly once per queued job.

	private:
		static_assert(std::atomic<State>::is_always_lock_free);
		std::atomic<State> _state{ State::Pending };
		// DSP only polls the token; never register stop callbacks on this source.
		std::stop_source _stop;
		std::shared_ptr<const SpectralAnalysis> _result;
		std::shared_ptr<const SpectralPlaybackSnapshot> _playback;
		std::shared_ptr<const SpectralEditState> _editState;
		std::string _reason;
	};
}
