#pragma once

#include <atomic>
#include <memory>
#include <stop_token>
#include <string>
#include "AudioSpectralDsp.h"
#include "AudioSpectralEditState.h"
#include "AudioSpectralPlaybackSnapshot.h"

namespace spectral
{
	// Metadata is immutable after construction. A single worker writes Result/
	// Reason before a release terminal-state publication; UI readers acquire it.
	// No callback uses this job, stop source, shared ownership or strings.
	class AudioSpectralAnalysisJob
	{
	public:
		enum class State { Pending, Complete, Failed, Cancelled };
		AudioSpectralAnalysisJob(std::shared_ptr<const AudioSpectralSource> source,
			AudioSpectralTransformConfig transform);
		const std::shared_ptr<const AudioSpectralSource> Source;
		const AudioSpectralTransformConfig Transform;
		State Status() const noexcept { return _state.load(std::memory_order_acquire); }
		void Cancel() noexcept; // Off audio; no waits for worker completion.
		std::shared_ptr<const AudioSpectralAnalysis> Result() const noexcept;
		std::string Reason() const;
		std::shared_ptr<const AudioSpectralEditState> EditState() const noexcept;
		std::shared_ptr<const AudioSpectralPlaybackSnapshot> Playback() const noexcept;
		void Run(); // Dedicated worker only; exactly once per queued job.

	private:
		static_assert(std::atomic<State>::is_always_lock_free);
		std::atomic<State> _state{ State::Pending };
		// DSP only polls the token; never register stop callbacks on this source.
		std::stop_source _stop;
		std::shared_ptr<const AudioSpectralAnalysis> _result;
		std::shared_ptr<const AudioSpectralPlaybackSnapshot> _playback;
		std::shared_ptr<const AudioSpectralEditState> _editState;
		std::string _reason;
	};
}
