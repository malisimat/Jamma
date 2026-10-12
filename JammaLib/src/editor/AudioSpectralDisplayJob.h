#pragma once

#include <atomic>
#include <memory>
#include <stop_token>
#include <string>
#include "../audio/AudioSpectralEditState.h"
#include "../graphics/AudioSpectralModel.h"

namespace editor
{
	// Immutable request, one worker-owned result. No GL calls occur in Run;
	// UI/render readers acquire Complete before receiving const display data.
	class AudioSpectralDisplayJob
	{
	public:
		enum class State { Pending, Complete, Failed, Cancelled };
		AudioSpectralDisplayJob(std::shared_ptr<const audio::AudioSpectralAnalysis> analysis,
			std::shared_ptr<const audio::AudioSpectralEditState> editState,
			unsigned int timeSegments, unsigned int frequencySegments);
		const std::shared_ptr<const audio::AudioSpectralAnalysis> Analysis;
		const std::shared_ptr<const audio::AudioSpectralEditState> EditState;
		const unsigned int TimeSegments;
		const unsigned int FrequencySegments;
		State Status() const noexcept { return _state.load(std::memory_order_acquire); }
		void Cancel() noexcept; // Off audio; never waits for the worker.
		std::shared_ptr<const graphics::AudioSpectralDisplay> Result() const noexcept;
		std::string Reason() const;
		void Run(); // Dedicated worker only, exactly once per request.

	private:
		static_assert(std::atomic<State>::is_always_lock_free);
		std::atomic<State> _state{ State::Pending };
		// BuildDisplay polls this token; no stop callbacks may be registered.
		std::stop_source _stop;
		std::shared_ptr<const graphics::AudioSpectralDisplay> _result;
		std::string _reason;
	};
}
