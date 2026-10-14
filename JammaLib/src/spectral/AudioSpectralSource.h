#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "../audio/BufferBank.h"
#include "AudioSpectralConfig.h"
#include "AudioSpectralResourceBudget.h"

namespace spectral
{
	struct AudioSpectralSourceLayout
	{
		unsigned long LogicalLength = 0ul;
		unsigned long PhysicalLength = 0ul; // Excludes the private capture prefix.
		unsigned int FadeSamps = 0u;
		float SampleRate = 0.0f;

		static constexpr unsigned long BodyOffset = constants::MaxLoopFadeSamps;
		bool IsValid() const noexcept;
		unsigned long StoredSampleCount() const noexcept { return BodyOffset + PhysicalLength; }
		// Prefix samples have negative body time; tail samples wrap by logical length.
		unsigned long WrappedBodyIndex(unsigned long storageIndex) const noexcept;
		bool operator==(const AudioSpectralSourceLayout&) const = default;
	};

	// A per-Loop lifetime token prevents source reuse across different Loop owners.
	struct AudioSpectralSourceIdentity {};

	// Once a capture completes, only const ownership escapes to analysis/export.
	// Samples retain the exact audio::BufferBank layout: prefix, logical body, physical tail.
	struct AudioSpectralSource
	{
		std::shared_ptr<const AudioSpectralSourceIdentity> Identity;
		std::uint64_t Generation;
		AudioSpectralSourceLayout Layout;
		// Declared before PCM so final destruction frees PCM before releasing admission.
        std::shared_ptr<const AudioSpectralResourceBudget::Reservation> Resources;
        std::vector<float> Samples;
        // Chosen by UI/request owner before publication; immutable for worker/UI readers.
        // Source pins the transform and reservation until all jobs/history release it.
        AudioSpectralTransformConfig SelectedTransform;

		std::span<const float> Body() const noexcept
		{
			return std::span<const float>(Samples).subspan(Layout.BodyOffset, Layout.LogicalLength);
		}
	};

	class AudioSpectralSourceCapture
	{
	public:
		enum class State { Preparing, Pending, Complete, Cancelled, Failed };
		// Initial tuning budget, not a measured callback timing guarantee.
		static constexpr unsigned long CopySamplesPerBlock = AudioSpectralConfig::SourceCopySamplesPerBlock;
		static constexpr std::uint64_t MaxSourceBytes = AudioSpectralConfig::MaxSourceBytes;

		// Construct the small request off callback; Prepare() allocates its PCM on
		// the dedicated worker. Source() is off callback; Cancel() is bounded and
		// safe on either the UI or callback owner.
		AudioSpectralSourceCapture(AudioSpectralSourceLayout layout, std::uint64_t generation,
			std::shared_ptr<const AudioSpectralSourceIdentity> identity,
            std::shared_ptr<const AudioSpectralResourceBudget::Reservation> resources = nullptr,
            AudioSpectralTransformConfig transform = {});
		void Prepare(); // Dedicated worker only; called once per request.
		const AudioSpectralSourceLayout& Layout() const noexcept { return _source->Layout; }
		const AudioSpectralTransformConfig& SelectedTransform() const noexcept { return _source->SelectedTransform; }
		State Status() const noexcept { return _state.load(std::memory_order_acquire); }
		void Cancel() noexcept;
		std::shared_ptr<const AudioSpectralSource> Source() const noexcept;

		// Only the Loop's audio-boundary hook may call this. The request's topology
		// never changes; callback-only progress/PCM become immutable before Complete.
		void CopyAtAudioBoundary(const audio::BufferBank& buffer,
			const AudioSpectralSourceLayout& layout, std::uint64_t generation,
			bool eligible) noexcept;

	private:
		static_assert(std::atomic<State>::is_always_lock_free);
		std::atomic<State> _state{ State::Preparing };
		std::shared_ptr<AudioSpectralSource> _source;
		unsigned long _copied = 0ul; // Audio reader only; never inspected by the UI.
	};
}
