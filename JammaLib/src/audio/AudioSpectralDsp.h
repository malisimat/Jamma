#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>
#include "AudioSpectralConfig.h"
#include "AudioSpectralFft.h"
#include "AudioSpectralSource.h"

namespace audio
{
	class AudioSpectralGainField;
	class AudioSpectralEnvelope;

	struct AudioSpectralGeometry
	{
		// Frame centres are a hop-aligned lattice in body-source samples, including
		// frames outside the body needed to reconstruct prefix/seam contributors.
		std::int64_t FirstFrameCentre = 0;
		std::size_t FrameCount = 0u;
		unsigned int BinCount = 0u;
		unsigned long TargetFirstStorage = 0ul;
		unsigned long TargetLength = 0ul;
		std::int64_t FrameCentre(std::size_t frame, unsigned int hop) const noexcept
		{
			return FirstFrameCentre + static_cast<std::int64_t>(frame) * hop;
		}
	};

	struct AudioSpectralMemoryEstimate
	{
		AudioSpectralGeometry Geometry;
		std::uint64_t WorkingBytes = 0u;
	};

	// Original complex coefficients remain immutable across all edit revisions.
	struct AudioSpectralAnalysis
	{
		std::shared_ptr<const AudioSpectralSource> Source;
		AudioSpectralTransformConfig Transform;
		AudioSpectralGeometry Geometry;
		float InverseFftNormalization = 0.0f;
		double WindowSum = 0.0;
		std::vector<float> Window;
		std::vector<AudioSpectralFft::Bin> Coefficients;
		std::span<const AudioSpectralFft::Bin> Frame(std::size_t index) const noexcept;
		double Magnitude(std::size_t frame, unsigned int bin) const noexcept;
	};

	struct AudioSpectralPcm
	{
		std::shared_ptr<const AudioSpectralSource> Source;
		// Empty means exact original PCM, with no STFT arithmetic or float copying.
		std::shared_ptr<const std::vector<float>> ProcessedSamples;
		// Non-null only for ownership recovery: synthesis already includes this layer.
		std::shared_ptr<const AudioSpectralEnvelope> ComposedEnvelope;
		bool EnvelopeComposed = false;
		std::uint64_t ComposedSpectralRevision = 0u, ComposedEnvelopeRevision = 0u;
		bool UsesOriginal() const noexcept { return !ProcessedSamples; }
		std::span<const float> Samples() const noexcept
		{
			return ProcessedSamples ? std::span<const float>(*ProcessedSamples)
				: std::span<const float>(Source->Samples);
		}
	};

	class AudioSpectralDsp
	{
	public:
		static std::optional<AudioSpectralMemoryEstimate> EstimateWorkingMemory(
			const AudioSpectralSourceLayout& layout, const AudioSpectralTransformConfig& transform,
			std::string& reason);
		// Off-callback admission: preferred transform first, then legacy resolution.
		// A successful reservation is pinned by the source through worker/history teardown.
		static bool SelectTransform(const AudioSpectralSourceLayout& layout,
			const std::shared_ptr<AudioSpectralResourceBudget>& budget,
			AudioSpectralTransformConfig& transform,
			std::shared_ptr<const AudioSpectralResourceBudget::Reservation>& resources,
			std::string& reason);
		// Worker-only allocation/FFT/synthesis. Cancellation is checked per frame
		// and bounded sample batches; it is for obsolete sources/teardown, not every paint update.
		static std::shared_ptr<const AudioSpectralAnalysis> Analyze(
			std::shared_ptr<const AudioSpectralSource> source,
			const AudioSpectralTransformConfig& transform, std::string& reason,
			std::stop_token stop = {});
		static std::shared_ptr<const AudioSpectralPcm> Reconstruct(
			const std::shared_ptr<const AudioSpectralAnalysis>& analysis,
			const AudioSpectralGainField& gains, std::string& reason,
			std::stop_token stop = {}, bool forceUnityRoundTrip = false,
			std::shared_ptr<const AudioSpectralEnvelope> envelope = nullptr);

	private:
		static float _SourceSample(const AudioSpectralSource& source, std::int64_t bodyIndex) noexcept;
	};
}
