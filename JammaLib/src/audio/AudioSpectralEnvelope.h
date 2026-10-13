#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "AudioSpectralConfig.h"

namespace audio
{
	// Immutable dense source-time gains. Construction and ownership stay off audio;
	// a callback borrows the envelope and performs one bounded indexed read.
	class AudioSpectralEnvelope
	{
	public:
		static std::shared_ptr<const AudioSpectralEnvelope> Unity(unsigned long length, std::string& reason);
		static std::shared_ptr<const AudioSpectralEnvelope> FromGains(std::span<const float> gains, std::string& reason);
		static float MaximumGain() noexcept;
		static float TargetFromDragPixels(double upwardPositiveLogicalPixels) noexcept;
		unsigned long Length() const noexcept { return static_cast<unsigned long>(_gains.size()); }
		float GainAt(unsigned long wrappedIndex) const noexcept { return wrappedIndex < _gains.size() ? _gains[wrappedIndex] : 1.0f; }
		bool IsUnity() const noexcept { return _unity; }
		std::span<const float> Gains() const noexcept { return _gains; }

	private:
		friend class AudioSpectralEnvelopeStroke;
		AudioSpectralEnvelope(std::vector<float> gains, bool unity);
		std::vector<float> _gains;
		bool _unity;
	};

	// Single worker/UI owner. A fresh stroke captures E0 once; its dominance
	// scratch is discarded on release/cancel and is never published to audio.
	class AudioSpectralEnvelopeStroke
	{
	public:
		static std::unique_ptr<AudioSpectralEnvelopeStroke> Begin(
			std::shared_ptr<const AudioSpectralEnvelope> before, float sampleRate, std::string& reason,
			double radiusSeconds = AudioSpectralConfig::BarTimeRadiusSeconds);
		// A stroke uses either anchored relative mode or swept absolute mode below.
		// Relative targets evaluate against E0, preserving silence and replacing prior targets.
		bool SetRelativeBand(double centreSample, float factor) noexcept;
		// Time is explicitly unwrapped. The caller supplies its intended path;
		// only brush support wraps, never an inferred shortest cursor movement.
		bool PaintSegment(double startSample, float startGain, double endSample, float endGain) noexcept;
		std::shared_ptr<const AudioSpectralEnvelope> Snapshot(std::string& reason) const;
		bool HasChanges() const noexcept { return _changedSamples != 0ul; }
		std::shared_ptr<const AudioSpectralEnvelope> Before() const noexcept { return _before; }

		// Snapshot adds finite local ramps at competing-winner boundaries, only
		// within touched feather samples. Authored and untouched samples are protected.

	private:
		AudioSpectralEnvelopeStroke(std::shared_ptr<const AudioSpectralEnvelope> before, double radiusSamps, double rampHalfSamps);
		void _PaintSample(unsigned long index, double startSample, float startGain,
			double endSample, float endGain) noexcept;
		bool _IsRampBoundary(unsigned long leftIndex) const noexcept;
		void _ApplyLocalRamps(std::vector<float>& gains) const noexcept;
		static unsigned long _WrapSample(double integralSample, unsigned long length) noexcept;
		std::shared_ptr<const AudioSpectralEnvelope> _before;
		std::vector<float> _gains;
		std::vector<double> _bestInfluence;
		std::vector<std::uint64_t> _winnerIds;
		std::vector<float> _winningTargets;
		double _radiusSamps;
		double _rampHalfSamps;
		std::uint64_t _winnerSerial = 0u;
		unsigned long _changedSamples = 0ul;
	};
}
