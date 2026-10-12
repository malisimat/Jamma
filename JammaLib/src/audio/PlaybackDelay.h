#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "../base/PublishedAudioSnapshot.h"

namespace audio
{
	// The job owner publishes delay processors. Only the audio reader mutates
	// sample history; a descriptor owns at most the current and previous processor.
	class PlaybackDelay
	{
	public:
		static constexpr unsigned int MaxDelaySamps = 262144u;
		static constexpr unsigned int TransitionSamps = 64u;

		bool CanConfigure(unsigned int delaySamps) const
		{
			if (delaySamps > MaxDelaySamps) return false;
			const auto state = _state.load();
			return !state || !state->Previous
				|| state->Current->DelaySamps == delaySamps
				|| state->TransitionPosition.load(std::memory_order_acquire) >= TransitionSamps;
		}

		bool Configure(unsigned int delaySamps)
		{
			if (delaySamps > MaxDelaySamps) return false;
			_state.collect(); // Reclaim retired processors on the job owner.
			const auto former = _state.load();
			if (former && former->Previous
				&& former->TransitionPosition.load(std::memory_order_acquire) < TransitionSamps)
				return former->Current->DelaySamps == delaySamps; // Retry a changed target on the next job tick.
			if (former && former->Current->DelaySamps == delaySamps)
			{
				if (former->Previous)
				{
					auto settled = std::make_shared<Descriptor>();
					settled->Current = former->Current;
					settled->TransitionPosition.store(TransitionSamps, std::memory_order_relaxed);
					_state.store(std::move(settled));
				}
				return true;
			}
			auto next = std::make_shared<Descriptor>();
			next->Current = std::make_shared<Processor>(delaySamps);
			if (former) next->Previous = former->Current;
			else next->TransitionPosition.store(TransitionSamps, std::memory_order_relaxed);
			_state.store(std::move(next));
			return true;
		}

		void Process(float* samples, unsigned int count) noexcept
		{
			if (!samples) return;
			auto descriptor = _state.borrow();
			if (!descriptor) return;
			auto position = descriptor->TransitionPosition.load(std::memory_order_relaxed);
			for (auto sample = 0u; sample < count; ++sample)
			{
				const auto input = samples[sample];
				const auto delayed = descriptor->Current->Read(input);
				if (position < TransitionSamps && descriptor->Previous)
				{
					const auto former = descriptor->Previous->Read(input);
					const auto blend = static_cast<float>(++position)
						/ static_cast<float>(TransitionSamps);
					samples[sample] = former * (1.0f - blend) + delayed * blend;
					descriptor->TransitionPosition.store(position, std::memory_order_release);
				}
				else
					samples[sample] = delayed;
			}
		}

		// Callback-only source cancellation. The history becomes invalid without
		// scanning a potentially long ring or changing recorded source coordinates.
		void Clear() noexcept
		{
			auto descriptor = _state.borrow();
			if (!descriptor) return;
			descriptor->Current->ValidSamples = 0u;
			if (descriptor->Previous) descriptor->Previous->ValidSamples = 0u;
			descriptor->TransitionPosition.store(TransitionSamps, std::memory_order_release);
		}

		unsigned int ConfiguredDelaySamps() const
		{
			const auto descriptor = _state.load();
			return descriptor ? descriptor->Current->DelaySamps : 0u;
		}

		std::size_t PublishedProcessorCount() const
		{
			const auto descriptor = _state.load();
			return descriptor ? 1u + (descriptor->Previous ? 1u : 0u) : 0u;
		}

	private:
		struct Processor
		{
			explicit Processor(unsigned int delay) : DelaySamps(delay),
				Ring(delay == 0u ? 0u : static_cast<std::size_t>(delay) + 1u, 0.0f) {}

			float Read(float input) noexcept
			{
				if (DelaySamps == 0u) return input;
				Ring[WriteIndex] = input;
				auto read = WriteIndex + 1u;
				if (read == Ring.size()) read = 0u;
				if (ValidSamples <= DelaySamps) ++ValidSamples;
				const auto output = ValidSamples > DelaySamps ? Ring[read] : 0.0f;
				WriteIndex = read;
				return output;
			}

			const unsigned int DelaySamps;
			std::vector<float> Ring;
			std::size_t WriteIndex = 0u; // Audio callback only.
			unsigned int ValidSamples = 0u; // Clear without scanning the ring.
		};

		struct Descriptor
		{
			std::shared_ptr<Processor> Current;
			std::shared_ptr<Processor> Previous;
			std::atomic<unsigned int> TransitionPosition{ 0u };
		};

		base::PublishedAudioSnapshot<Descriptor> _state;
	};
}
