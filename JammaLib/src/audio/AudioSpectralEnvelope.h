#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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
		static double RelativeGain(double baseline, double factor, double floor, double maximum) noexcept;
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
		// UI-owned run provenance survives map handoffs. FIFO application, not
		// these IDs, gives a later equal-influence input priority.
		class RelativeMap;
		class RelativePath
		{
			friend class RelativeMap;
		public:
			bool Next(double start, double startDrag, double end, double endDrag) noexcept;
			std::uint32_t Winner() const noexcept { return _winner; }
		private:
			std::uint32_t _winner = 0u;
			double _end = 0.0, _endDrag = 0.0, _dx = 0.0, _dy = 0.0;
		};
		// Dense bounded scratch: 24 bytes/sample including reserved dirty indices.
		// UI freezes a map; worker reads it; terminal receipt returns it for reuse.
		class RelativeMap
		{
			friend class AudioSpectralEnvelopeStroke;
		public:
			static std::unique_ptr<RelativeMap> Create(unsigned long length, float sampleRate,
				double radiusSeconds, std::string& reason);
			bool Accumulate(double startSample, double startDrag, double endSample,
				double endDrag, RelativePath& path, std::string& reason);
			bool Empty() const noexcept { return _dirty.empty(); }
			void Clear() noexcept;
		private:
			RelativeMap(unsigned long length, double radius);
			void _AccumulateSample(unsigned long index, double start, double startDrag,
				double end, double endDrag, std::uint32_t winner) noexcept;
			std::vector<double> _influence, _drag;
			std::vector<std::uint32_t> _winners, _dirty;
			double _radius;
		};
		static std::unique_ptr<AudioSpectralEnvelopeStroke> Begin(
			std::shared_ptr<const AudioSpectralEnvelope> before, float sampleRate, std::string& reason,
			double radiusSeconds = AudioSpectralConfig::BarTimeRadiusSeconds);
		// One stroke uses exactly one mode: SetRelativeBand, ApplyRelativeMap,
		// or PaintSegment. Mixing modes/provenance in one stroke is unsupported.
		// Relative targets evaluate against E0, preserving silence and replacing prior targets.
		bool SetRelativeBand(double centreSample, float factor) noexcept;
		bool ApplyRelativeMap(const RelativeMap& map) noexcept;
		// Time is explicitly unwrapped. The caller supplies its intended path;
		// only brush support wraps, never an inferred shortest cursor movement.
		bool PaintSegment(double startSample, float startGain, double endSample, float endGain) noexcept;
		std::shared_ptr<const AudioSpectralEnvelope> Snapshot(std::string& reason) const;
		std::optional<double> RelativeFactorAt(unsigned long index) const noexcept;
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
		bool _relativeMode = false; // Single worker owner; never published mutable.
	};
}
