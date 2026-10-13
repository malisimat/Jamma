#include "AudioSpectralDsp.h"
#include "AudioSpectralGainField.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>

using namespace audio;

std::span<const AudioSpectralFft::Bin> AudioSpectralAnalysis::Frame(std::size_t index) const noexcept
{
	if (index >= Geometry.FrameCount || Geometry.BinCount == 0u
		|| Geometry.FrameCount > Coefficients.size() / Geometry.BinCount) return {};
	return std::span<const AudioSpectralFft::Bin>(Coefficients).subspan(index * Geometry.BinCount, Geometry.BinCount);
}

double AudioSpectralAnalysis::Magnitude(std::size_t frame, unsigned int bin) const noexcept
{
	if (frame >= Geometry.FrameCount || bin >= Geometry.BinCount || WindowSum <= 0.0) return 0.0;
	const auto values = Frame(frame);
	if (values.empty()) return 0.0;
	const auto& value = values[bin];
	const auto oneSidedScale = bin == 0u || bin + 1u == Geometry.BinCount ? 1.0 : 2.0;
	return std::hypot(static_cast<double>(value.Real), static_cast<double>(value.Imaginary)) * oneSidedScale / WindowSum;
}

std::optional<AudioSpectralMemoryEstimate> AudioSpectralDsp::EstimateWorkingMemory(
	const AudioSpectralSourceLayout& layout, const AudioSpectralTransformConfig& transform,
	std::string& reason)
{
	reason.clear();
	if (!layout.IsValid() || !transform.IsValid())
	{
		reason = "Audio source or spectral transform configuration is invalid";
		return std::nullopt;
	}
	const auto hop = static_cast<std::int64_t>(transform.HopSamps);
	const auto half = static_cast<std::int64_t>(transform.WindowSamps / 2u);
	const auto first = -((static_cast<std::int64_t>(layout.FadeSamps) + half + hop - 1) / hop) * hop;
	const auto last = ((static_cast<std::int64_t>(layout.LogicalLength) - 1 + half + hop - 1) / hop) * hop;
	AudioSpectralMemoryEstimate estimate;
	estimate.Geometry = { first, static_cast<std::size_t>((last - first) / hop + 1),
		transform.WindowSamps / 2u + 1u, layout.BodyOffset - layout.FadeSamps,
		layout.LogicalLength + layout.FadeSamps };
	const auto coefficients = static_cast<std::uint64_t>(estimate.Geometry.FrameCount) * estimate.Geometry.BinCount;
	const auto tileRows = (estimate.Geometry.FrameCount - 1u) / AudioSpectralGainField::TileFrames + 1u;
	const auto tileColumns = (estimate.Geometry.BinCount - 1u) / AudioSpectralGainField::TileBins + 1u;
	// Conservative peak: old/new originals + bounded processed handles, full gain
	// tile revisions (draft, before, worker, queued, display and restoration),
    // envelope stroke/retained envelopes, WOLA scratch, display and history.
	// Three analyses cover the previous visible result, cancelled running source,
	// and newest source result without forcing a punch-in/UI wait.
	// Layout/config bounds above keep these uint64 calculations below overflow.
	estimate.WorkingBytes = static_cast<std::uint64_t>(layout.StoredSampleCount()) * sizeof(float) * 6u
		+ coefficients * sizeof(AudioSpectralFft::Bin) * 3u
        + coefficients * AudioSpectralConfig::InputMapBytesPerCoefficient
		+ static_cast<std::uint64_t>(tileRows) * tileColumns
			* (sizeof(AudioSpectralGainField::Tile) + sizeof(std::shared_ptr<AudioSpectralGainField::Tile>) + 64u) * 6u
		+ static_cast<std::uint64_t>(layout.LogicalLength) * 64u
		+ static_cast<std::uint64_t>(estimate.Geometry.TargetLength) * sizeof(double) * 2u
		+ static_cast<std::uint64_t>(transform.WindowSamps) * 64u
		+ AudioSpectralConfig::DisplayWorkingBytes + AudioSpectralConfig::MaxHistoryBytes;
	if (estimate.WorkingBytes > AudioSpectralConfig::MaxWorkingBytes)
	{
		reason = "Spectral editor needs approximately " + std::to_string(
			(estimate.WorkingBytes + 1024u * 1024u - 1u) / (1024u * 1024u))
			+ " MiB; current budget is " + std::to_string(AudioSpectralConfig::MaxWorkingBytes / (1024u * 1024u)) + " MiB";
		return std::nullopt;
	}
	return estimate;
}

bool AudioSpectralDsp::SelectTransform(const AudioSpectralSourceLayout& layout,
    const std::shared_ptr<AudioSpectralResourceBudget>& budget,
    AudioSpectralTransformConfig& transform,
    std::shared_ptr<const AudioSpectralResourceBudget::Reservation>& resources,
    std::string& reason)
{
    resources.reset();
    for (auto hop = AudioSpectralConfig::HopSamps; hop <= AudioSpectralConfig::BaselineHopSamps; hop *= 2u)
    {
        const AudioSpectralTransformConfig candidate{ AudioSpectralConfig::TransformVersion,
            AudioSpectralConfig::WindowSamps, hop };
        const auto estimate = EstimateWorkingMemory(layout, candidate, reason);
        if (!estimate) continue;
        if (hop != AudioSpectralConfig::BaselineHopSamps
            && estimate->WorkingBytes > AudioSpectralConfig::PreferredWorkingBytes) continue;
        if (budget)
        {
            resources = budget->Reserve(estimate->WorkingBytes, reason);
            if (!resources) continue;
        }
        transform = candidate;
        reason = hop == AudioSpectralConfig::HopSamps ? ""
            : "Using " + std::to_string(hop) + "-sample spectral time steps to fit the memory budget";
        return true;
    }
    return false;
}

float AudioSpectralDsp::_SourceSample(const AudioSpectralSource& source, std::int64_t bodyIndex) noexcept
{
	const auto storageIndex = static_cast<std::int64_t>(source.Layout.BodyOffset) + bodyIndex;
	if (storageIndex >= 0 && static_cast<std::uint64_t>(storageIndex) < source.Samples.size())
		return source.Samples[static_cast<std::size_t>(storageIndex)];
	// Prefer actual captured prefix/tail. Only missing context extends the body
	// periodically; it never replaces stored prefix samples with body-start data.
	const auto length = static_cast<std::int64_t>(source.Layout.LogicalLength);
	auto wrapped = bodyIndex % length;
	if (wrapped < 0) wrapped += length;
	return source.Samples[source.Layout.BodyOffset + static_cast<unsigned long>(wrapped)];
}

std::shared_ptr<const AudioSpectralAnalysis> AudioSpectralDsp::Analyze(
	std::shared_ptr<const AudioSpectralSource> source, const AudioSpectralTransformConfig& transform,
	std::string& reason, std::stop_token stop)
{
	reason.clear();
	if (!source || !source->Identity || source->Generation == 0u
		|| source->Samples.size() != source->Layout.StoredSampleCount())
	{
		reason = "Audio spectral source is incomplete";
		return nullptr;
	}
	const auto estimate = EstimateWorkingMemory(source->Layout, transform, reason);
	if (!estimate) return nullptr;
	try
	{
		for (std::size_t first = 0u; first < source->Samples.size(); first += 4096u)
		{
			if (stop.stop_requested()) { reason = "Audio analysis cancelled"; return nullptr; }
			const auto end = (std::min)(source->Samples.size(), first + 4096u);
			for (auto index = first; index < end; ++index)
				if (!std::isfinite(source->Samples[index]))
				{
					reason = "Audio source contains non-finite samples";
					return nullptr;
				}
		}
		AudioSpectralFft fft(transform.WindowSamps);
		if (!fft.IsValid()) { reason = "Unable to prepare the audio FFT"; return nullptr; }
		auto analysis = std::make_shared<AudioSpectralAnalysis>();
		analysis->Source = std::move(source);
		analysis->Transform = transform;
		analysis->Geometry = estimate->Geometry;
		analysis->InverseFftNormalization = fft.InverseNormalization();
		analysis->Window.resize(transform.WindowSamps);
		for (unsigned int i = 0u; i < transform.WindowSamps; ++i)
		{
			const auto window = static_cast<float>(0.5 - 0.5 * std::cos(constants::TWOPI * i / transform.WindowSamps));
			analysis->Window[i] = window;
			analysis->WindowSum += window;
		}
		analysis->Coefficients.resize(analysis->Geometry.FrameCount * analysis->Geometry.BinCount);
		std::vector<float> samples(transform.WindowSamps);
		for (std::size_t frame = 0u; frame < analysis->Geometry.FrameCount; ++frame)
		{
			if (stop.stop_requested()) { reason = "Audio analysis cancelled"; return nullptr; }
			const auto first = analysis->Geometry.FrameCentre(frame, transform.HopSamps) - transform.WindowSamps / 2u;
			for (unsigned int i = 0u; i < transform.WindowSamps; ++i)
				samples[i] = _SourceSample(*analysis->Source, first + i) * analysis->Window[i];
			fft.Forward(samples, std::span<AudioSpectralFft::Bin>(analysis->Coefficients).subspan(
				frame * analysis->Geometry.BinCount, analysis->Geometry.BinCount));
		}
		return analysis;
	}
	catch (const std::bad_alloc&) { reason = "Not enough memory for audio spectral analysis"; }
	catch (const std::exception& error) { reason = error.what(); }
	return nullptr;
}

std::shared_ptr<const AudioSpectralPcm> AudioSpectralDsp::Reconstruct(
	const std::shared_ptr<const AudioSpectralAnalysis>& analysis, const AudioSpectralGainField& gains,
	std::string& reason, std::stop_token stop, bool forceUnityRoundTrip)
{
	reason.clear();
	if (!analysis || !analysis->Source || gains.FrameCount() != analysis->Geometry.FrameCount
		|| gains.BinCount() != analysis->Geometry.BinCount)
	{
		reason = "Spectral reconstruction dimensions do not match their source";
		return nullptr;
	}
	const auto estimate = EstimateWorkingMemory(analysis->Source->Layout, analysis->Transform, reason);
	if (!estimate) return nullptr;
	const auto& expected = estimate->Geometry;
	const auto& actual = analysis->Geometry;
	if (!analysis->Source->Identity || analysis->Source->Generation == 0u
		|| analysis->Source->Samples.size() != analysis->Source->Layout.StoredSampleCount()
		|| actual.FirstFrameCentre != expected.FirstFrameCentre || actual.FrameCount != expected.FrameCount
		|| actual.BinCount != expected.BinCount || actual.TargetFirstStorage != expected.TargetFirstStorage
		|| actual.TargetLength != expected.TargetLength
		|| analysis->Window.size() != analysis->Transform.WindowSamps
		|| analysis->Coefficients.size() != actual.FrameCount * actual.BinCount
		|| !std::isfinite(analysis->WindowSum) || analysis->WindowSum <= 0.0
		|| !std::all_of(analysis->Window.begin(), analysis->Window.end(),
			[](float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; }))
	{
		reason = "Spectral analysis layout or storage is invalid";
		return nullptr;
	}
	if (stop.stop_requested()) { reason = "Audio reconstruction cancelled"; return nullptr; }
	try
	{
		auto pcm = std::make_shared<AudioSpectralPcm>();
		pcm->Source = analysis->Source;
		if (gains.IsUnity() && !forceUnityRoundTrip) return pcm;
		AudioSpectralFft fft(analysis->Transform.WindowSamps);
		if (!fft.IsValid() || fft.InverseNormalization() != analysis->InverseFftNormalization)
		{
			reason = "Audio FFT normalization does not match the analysis";
			return nullptr;
		}
		const auto& geometry = analysis->Geometry;
		auto output = std::make_shared<std::vector<float>>(analysis->Source->Samples);
		std::vector<double> sum(geometry.TargetLength, 0.0);
		std::vector<double> normalization(geometry.TargetLength, 0.0);
		std::vector<float> samples(analysis->Transform.WindowSamps);
		std::vector<AudioSpectralFft::Bin> coefficients(geometry.BinCount);
		for (std::size_t frame = 0u; frame < geometry.FrameCount; ++frame)
		{
			if (stop.stop_requested()) { reason = "Audio reconstruction cancelled"; return nullptr; }
			const auto original = analysis->Frame(frame);
			for (unsigned int bin = 0u; bin < geometry.BinCount; ++bin)
			{
				const auto db = gains.GainDb(frame, bin);
				if (!std::isfinite(db) || (db != AudioSpectralConfig::SilenceGainDb
                    && (db < AudioSpectralConfig::MinimumSpectralGainDb || db > AudioSpectralConfig::MaximumSpectralGainDb)))
				{
					reason = "Spectral gain is outside its finite bounds";
					return nullptr;
				}
				const auto gain = gains.GainLinear(frame, bin);
				// Positive scaling retains original complex direction. Zero coefficients
				// stay exactly zero; no phase division or iterative phase recovery.
				coefficients[bin] = { static_cast<float>(original[bin].Real * gain), static_cast<float>(original[bin].Imaginary * gain) };
                if (!std::isfinite(coefficients[bin].Real) || !std::isfinite(coefficients[bin].Imaginary))
                { reason = "Spectral coefficient exceeded finite float range"; return nullptr; }
			}
			coefficients.front().Imaginary = 0.0f;
			coefficients.back().Imaginary = 0.0f;
			fft.Inverse(coefficients, samples);
			const auto firstStorage = static_cast<std::int64_t>(analysis->Source->Layout.BodyOffset)
				+ geometry.FrameCentre(frame, analysis->Transform.HopSamps) - analysis->Transform.WindowSamps / 2u;
			for (unsigned int i = 0u; i < samples.size(); ++i)
			{
				const auto target = firstStorage + i - static_cast<std::int64_t>(geometry.TargetFirstStorage);
				if (target < 0 || static_cast<std::uint64_t>(target) >= geometry.TargetLength) continue;
				const auto window = analysis->Window[i];
				sum[static_cast<std::size_t>(target)] += static_cast<double>(samples[i]) * window;
				normalization[static_cast<std::size_t>(target)] += static_cast<double>(window) * window;
			}
		}
		for (unsigned long first = 0ul; first < geometry.TargetLength; first += 4096ul)
		{
			if (stop.stop_requested()) { reason = "Audio reconstruction cancelled"; return nullptr; }
			const auto end = (std::min)(geometry.TargetLength, first + 4096ul);
			for (auto index = first; index < end; ++index)
			{
				if (!(normalization[index] > 0.0) || !std::isfinite(normalization[index]))
				{
					reason = "Spectral overlap-add has uncovered samples";
					return nullptr;
				}
				const auto sample = static_cast<float>(sum[index] / normalization[index]);
				if (!std::isfinite(sample))
				{
					reason = "Spectral reconstruction exceeded finite float range";
					return nullptr;
				}
				(*output)[geometry.TargetFirstStorage + index] = sample;
			}
		}
		// Everything outside prefix fade + body is copied unchanged, including
		// unrelated physical capture tail and the earlier private prefix.
		pcm->ProcessedSamples = std::move(output);
		return pcm;
	}
	catch (const std::bad_alloc&) { reason = "Not enough memory for audio reconstruction"; }
	catch (const std::exception& error) { reason = error.what(); }
	return nullptr;
}
