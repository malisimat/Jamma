#include "AudioSpectralEnvelope.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

using namespace audio;

AudioSpectralEnvelope::AudioSpectralEnvelope(std::vector<float> gains, bool unity) :
	_gains(std::move(gains)), _unity(unity)
{
}

float AudioSpectralEnvelope::MaximumGain() noexcept
{
	return static_cast<float>(std::pow(10.0, AudioSpectralConfig::MaximumGainDb / 20.0));
}

float AudioSpectralEnvelope::TargetFromDragPixels(double displacement) noexcept
{
	if (!std::isfinite(displacement)) return 1.0f;
	if (displacement == 0.0) return 1.0f;
	if (displacement <= -AudioSpectralConfig::BarSilenceDragPixels) return 0.0f;
	if (displacement < 0.0)
		return static_cast<float>(1.0 + displacement / AudioSpectralConfig::BarSilenceDragPixels);
	const auto fraction = (std::min)(displacement / AudioSpectralConfig::BarBoostDragPixels, 1.0);
	return static_cast<float>(std::pow(10.0, AudioSpectralConfig::MaximumGainDb * fraction / 20.0));
}

std::shared_ptr<const AudioSpectralEnvelope> AudioSpectralEnvelope::Unity(unsigned long length, std::string& reason)
{
	reason.clear();
	if (length == 0ul || static_cast<std::uint64_t>(length) * sizeof(float) > AudioSpectralConfig::MaxWorkingBytes)
	{
		reason = "Broadband envelope exceeds the editor memory budget or has no length";
		return nullptr;
	}
	try
	{
		return std::shared_ptr<const AudioSpectralEnvelope>(new AudioSpectralEnvelope(std::vector<float>(length, 1.0f), true));
	}
	catch (const std::bad_alloc&)
	{
		reason = "Not enough memory for the broadband envelope";
		return nullptr;
	}
}

std::shared_ptr<const AudioSpectralEnvelope> AudioSpectralEnvelope::FromGains(std::span<const float> gains, std::string& reason)
{
	reason.clear();
	if (gains.empty() || gains.size() > std::numeric_limits<unsigned long>::max()
		|| gains.size() > AudioSpectralConfig::MaxWorkingBytes / sizeof(float))
	{
		reason = "Broadband envelope exceeds the editor memory budget or has no length";
		return nullptr;
	}
	const auto maximum = MaximumGain();
	bool unity = true;
	for (const auto gain : gains)
	{
		if (!std::isfinite(gain) || gain < 0.0f || gain > maximum)
		{
			reason = "Broadband envelope contains an invalid gain";
			return nullptr;
		}
		unity &= gain == 1.0f;
	}
	try
	{
		return std::shared_ptr<const AudioSpectralEnvelope>(new AudioSpectralEnvelope(
			std::vector<float>(gains.begin(), gains.end()), unity));
	}
	catch (const std::bad_alloc&)
	{
		reason = "Not enough memory for the broadband envelope";
		return nullptr;
	}
}

AudioSpectralEnvelopeStroke::AudioSpectralEnvelopeStroke(
	std::shared_ptr<const AudioSpectralEnvelope> before, double radiusSamps, double rampHalfSamps) :
	_before(std::move(before)), _gains(_before->Gains().begin(), _before->Gains().end()),
	_bestInfluence(_before->Length(), 0.0), _winnerIds(_before->Length(), 0u),
	_winningTargets(_before->Length(), 1.0f), _radiusSamps(radiusSamps), _rampHalfSamps(rampHalfSamps)
{
}

std::unique_ptr<AudioSpectralEnvelopeStroke> AudioSpectralEnvelopeStroke::Begin(
	std::shared_ptr<const AudioSpectralEnvelope> before, float sampleRate, std::string& reason)
{
	reason.clear();
	// Count E0, raw gains, influence, winner IDs/targets and one moved snapshot.
	constexpr auto bytesPerSample = 4u * sizeof(float) + sizeof(double) + sizeof(std::uint64_t);
	if (!before || !std::isfinite(sampleRate) || sampleRate <= 0.0f
		|| static_cast<std::uint64_t>(before->Length()) * bytesPerSample > AudioSpectralConfig::MaxWorkingBytes)
	{
		reason = "Broadband stroke has invalid geometry or exceeds the editor memory budget";
		return nullptr;
	}
	try
	{
		return std::unique_ptr<AudioSpectralEnvelopeStroke>(new AudioSpectralEnvelopeStroke(
			std::move(before), sampleRate * AudioSpectralConfig::BarTimeRadiusSeconds,
			sampleRate * AudioSpectralConfig::EnvelopeRampSeconds * 0.5));
	}
	catch (const std::bad_alloc&)
	{
		reason = "Not enough memory for broadband stroke scratch";
		return nullptr;
	}
}

unsigned long AudioSpectralEnvelopeStroke::_WrapSample(double integralSample, unsigned long length) noexcept
{
	auto wrapped = std::fmod(integralSample, static_cast<double>(length));
	if (wrapped < 0.0) wrapped += length;
	return static_cast<unsigned long>(wrapped);
}

bool AudioSpectralEnvelopeStroke::PaintSegment(double startSample, float startGain,
	double endSample, float endGain) noexcept
{
	// Keep integer sample locations exactly representable across the supported
	// unwrapped path range; invalid input leaves the entire stroke untouched.
	constexpr double maximumCoordinate = 0x1p50;
	const auto maximumGain = AudioSpectralEnvelope::MaximumGain();
	if (!std::isfinite(startSample) || !std::isfinite(endSample)
		|| std::abs(startSample) > maximumCoordinate || std::abs(endSample) > maximumCoordinate
		|| !std::isfinite(startGain) || !std::isfinite(endGain)
		|| startGain < 0.0f || endGain < 0.0f || startGain > maximumGain || endGain > maximumGain)
		return false;
	if (_winnerSerial == std::numeric_limits<std::uint64_t>::max()) return false;
	++_winnerSerial;
	const auto length = _before->Length();
	const auto first = std::ceil((std::min)(startSample, endSample) - _radiusSamps);
	const auto last = std::floor((std::max)(startSample, endSample) + _radiusSamps);
	const auto count = last - first + 1.0;
	if (count >= length)
	{
		for (unsigned long i = 0ul; i < length; ++i)
			_PaintSample(i, startSample, startGain, endSample, endGain);
	}
	else
	{
		for (double i = first; i <= last; i += 1.0)
			_PaintSample(_WrapSample(i, length), startSample, startGain, endSample, endGain);
	}
	return true;
}

void AudioSpectralEnvelopeStroke::_PaintSample(unsigned long index, double startSample,
	float startGain, double endSample, float endGain) noexcept
{
	const auto length = static_cast<double>(_before->Length());
	const auto low = (std::min)(startSample, endSample);
	const auto high = (std::max)(startSample, endSample);
	const auto firstTurn = std::ceil((low - index) / length);
	const auto lastTurn = std::floor((high - index) / length);
	double sample;
	double centre;
	if (firstTurn <= lastTurn)
	{
		// Multiple full-strength visits in one swept segment: its later visit wins.
		const auto turn = endSample >= startSample ? lastTurn : firstTurn;
		sample = index + turn * length;
		centre = sample;
	}
	else
	{
		const auto below = index + std::floor((low - index) / length) * length;
		const auto above = below + length;
		const auto lowerDistance = low - below;
		const auto upperDistance = above - high;
		// For equidistant copies, choose the later point along the authored path.
		const bool chooseBelow = lowerDistance < upperDistance
			|| (lowerDistance == upperDistance && endSample < startSample);
		sample = chooseBelow ? below : above;
		centre = chooseBelow ? low : high;
	}
	const auto distance = std::abs(sample - centre);
	if (distance >= _radiusSamps) return;
	const auto fraction = distance / _radiusSamps;
	const auto falloff = 1.0 - fraction * fraction;
	const auto influence = falloff * falloff;
	// Exact double comparison is the tie convention: the same geometric distance
	// evaluates identically. No epsilon permits a weaker tail to erode authority.
	if (influence < _bestInfluence[index]) return;
	const auto alpha = startSample == endSample ? 1.0
		: std::clamp((centre - startSample) / (endSample - startSample), 0.0, 1.0);
	const auto target = alpha == 0.0 ? startGain : alpha == 1.0 ? endGain
		: static_cast<float>(startGain + alpha * (static_cast<double>(endGain) - startGain));
	const auto baseline = _before->GainAt(index);
	const auto candidate = influence == 1.0 ? target
		: static_cast<float>(baseline + influence * (static_cast<double>(target) - baseline));
	const bool wasChanged = _gains[index] != baseline;
	const bool isChanged = candidate != baseline;
	if (wasChanged != isChanged)
	{
		if (isChanged) ++_changedSamples;
		else --_changedSamples;
	}
	_gains[index] = candidate;
	_bestInfluence[index] = influence;
	_winnerIds[index] = _winnerSerial;
	_winningTargets[index] = target;
}

std::shared_ptr<const AudioSpectralEnvelope> AudioSpectralEnvelopeStroke::Snapshot(std::string& reason) const
{
	if (!HasChanges())
	{
		reason.clear();
		return _before;
	}
	reason.clear();
	try
	{
		auto gains = _gains; // Always start from raw E0-based candidates.
		_ApplyLocalRamps(gains);
		if (std::equal(gains.begin(), gains.end(), _before->Gains().begin())) return _before;
		const auto unity = std::all_of(gains.begin(), gains.end(), [](float gain) { return gain == 1.0f; });
		return std::shared_ptr<const AudioSpectralEnvelope>(new AudioSpectralEnvelope(std::move(gains), unity));
	}
	catch (const std::bad_alloc&)
	{
		reason = "Not enough memory to publish the broadband envelope";
		return nullptr;
	}
}

bool AudioSpectralEnvelopeStroke::_IsRampBoundary(unsigned long leftIndex) const noexcept
{
	const auto rightIndex = (leftIndex + 1ul) % _before->Length();
	return _winnerIds[leftIndex] != 0u && _winnerIds[rightIndex] != 0u
		&& _winnerIds[leftIndex] != _winnerIds[rightIndex]
		&& _winningTargets[leftIndex] != _winningTargets[rightIndex]
		&& _gains[leftIndex] != _gains[rightIndex];
}

void AudioSpectralEnvelopeStroke::_ApplyLocalRamps(std::vector<float>& gains) const noexcept
{
	const auto length = _before->Length();
	if (length < 2ul) return;
	const auto limit = static_cast<unsigned long>((std::min)(std::floor(_rampHalfSamps),
		static_cast<double>((length - 1ul) / 2ul)));
	if (limit == 0ul) return;
	for (unsigned long boundary = 0ul; boundary < length; ++boundary)
	{
		if (!_IsRampBoundary(boundary)) continue;
		const auto right = (boundary + 1ul) % length;
		unsigned long leftSteps = 0ul;
		unsigned long rightSteps = 0ul;
		auto leftEnd = boundary;
		auto rightEnd = right;
		auto leftLimit = limit;
		auto rightLimit = limit;
		// Divide the space between competing boundaries so finite local ramps
		// cannot overlap; each uses only raw candidates as endpoint values.
		for (unsigned long distance = 1ul; distance <= 2ul * limit; ++distance)
		{
			if (_IsRampBoundary((boundary + length - distance) % length))
			{
				leftLimit = (std::min)(limit, (distance - 1ul) / 2ul);
				break;
			}
		}
		for (unsigned long distance = 1ul; distance <= 2ul * limit; ++distance)
		{
			if (_IsRampBoundary((boundary + distance) % length))
			{
				rightLimit = (std::min)(limit, (distance - 1ul) / 2ul);
				break;
			}
		}
		// Stop at protected values and other winner boundaries. Endpoints keep
		// their raw value, so neighbouring ramps do not overwrite one another.
		while (leftSteps < leftLimit && _bestInfluence[leftEnd] > 0.0 && _bestInfluence[leftEnd] < 1.0)
		{
			const auto previous = (leftEnd + length - 1ul) % length;
			if (_IsRampBoundary(previous)) break;
			leftEnd = previous;
			++leftSteps;
		}
		while (rightSteps < rightLimit && _bestInfluence[rightEnd] > 0.0 && _bestInfluence[rightEnd] < 1.0)
		{
			if (_IsRampBoundary(rightEnd)) break;
			rightEnd = (rightEnd + 1ul) % length;
			++rightSteps;
		}
		const auto intervals = leftSteps + rightSteps + 1ul;
		if (intervals <= 1ul) continue;
		const auto leftGain = _gains[leftEnd];
		const auto rightGain = _gains[rightEnd];
		for (unsigned long step = 1ul; step < intervals; ++step)
		{
			const auto index = (leftEnd + step) % length;
			if (_bestInfluence[index] <= 0.0 || _bestInfluence[index] >= 1.0) continue;
			const auto fraction = static_cast<double>(step) / intervals;
			gains[index] = leftGain == rightGain ? leftGain
				: static_cast<float>(leftGain + fraction * (static_cast<double>(rightGain) - leftGain));
		}
	}
}
