#pragma once

#include <atomic>
#include <memory>
#include <stop_token>
#include <string>
#include "SpectralEditState.h"
#include "../graphics/SpectralModel.h"

namespace spectral
{
	// Immutable request, one worker-owned result. No GL calls occur in Run;
	// UI/render readers acquire Complete before receiving const display data.
	class SpectralDisplayJob
	{
	public:
		enum class State { Pending, Complete, Failed, Cancelled };
        class Cache
        {
            friend class SpectralDisplayJob;
            std::shared_ptr<const graphics::SpectralDisplay> Display; // Visual-worker only; builder provenance.
        };
        struct Damage
        {
            std::uint64_t BaselineRevision = 0, TargetRevision = 0;
            std::size_t FirstFrame = static_cast<std::size_t>(-1), LastFrame = 0;
            unsigned int FirstBin = static_cast<unsigned int>(-1), LastBin = 0;
            unsigned long FirstSample = static_cast<unsigned long>(-1), LastSample = 0;
        };
		SpectralDisplayJob(std::shared_ptr<const SpectralAnalysis> analysis,
			std::shared_ptr<const SpectralEditState> editState,
			unsigned int timeSegments, unsigned int frequencySegments,
			graphics::SpectralViewport viewport = {}, std::uint64_t presentationRevision = 0u,
            std::shared_ptr<Cache> cache = nullptr, Damage damage = {});
		const std::shared_ptr<const SpectralAnalysis> Analysis;
		const std::shared_ptr<const SpectralEditState> EditState;
		const unsigned int TimeSegments;
		const unsigned int FrequencySegments;
		const graphics::SpectralViewport Viewport;
		const std::uint64_t PresentationRevision;
		State Status() const noexcept { return _state.load(std::memory_order_acquire); }
		void Cancel() noexcept; // Off audio; never waits for the worker.
		std::shared_ptr<const graphics::SpectralDisplay> Result() const noexcept;
		std::string Reason() const;
		void Run(); // Dedicated worker only, exactly once per request.

	private:
		static_assert(std::atomic<State>::is_always_lock_free);
		std::atomic<State> _state{ State::Pending };
		// BuildDisplay polls this token; no stop callbacks may be registered.
		std::stop_source _stop;
		std::shared_ptr<const graphics::SpectralDisplay> _result;
		std::string _reason;
        std::shared_ptr<Cache> _cache;
        Damage _damage;
	};
}
