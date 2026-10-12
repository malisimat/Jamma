#include "AudioSpectralGainField.h"
#include "AudioSpectralDsp.h"

#include <algorithm>
#include <cmath>
#include <new>

using namespace audio;

AudioSpectralGainField::AudioSpectralGainField(std::size_t frames, unsigned int bins) :
	_frameCount(frames), _binCount(bins), _binTiles((bins - 1u) / TileBins + 1u),
	_tiles(((frames - 1u) / TileFrames + 1u) * _binTiles)
{
}

std::optional<AudioSpectralGainField> AudioSpectralGainField::Unity(
	std::size_t frames, unsigned int bins, std::string& reason)
{
	reason.clear();
	if (frames == 0u || bins == 0u)
	{
		reason = "Spectral gain field dimensions are empty";
		return std::nullopt;
	}
	const auto frameTiles = (frames - 1u) / TileFrames + 1u;
	const auto binTiles = (bins - 1u) / TileBins + 1u;
	if (frameTiles > AudioSpectralConfig::MaxWorkingBytes
		/ (sizeof(Tile) + sizeof(std::shared_ptr<Tile>) + 64u) / binTiles)
	{
		reason = "Spectral gain tiles exceed the editor memory budget";
		return std::nullopt;
	}
	try { return AudioSpectralGainField(frames, bins); }
	catch (const std::bad_alloc&)
	{
		reason = "Not enough memory for spectral gain tiles";
		return std::nullopt;
	}
}

std::size_t AudioSpectralGainField::_TileIndex(std::size_t frame, unsigned int bin) const noexcept
{
	return (frame / TileFrames) * _binTiles + bin / TileBins;
}

float AudioSpectralGainField::GainDb(std::size_t frame, unsigned int bin) const noexcept
{
	if (frame >= _frameCount || bin >= _binCount) return 0.0f;
	const auto& tile = _tiles[_TileIndex(frame, bin)];
	return tile ? (*tile)[(frame % TileFrames) * TileBins + bin % TileBins] : 0.0f;
}

double AudioSpectralGainField::GainLinear(std::size_t frame, unsigned int bin) const noexcept
{
    const auto db = GainDb(frame, bin);
    return db == AudioSpectralConfig::SilenceGainDb ? 0.0 : std::pow(10.0, db / 20.0);
}
bool AudioSpectralGainField::SetGainDb(std::size_t frame, unsigned int bin, float db)
{
	if (frame >= _frameCount || bin >= _binCount || !std::isfinite(db)) return false;
	if (db != AudioSpectralConfig::SilenceGainDb)
        db = std::clamp(db, AudioSpectralConfig::MinimumSpectralGainDb, AudioSpectralConfig::MaximumSpectralGainDb);
	const auto old = GainDb(frame, bin);
	if (old == db) return false;
	auto& tile = _tiles[_TileIndex(frame, bin)];
	if (!tile) tile = std::make_shared<Tile>();
	else if (tile.use_count() != 1u) tile = std::make_shared<Tile>(*tile);
	(*tile)[(frame % TileFrames) * TileBins + bin % TileBins] = db;
	if (old == 0.0f) ++_nonUnityBins;
	if (db == 0.0f) --_nonUnityBins;
	return true;
}

double AudioSpectralGainField::_IntegratedOval(double x, double y, double dx, double dy,
	double first, double last) noexcept
{
	const auto a = dx * dx + dy * dy;
	if (a < 1.0e-24)
	{
		// Sub-float-resolution motion: midpoint avoids ill-conditioned remote minima.
		const auto middle = (first + last) * 0.5;
		const auto q = std::pow(x + dx * middle, 2.0) + std::pow(y + dy * middle, 2.0);
		const auto h = (std::max)(0.0, 1.0 - q);
		return (last - first) * h * h;
	}
	const auto centre = -(x * dx + y * dy) / a;
	const auto nearX = x + dx * centre;
	const auto nearY = y + dy * centre;
	const auto h = 1.0 - (nearX * nearX + nearY * nearY);
	if (h <= 0.0) return 0.0;
	const auto radius = std::sqrt(h / a);
	const auto lo = (std::max)(first, centre - radius);
	const auto hi = (std::min)(last, centre + radius);
	if (lo >= hi) return 0.0;
	// Integrate h^2*(1-r^2)^2 with r in [-1,1], avoiding cancellation
	// between large quartic terms on long fast sweeps.
	const auto r0 = std::clamp((lo - centre) / radius, -1.0, 1.0);
	const auto r1 = std::clamp((hi - centre) / radius, -1.0, 1.0);
	const auto integral0 = r0 * (1.0 - r0 * r0 * (2.0 / 3.0 - r0 * r0 / 5.0));
	const auto integral1 = r1 * (1.0 - r1 * r1 * (2.0 / 3.0 - r1 * r1 / 5.0));
	return std::clamp(h * h * radius * (integral1 - integral0), 0.0, last - first);
}

bool AudioSpectralGainField::PaintOvalSegment(const AudioSpectralAnalysis& analysis,
	double startSeconds, double startHz, double endSeconds, double endHz,
	double elapsedSeconds, bool increase, std::string& reason)
{
	try
	{
		auto candidate = *this;
		const auto changed = candidate._PaintOvalSegment(analysis, startSeconds, startHz,
			endSeconds, endHz, elapsedSeconds, increase, reason);
		if (changed) *this = std::move(candidate);
		return changed;
	}
	catch (const std::bad_alloc&)
	{
		reason = "Not enough memory for this spectral brush update";
		return false; // Original tiles are unchanged; never publish a partial update.
	}
}

bool AudioSpectralGainField::_PaintOvalSegment(const AudioSpectralAnalysis& analysis,
	double startSeconds, double startHz, double endSeconds, double endHz,
	double elapsedSeconds, bool increase, std::string& reason)
{
	reason.clear();
	if (!analysis.Source || !analysis.Source->Layout.IsValid() || !analysis.Transform.IsValid()
		|| _frameCount != analysis.Geometry.FrameCount || _binCount != analysis.Geometry.BinCount
		|| analysis.Coefficients.size() / _binCount != _frameCount
		|| analysis.Coefficients.size() % _binCount != 0u
		|| !std::isfinite(startSeconds) || !std::isfinite(endSeconds)
		|| !std::isfinite(startHz) || !std::isfinite(endHz) || startHz <= 0.0 || endHz <= 0.0
		|| !std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0)
	{
		reason = "Spectral brush coordinates or gain dimensions are invalid";
		return false;
	}
	if (elapsedSeconds == 0.0) return false;
	const auto sampleRate = static_cast<double>(analysis.Source->Layout.SampleRate);
	const auto period = analysis.Source->Layout.LogicalLength / sampleRate;
	const auto deltaTime = endSeconds - startSeconds;
	if (!std::isfinite(deltaTime) || std::abs(deltaTime) > period)
	{
		reason = "Spectral brush segment spans more than one Loop";
		return false;
	}
	startSeconds = std::fmod(startSeconds, period);
	if (startSeconds < 0.0) startSeconds += period;
	endSeconds = startSeconds + deltaTime;
	const auto nyquist = sampleRate * 0.5;
	const auto lower = (std::min)(AudioSpectralConfig::DisplayMinimumFrequencyHz, nyquist * 0.5);
	startHz = std::clamp(startHz, lower, nyquist);
	endHz = std::clamp(endHz, lower, nyquist);
	const auto startOctave = std::log2(startHz);
	const auto deltaOctave = std::log2(endHz) - startOctave;
	const auto octaveRadius = AudioSpectralConfig::BrushFrequencyRadiusOctaves;
	const auto timeRadius = AudioSpectralConfig::BrushTimeRadiusSeconds;
	const auto lowHz = (std::min)(startHz, endHz) * std::exp2(-octaveRadius);
	const auto highHz = (std::min)(nyquist, (std::max)(startHz, endHz) * std::exp2(octaveRadius));
	const auto firstBin = lowHz <= lower ? 0u
		: static_cast<unsigned int>(std::ceil(lowHz * analysis.Transform.WindowSamps / sampleRate));
	const auto lastBin = (std::min)(_binCount - 1u,
		static_cast<unsigned int>(std::floor(highHz * analysis.Transform.WindowSamps / sampleRate)));
	bool changed = false;
	for (std::size_t frame = 0u; frame < _frameCount; ++frame)
	{
		auto time = std::fmod(analysis.Geometry.FrameCentre(frame, analysis.Transform.HopSamps) / sampleRate, period);
		if (time < 0.0) time += period;
		const auto firstImage = std::floor((startSeconds - time) / period + 0.5);
		const auto lastImage = std::floor((endSeconds - time) / period + 0.5);
		double split = 1.0;
		if (firstImage != lastImage && deltaTime != 0.0)
		{
			const auto boundary = time + ((std::min)(firstImage, lastImage) + 0.5) * period;
			split = std::clamp((boundary - startSeconds) / deltaTime, 0.0, 1.0);
		}
		for (unsigned int bin = firstBin; bin <= lastBin; ++bin)
		{
			const auto& original = analysis.Coefficients[frame * _binCount + bin];
			if (original.Real == 0.0f && original.Imaginary == 0.0f) continue;
			// DC and sub-display-floor bins share the explicit lower visual edge.
			const auto hz = (std::max)(lower, bin * sampleRate / analysis.Transform.WindowSamps);
			const auto y = (std::log2(hz) - startOctave) / octaveRadius;
			auto exposure = _IntegratedOval((time + firstImage * period - startSeconds) / timeRadius,
				y, -deltaTime / timeRadius, -deltaOctave / octaveRadius, 0.0, split);
			if (split < 1.0)
				exposure += _IntegratedOval((time + lastImage * period - startSeconds) / timeRadius,
					y, -deltaTime / timeRadius, -deltaOctave / octaveRadius, split, 1.0);
			if (exposure <= 0.0) continue;
            const auto originalMagnitude = analysis.Magnitude(frame, bin);
            if (!(originalMagnitude > 0.0)) continue;
            const auto originalDb = 20.0 * std::log10(originalMagnitude);
            const auto old = GainDb(frame, bin);
            const auto silent = old == AudioSpectralConfig::SilenceGainDb;
            if (silent && !increase) continue;
            const auto currentDb = silent ? AudioSpectralConfig::SpectralFloorDb : originalDb + old;
            const auto delta = (std::min)(2400.0, elapsedSeconds * AudioSpectralConfig::BrushDbPerSecond * exposure);
            // Author absolute magnitudes, retaining original coefficients for recovery.
            // Boosting silence starts at the visual floor; originally zero bins stay zero.
            const auto targetDb = increase
                ? (std::min)(AudioSpectralConfig::SpectralCeilingDb,
                    (std::max)(currentDb, AudioSpectralConfig::SpectralFloorDb) + delta)
                : currentDb - delta;
            const auto next = !increase && targetDb <= AudioSpectralConfig::SpectralFloorDb
                ? AudioSpectralConfig::SilenceGainDb : static_cast<float>(targetDb - originalDb);
            changed |= SetGainDb(frame, bin, next);
		}
	}
	return changed;
}
