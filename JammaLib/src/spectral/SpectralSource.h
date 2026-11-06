#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "../audio/BufferBank.h"
#include "SpectralConfig.h"
#include "SpectralResourceBudget.h"

namespace spectral
{
	struct SpectralSourceLayout
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
		bool operator==(const SpectralSourceLayout&) const = default;
	};

	// A per-Loop lifetime token prevents source reuse across different Loop owners.
	struct SpectralSourceIdentity {};

	// Once a capture completes, only const ownership escapes to analysis/export.
	// Samples retain the exact audio::BufferBank layout: prefix, logical body, physical tail.
	struct SpectralSource
	{
		std::shared_ptr<const SpectralSourceIdentity> Identity;
		std::uint64_t Generation;
		SpectralSourceLayout Layout;
		// Declared before PCM so final destruction frees PCM before releasing admission.
        std::shared_ptr<const SpectralResourceBudget::Reservation> Resources;
        std::vector<float> Samples;
        // Chosen by UI/request owner before publication; immutable for worker/UI readers.
        // Source pins the transform and reservation until all jobs/history release it.
        SpectralTransformConfig SelectedTransform;

		std::span<const float> Body() const noexcept
		{
			return std::span<const float>(Samples).subspan(Layout.BodyOffset, Layout.LogicalLength);
		}
	};

	class SpectralSourceCapture
	{
	public:
		enum class State { Preparing, Pending, Complete, Cancelled, Failed };
		// Initial tuning budget, not a measured callback timing guarantee.
		static constexpr unsigned long CopySamplesPerBlock = SpectralConfig::SourceCopySamplesPerBlock;
		static constexpr std::uint64_t MaxSourceBytes = SpectralConfig::MaxSourceBytes;

		// Construct the small request off callback; Prepare() allocates its PCM on
		// the dedicated worker. Source() is off callback; Cancel() is bounded and
		// safe on either the UI or callback owner.
		SpectralSourceCapture(SpectralSourceLayout layout, std::uint64_t generation,
			std::shared_ptr<const SpectralSourceIdentity> identity,
            std::shared_ptr<const SpectralResourceBudget::Reservation> resources = nullptr,
            SpectralTransformConfig transform = {});
		void Prepare(); // Dedicated worker only; called once per request.
		const SpectralSourceLayout& Layout() const noexcept { return _source->Layout; }
		const SpectralTransformConfig& SelectedTransform() const noexcept { return _source->SelectedTransform; }
		State Status() const noexcept { return _state.load(std::memory_order_acquire); }
		void Cancel() noexcept;
		std::shared_ptr<const SpectralSource> Source() const noexcept;

		// Only the Loop's audio-boundary hook may call this. The request's topology
		// never changes; callback-only progress/PCM become immutable before Complete.
		void CopyAtAudioBoundary(const audio::BufferBank& buffer,
			const SpectralSourceLayout& layout, std::uint64_t generation,
			bool eligible) noexcept;

	private:
		static_assert(std::atomic<State>::is_always_lock_free);
		std::atomic<State> _state{ State::Preparing };
		std::shared_ptr<SpectralSource> _source;
		unsigned long _copied = 0ul; // Audio reader only; never inspected by the UI.
	};
}
