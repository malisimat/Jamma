#include "AudioSpectralGainField.h"
#include "AudioSpectralDsp.h"
#include "AudioSpectralEnvelope.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <limits>

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

bool AudioSpectralGainField::IsOwned(std::size_t frame, unsigned int bin) const noexcept
{
    if (frame >= _frameCount || bin >= _binCount) return false;
    const auto& tile = _tiles[_TileIndex(frame, bin)];
    return tile && tile->Owned[(frame % TileFrames) * TileBins + bin % TileBins];
}

bool AudioSpectralGainField::ContentEquals(const AudioSpectralGainField& other) const noexcept
{
    if (_frameCount != other._frameCount || _binCount != other._binCount
        || _ownedBins != other._ownedBins || _nonUnityBins != other._nonUnityBins) return false;
    for (std::size_t i = 0; i < _tiles.size(); ++i)
    {
        if (_tiles[i] == other._tiles[i]) continue;
        if (_tiles[i] && other._tiles[i])
        {
            if (_tiles[i]->Owned != other._tiles[i]->Owned
                || !std::equal(_tiles[i]->begin(), _tiles[i]->end(), other._tiles[i]->begin())) return false;
        }
        else
        {
            const auto& tile = _tiles[i] ? _tiles[i] : other._tiles[i];
            if (tile->Owned.any() || std::any_of(tile->begin(), tile->end(), [](float db) { return db != 0.0f; })) return false;
        }
    }
    return true;
}

bool AudioSpectralGainField::FrameHasOwned(std::size_t frame) const noexcept
{
    if (!_ownedBins || frame >= _frameCount) return false;
    for (unsigned int column = 0; column < _binTiles; ++column)
    {
        const auto& tile = _tiles[(frame / TileFrames) * _binTiles + column];
        if (!tile || tile->Owned.none()) continue;
        for (unsigned int bin = 0; bin < TileBins; ++bin)
            if (tile->Owned[(frame % TileFrames) * TileBins + bin]) return true;
    }
    return false;
}

bool AudioSpectralGainField::SetOwned(std::size_t frame, unsigned int bin, bool owned)
{
    if (frame >= _frameCount || bin >= _binCount || IsOwned(frame, bin) == owned) return false;
    auto& tile = _tiles[_TileIndex(frame, bin)];
    if (!tile) tile = std::make_shared<Tile>();
    else if (tile.use_count() != 1u) tile = std::make_shared<Tile>(*tile);
    tile->Owned[(frame % TileFrames) * TileBins + bin % TileBins] = owned;
    if (owned) ++_ownedBins; else --_ownedBins;
    return true;
}

unsigned long AudioSpectralGainField::EnvelopeIndex(const AudioSpectralAnalysis& analysis, std::size_t frame) noexcept
{
    const auto length = static_cast<std::int64_t>(analysis.Source->Layout.LogicalLength);
    const auto sample = analysis.Geometry.FrameCentre(frame, analysis.Transform.HopSamps) % length;
    return static_cast<unsigned long>(sample < 0 ? sample + length : sample);
}

bool AudioSpectralGainField::ApplyOwnedBand(const AudioSpectralAnalysis& analysis,
    const AudioSpectralGainField& before, const AudioSpectralEnvelopeStroke& stroke, std::string& reason)
{
    reason.clear();
    try
    {
        auto candidate = *this;
        const auto floor = std::pow(10.0, AudioSpectralConfig::SpectralFloorDb / 20.0);
        bool changed = false;
        for (std::size_t frame = 0; frame < _frameCount; ++frame)
        {
            if (!before.FrameHasOwned(frame)) continue;
            const auto index = EnvelopeIndex(analysis, frame);
            const auto factor = stroke.RelativeFactorAt(index);
            if (!factor) continue;
            for (unsigned int bin = 0; bin < _binCount; ++bin)
            {
                if (!before.IsOwned(frame, bin)) continue;
                const auto db = before.GainDb(frame, bin);
                const auto original = analysis.Magnitude(frame, bin);
                if (!(original > 0.0)) continue;
                const auto baseline = db == AudioSpectralConfig::SilenceGainDb ? 0.0
                    : original * before.GainLinear(frame, bin);
                const auto target = AudioSpectralEnvelope::RelativeGain(baseline, *factor,
                    floor, AudioSpectralEnvelope::MaximumGain());
                const auto targetDb = *factor == 1.0 ? db : target == 0.0
                    ? AudioSpectralConfig::SilenceGainDb : static_cast<float>(20.0 * std::log10(target / original));
                changed |= candidate.SetGainDb(frame, bin, targetDb);
            }
        }
        if (changed) *this = std::move(candidate);
        return changed;
    }
    catch (const std::bad_alloc&) { reason = "Not enough memory for recovered band targets"; return false; }
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

double AudioSpectralGainField::_IntegratedShoulder(double x, double y, double dx, double dy,
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

double AudioSpectralGainField::EffectiveTimeRadius(const AudioSpectralAnalysis& analysis, double radius) noexcept
{
    return (std::max)(radius, analysis.Transform.HopSamps / static_cast<double>(analysis.Source->Layout.SampleRate));
}

double AudioSpectralGainField::EffectiveFrequencyRadius(const AudioSpectralAnalysis& analysis, double hz, double radius) noexcept
{
    const auto spacing = analysis.Source->Layout.SampleRate / static_cast<double>(analysis.Transform.WindowSamps);
    const auto lower = (std::min)(AudioSpectralConfig::DisplayMinimumFrequencyHz, analysis.Source->Layout.SampleRate * 0.25);
    // Fixed per-coefficient support: segmentation cannot change the response.
    return (std::max)(radius, std::log2(1.0 + spacing / (std::max)(lower, hz - spacing)));
}

double AudioSpectralGainField::_IntegratedOval(double x, double y, double dx, double dy,
    double first, double last) noexcept
{
    constexpr double plateauSquared = 0.5625; // Inner 75% receives the full 60 dB/s.
    constexpr double shoulderScale = (1.0 - plateauSquared) * (1.0 - plateauSquared);
    const auto a = dx * dx + dy * dy;
    if (a < 1.0e-24)
    {
        const auto middle = (first + last) * 0.5;
        const auto q = std::pow(x + dx * middle, 2.0) + std::pow(y + dy * middle, 2.0);
        return (last - first) * (q <= plateauSquared ? 1.0 : std::pow((std::max)(0.0, 1.0-q), 2.0) / shoulderScale);
    }
    auto result = _IntegratedShoulder(x,y,dx,dy,first,last);
    const auto centre = -(x*dx+y*dy)/a;
    const auto closest = std::pow(x+dx*centre,2.0)+std::pow(y+dy*centre,2.0);
    if (closest < plateauSquared)
    {
        const auto half = std::sqrt((plateauSquared-closest)/a);
        const auto lo = (std::max)(first,centre-half);
        const auto hi = (std::min)(last,centre+half);
        if (hi > lo) result += (hi-lo)*shoulderScale - _IntegratedShoulder(x,y,dx,dy,lo,hi);
    }
    return std::clamp(result / shoulderScale, 0.0, last-first);
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
    if (_frameCount != analysis.Geometry.FrameCount || _binCount != analysis.Geometry.BinCount)
    { reason = "Spectral gain field dimensions differ from the source"; return false; }
    return _VisitOval(analysis, startSeconds, startHz, endSeconds, endHz, elapsedSeconds, reason,
        AudioSpectralConfig::BrushTimeRadiusSeconds, AudioSpectralConfig::BrushFrequencyRadiusOctaves,
        [&](std::size_t frame, unsigned int bin, double exposure)
        { return SetGainDb(frame, bin, GainDbAfterExposure(analysis, frame, bin, GainDb(frame, bin), exposure, increase)); });
}

float AudioSpectralGainField::GainDbAfterExposure(const AudioSpectralAnalysis& analysis, std::size_t frame,
    unsigned int bin, float beforeDb, double exposureSeconds, bool increase) noexcept
{
    if (!(exposureSeconds > 0.0)) return beforeDb;
    const auto magnitude = analysis.Magnitude(frame, bin);
    if (!(magnitude > 0.0)) return beforeDb;
    const auto originalDb = 20.0 * std::log10(magnitude);
    const auto silent = beforeDb == AudioSpectralConfig::SilenceGainDb;
    if (silent && !increase) return beforeDb;
    const auto currentDb = silent ? AudioSpectralConfig::SpectralFloorDb : originalDb + beforeDb;
    const auto delta = (std::min)(2400.0, exposureSeconds * AudioSpectralConfig::BrushDbPerSecond);
    const auto targetDb = increase
        ? (std::min)(AudioSpectralConfig::SpectralCeilingDb,
            (std::max)(currentDb, AudioSpectralConfig::SpectralFloorDb) + delta)
        : currentDb - delta;
    return !increase && targetDb <= AudioSpectralConfig::SpectralFloorDb
        ? AudioSpectralConfig::SilenceGainDb : static_cast<float>(targetDb - originalDb);
}

AudioSpectralGainField::ExposureMap::ExposureMap(std::size_t frames, unsigned int bins)
    : _frames(frames), _bins(bins), _exposure(frames * bins, 0.0)
{
    _dirty.reserve(_exposure.size());
}

std::unique_ptr<AudioSpectralGainField::ExposureMap> AudioSpectralGainField::ExposureMap::Create(
    std::size_t frames, unsigned int bins, std::string& reason)
{
    reason.clear();
    if (!frames || !bins || frames > AudioSpectralConfig::MaxWorkingBytes / AudioSpectralConfig::InputMapBytesPerCoefficient / bins
        || frames > (std::numeric_limits<std::uint32_t>::max)() / bins)
    { reason = "Spectral input map exceeds its bounded dimensions"; return nullptr; }
    try { return std::unique_ptr<ExposureMap>(new ExposureMap(frames, bins)); }
    catch (const std::bad_alloc&) { reason = "Not enough memory for the spectral input map"; return nullptr; }
}

bool AudioSpectralGainField::ExposureMap::Accumulate(const AudioSpectralAnalysis& analysis,
    double startSeconds, double startHz, double endSeconds, double endHz, double elapsedSeconds,
    std::string& reason, double timeRadius, double frequencyRadius)
{
    reason.clear();
    if (_frames != analysis.Geometry.FrameCount || _bins != analysis.Geometry.BinCount
        )
    { reason = "Spectral input map dimensions differ from the source"; return false; }
    const auto changed = AudioSpectralGainField::_VisitOval(analysis, startSeconds, startHz,
        endSeconds, endHz, elapsedSeconds, reason, timeRadius, frequencyRadius,
        [&](std::size_t frame, unsigned int bin, double exposure)
    {
        const auto index = static_cast<std::uint32_t>(frame * _bins + bin);
        auto& value = _exposure[index];
        if (value == 0.0) _dirty.push_back(index); // Full capacity was reserved at creation.
        value = (std::min)(AudioSpectralConfig::MaximumBrushExposureSeconds, value + exposure); // Beyond 2400 dB, every finite input has saturated.
        return true;
    });
    if (reason.empty()) _elapsed += elapsedSeconds;
    return changed;
}

void AudioSpectralGainField::ExposureMap::Clear() noexcept
{
    for (const auto index : _dirty) _exposure[index] = 0.0;
    _dirty.clear();
    _elapsed = 0.0;
}

bool AudioSpectralGainField::ApplyExposureMap(const AudioSpectralAnalysis& analysis,
    const ExposureMap& exposure, bool increase, std::string& reason, const AudioSpectralEnvelope* envelope)
{
    reason.clear();
    if (_frameCount != exposure._frames || _binCount != exposure._bins
        || _frameCount != analysis.Geometry.FrameCount || _binCount != analysis.Geometry.BinCount)
    { reason = "Spectral input map dimensions differ from the gain field"; return false; }
    try
    {
        auto candidate = *this;
        bool changed = false;
        for (const auto index : exposure._dirty)
        {
            const auto frame = index / _binCount;
            const auto bin = index % _binCount;
            auto beforeDb = candidate.GainDb(frame, bin);
            const auto mask = envelope ? envelope->GainAt(EnvelopeIndex(analysis, frame)) : 1.0f;
            if (exposure._exposure[index] > 0.0 && analysis.Magnitude(frame, bin) > 0.0
                && mask != 1.0f && !candidate.IsOwned(frame, bin))
            {
                beforeDb = mask == 0.0f || beforeDb == AudioSpectralConfig::SilenceGainDb
                    ? AudioSpectralConfig::SilenceGainDb : beforeDb + static_cast<float>(20.0 * std::log10(mask));
                changed |= candidate.SetOwned(frame, bin, true);
            }
            changed |= candidate.SetGainDb(frame, bin, GainDbAfterExposure(analysis, frame, bin,
                beforeDb, exposure._exposure[index], increase));
        }
        if (changed) *this = std::move(candidate);
        return changed;
    }
    catch (const std::bad_alloc&) { reason = "Not enough memory to apply the spectral input map"; return false; }
}

template<class Visitor>
bool AudioSpectralGainField::_VisitOval(const AudioSpectralAnalysis& analysis,
    double startSeconds, double startHz, double endSeconds, double endHz,
    double elapsedSeconds, std::string& reason, double timeRadius, double frequencyRadius, Visitor&& visit)
{
	reason.clear();
	if (!analysis.Source || !analysis.Source->Layout.IsValid() || !analysis.Transform.IsValid()
		|| analysis.Geometry.FrameCount == 0u || analysis.Geometry.BinCount == 0u
		|| analysis.Coefficients.size() / analysis.Geometry.BinCount != analysis.Geometry.FrameCount
		|| analysis.Coefficients.size() % analysis.Geometry.BinCount != 0u
		|| !std::isfinite(startSeconds) || !std::isfinite(endSeconds)
		|| !std::isfinite(startHz) || !std::isfinite(endHz) || startHz <= 0.0 || endHz <= 0.0
		|| !std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0
		|| !std::isfinite(timeRadius) || timeRadius <= 0.0 || timeRadius > 2.0
		|| !std::isfinite(frequencyRadius) || frequencyRadius <= 0.0 || frequencyRadius > 4.0)
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
	timeRadius = EffectiveTimeRadius(analysis, timeRadius);
	const auto octaveRadius = EffectiveFrequencyRadius(analysis, lower, frequencyRadius);
	const auto lowHz = (std::min)(startHz, endHz) * std::exp2(-octaveRadius);
	const auto highHz = (std::min)(nyquist, (std::max)(startHz, endHz) * std::exp2(octaveRadius));
	const auto firstBin = lowHz <= lower ? 0u
		: static_cast<unsigned int>(std::ceil(lowHz * analysis.Transform.WindowSamps / sampleRate));
	const auto lastBin = (std::min)(analysis.Geometry.BinCount - 1u,
		static_cast<unsigned int>(std::floor(highHz * analysis.Transform.WindowSamps / sampleRate)));
	bool changed = false;
	const auto visitFrame = [&](std::size_t frame)
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
			const auto& original = analysis.Coefficients[frame * analysis.Geometry.BinCount + bin];
			if (original.Real == 0.0f && original.Imaginary == 0.0f) continue;
			// DC and sub-display-floor bins share the explicit lower visual edge.
			const auto hz = (std::max)(lower, bin * sampleRate / analysis.Transform.WindowSamps);
			const auto binRadius = EffectiveFrequencyRadius(analysis, hz, frequencyRadius);
			const auto y = (std::log2(hz) - startOctave) / binRadius;
			auto exposure = _IntegratedOval((time + firstImage * period - startSeconds) / timeRadius,
				y, -deltaTime / timeRadius, -deltaOctave / binRadius, 0.0, split);
			if (split < 1.0)
				exposure += _IntegratedOval((time + lastImage * period - startSeconds) / timeRadius,
					y, -deltaTime / timeRadius, -deltaOctave / binRadius, split, 1.0);
			if (exposure <= 0.0) continue;
            changed |= visit(frame, bin, elapsedSeconds * exposure);
		}
	};
    const auto firstTime = (std::min)(startSeconds, endSeconds) - timeRadius;
    const auto lastTime = (std::max)(startSeconds, endSeconds) + timeRadius;
    if (lastTime - firstTime >= period)
    {
        for (std::size_t frame = 0; frame < analysis.Geometry.FrameCount; ++frame) visitFrame(frame);
    }
    else
    {
        // Only hop-lattice rows intersecting the swept time support are visited.
        // Context rows outside the body get their corresponding wrapped exposure.
        const auto firstCentre = analysis.Geometry.FirstFrameCentre / sampleRate;
        const auto lastCentre = analysis.Geometry.FrameCentre(analysis.Geometry.FrameCount - 1u,
            analysis.Transform.HopSamps) / sampleRate;
        const auto firstImage = static_cast<std::int64_t>(std::ceil((firstCentre - lastTime) / period));
        const auto lastImage = static_cast<std::int64_t>(std::floor((lastCentre - firstTime) / period));
        for (auto image = firstImage; image <= lastImage; ++image)
        {
            const auto firstRow = std::ceil(((firstTime + image * period) * sampleRate
                - analysis.Geometry.FirstFrameCentre) / analysis.Transform.HopSamps);
            const auto lastRow = std::floor(((lastTime + image * period) * sampleRate
                - analysis.Geometry.FirstFrameCentre) / analysis.Transform.HopSamps);
            if (lastRow < 0 || firstRow >= static_cast<double>(analysis.Geometry.FrameCount)) continue;
            const auto first = static_cast<std::size_t>((std::max)(0.0, firstRow));
            const auto last = static_cast<std::size_t>((std::min)(lastRow,
                static_cast<double>(analysis.Geometry.FrameCount - 1u)));
            for (auto frame = first; frame <= last; ++frame) visitFrame(frame);
        }
    }
	return changed;
}
