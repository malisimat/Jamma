#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "AudioSpectralConfig.h"

namespace audio
{
	struct AudioSpectralAnalysis;

	// Value copies share tiles; editing a copy forks only the changed tiles.
	// Publish const value copies, never mutate a field already handed to a worker.
	class AudioSpectralGainField
	{
	public:
		static constexpr unsigned int TileFrames = 32u;
		static constexpr unsigned int TileBins = 32u;
		using Tile = std::array<float, TileFrames * TileBins>;
		// UI accumulates, then freezes a map for worker application and UI preview.
		// Only the UI clears/recycles it after the worker's terminal receipt.
		// Storage is reserved once, so rasterizing a pointer sample cannot allocate.
		class ExposureMap
		{
			friend class AudioSpectralGainField;
		public:
			static std::unique_ptr<ExposureMap> Create(std::size_t frames, unsigned int bins, std::string& reason);
			bool Accumulate(const AudioSpectralAnalysis& analysis, double startSeconds, double startHz,
				double endSeconds, double endHz, double elapsedSeconds, std::string& reason,
				double timeRadius = AudioSpectralConfig::BrushTimeRadiusSeconds,
				double frequencyRadius = AudioSpectralConfig::BrushFrequencyRadiusOctaves);
			bool Empty() const noexcept { return _dirty.empty(); }
			std::size_t DirtyCellCount() const noexcept { return _dirty.size(); }
			double ElapsedSeconds() const noexcept { return _elapsed; }
			double ExposureAt(std::size_t frame, unsigned int bin) const noexcept
			{ return frame < _frames && bin < _bins ? _exposure[frame * _bins + bin] : 0.0; }
			void Clear() noexcept;
		private:
			ExposureMap(std::size_t frames, unsigned int bins);
			std::size_t _frames;
			unsigned int _bins;
			std::vector<double> _exposure;
			std::vector<std::uint32_t> _dirty;
			double _elapsed = 0.0;
		};
		static std::optional<AudioSpectralGainField> Unity(
			std::size_t frames, unsigned int bins, std::string& reason);
		std::size_t FrameCount() const noexcept { return _frameCount; }
		unsigned int BinCount() const noexcept { return _binCount; }
		bool IsUnity() const noexcept { return _nonUnityBins == 0u; }
		float GainDb(std::size_t frame, unsigned int bin) const noexcept;
        double GainLinear(std::size_t frame, unsigned int bin) const noexcept;
		bool SetGainDb(std::size_t frame, unsigned int bin, float db);
		static float GainDbAfterExposure(const AudioSpectralAnalysis& analysis, std::size_t frame,
			unsigned int bin, float beforeDb, double exposureSeconds, bool increase) noexcept;
		bool ApplyExposureMap(const AudioSpectralAnalysis& analysis, const ExposureMap& exposure,
			bool increase, std::string& reason);

		// Integrates the compact oval analytically along a straight swept path.
		// Inputs are body-source seconds and positive Hz; time support wraps.
		// Caller supplies intended unwrapped movement (at most one loop per segment).
		bool PaintOvalSegment(const AudioSpectralAnalysis& analysis,
			double startSeconds, double startHz, double endSeconds, double endHz,
			double elapsedSeconds, bool increase, std::string& reason);

	private:
		AudioSpectralGainField(std::size_t frames, unsigned int bins);
		bool _PaintOvalSegment(const AudioSpectralAnalysis& analysis,
			double startSeconds, double startHz, double endSeconds, double endHz,
			double elapsedSeconds, bool increase, std::string& reason);
		template<class Visitor>
		static bool _VisitOval(const AudioSpectralAnalysis& analysis, double startSeconds, double startHz,
			double endSeconds, double endHz, double elapsedSeconds, std::string& reason,
			double timeRadius, double frequencyRadius, Visitor&& visit);
		std::size_t _TileIndex(std::size_t frame, unsigned int bin) const noexcept;
		static double _IntegratedOval(double x, double y, double dx, double dy,
			double first, double last) noexcept;
		std::size_t _frameCount;
		unsigned int _binCount;
		unsigned int _binTiles;
		std::size_t _nonUnityBins = 0u;
		std::vector<std::shared_ptr<Tile>> _tiles;
	};
}
