#pragma once

#include <array>
#include <algorithm>
#include <bitset>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "AudioSpectralConfig.h"

namespace spectral
{
	struct AudioSpectralAnalysis;
	class AudioSpectralEnvelope;
	class AudioSpectralEnvelopeStroke;

	// Value copies share tiles; editing a copy forks only the changed tiles.
	// Publish const value copies, never mutate a field already handed to a worker.
	class AudioSpectralGainField
	{
	public:
		struct ChangeBounds
        {
            std::size_t FirstFrame = static_cast<std::size_t>(-1), LastFrame = 0;
            unsigned int FirstBin = static_cast<unsigned int>(-1), LastBin = 0;
            void Include(std::size_t frame, unsigned int bin) noexcept
            {
                FirstFrame = (std::min)(FirstFrame, frame); LastFrame = (std::max)(LastFrame, frame);
                FirstBin = (std::min)(FirstBin, bin); LastBin = (std::max)(LastBin, bin);
            }
        };
        static constexpr unsigned int TileFrames = 32u;
		static constexpr unsigned int TileBins = 32u;
		struct Tile : std::array<float, TileFrames * TileBins>
		{
			std::bitset<TileFrames * TileBins> Owned;
		};
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
			double ExposureAt(std::size_t frame, unsigned int bin) const noexcept
			{ return frame < _frames && bin < _bins ? _exposure[frame * _bins + bin] : 0.0; }
			void Clear() noexcept;
		private:
			ExposureMap(std::size_t frames, unsigned int bins);
			std::size_t _frames;
			unsigned int _bins;
			std::vector<double> _exposure;
			std::vector<std::uint32_t> _dirty;
		};
		static std::optional<AudioSpectralGainField> Unity(
			std::size_t frames, unsigned int bins, std::string& reason);
		std::size_t FrameCount() const noexcept { return _frameCount; }
		unsigned int BinCount() const noexcept { return _binCount; }
		bool IsUnity() const noexcept { return _nonUnityBins == 0u && _ownedBins == 0u; }
		bool HasOwnedCoefficients() const noexcept { return _ownedBins != 0u; }
		bool IsOwned(std::size_t frame, unsigned int bin) const noexcept;
		bool FrameHasOwned(std::size_t frame) const noexcept;
		bool ContentEquals(const AudioSpectralGainField& other) const noexcept;
		bool SetOwned(std::size_t frame, unsigned int bin, bool owned);
		static unsigned long EnvelopeIndex(const AudioSpectralAnalysis&, std::size_t frame) noexcept;
		bool ApplyOwnedBand(const AudioSpectralAnalysis&, const AudioSpectralGainField& before,
			const AudioSpectralEnvelopeStroke& stroke, std::string& reason, ChangeBounds* changes = nullptr);
		float GainDb(std::size_t frame, unsigned int bin) const noexcept;
        double GainLinear(std::size_t frame, unsigned int bin) const noexcept;
		bool SetGainDb(std::size_t frame, unsigned int bin, float db);
		static float GainDbAfterExposure(const AudioSpectralAnalysis& analysis, std::size_t frame,
			unsigned int bin, float beforeDb, double exposureSeconds, bool increase) noexcept;
		bool ApplyExposureMap(const AudioSpectralAnalysis& analysis, const ExposureMap& exposure,
			bool increase, std::string& reason, const AudioSpectralEnvelope* envelope = nullptr, ChangeBounds* changes = nullptr);

		static double EffectiveTimeRadius(const AudioSpectralAnalysis&, double radius) noexcept;
		static double EffectiveFrequencyRadius(const AudioSpectralAnalysis&, double hz, double radius) noexcept;

	private:
		AudioSpectralGainField(std::size_t frames, unsigned int bins);
		template<class Visitor>
		static bool _VisitOval(const AudioSpectralAnalysis& analysis, double startSeconds, double startHz,
			double endSeconds, double endHz, double elapsedSeconds, std::string& reason,
			double timeRadius, double frequencyRadius, Visitor&& visit);
		std::size_t _TileIndex(std::size_t frame, unsigned int bin) const noexcept;
		static double _IntegratedShoulder(double x, double y, double dx, double dy, double first, double last) noexcept;
		static double _IntegratedOval(double x, double y, double dx, double dy,
			double first, double last) noexcept;
		std::size_t _frameCount;
		unsigned int _binCount;
		unsigned int _binTiles;
		std::size_t _nonUnityBins = 0u;
		std::size_t _ownedBins = 0u;
		std::vector<std::shared_ptr<Tile>> _tiles;
	};
}
