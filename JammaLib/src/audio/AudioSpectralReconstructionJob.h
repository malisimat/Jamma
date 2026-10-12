#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include "AudioSpectralDsp.h"
#include "AudioSpectralGainField.h"

namespace audio
{
	// Immutable request values; one worker produces PCM/reason before release
	// publication. UI readers acquire terminal state; audio never uses this job.
	class AudioSpectralReconstructionJob
	{
	public:
		enum class State { Pending, Complete, Failed, Cancelled };
		AudioSpectralReconstructionJob(std::shared_ptr<const AudioSpectralAnalysis> analysis,
			std::shared_ptr<const AudioSpectralGainField> gains, std::uint64_t spectralRevision);
		const std::shared_ptr<const AudioSpectralAnalysis> Analysis;
		const std::shared_ptr<const AudioSpectralGainField> Gains;
		const std::uint64_t SpectralRevision;
		State Status() const noexcept { return _state.load(std::memory_order_acquire); }
		void Cancel() noexcept; // Off audio; cancellation never joins the worker.
		std::shared_ptr<const AudioSpectralPcm> Result() const noexcept;
		std::string Reason() const;
		void Run(); // Dedicated worker only, once per request.

	private:
		static_assert(std::atomic<State>::is_always_lock_free);
		std::atomic<State> _state{ State::Pending };
		// DSP polls bounded frames/batches; no stop callbacks may be registered.
		std::stop_source _stop;
		std::shared_ptr<const AudioSpectralPcm> _result;
		std::string _reason;
	};
}
