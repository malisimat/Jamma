///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2026 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "NinjamMetronomeTiming.h"

#include <algorithm>
#include <cmath>

using namespace ninjam;

NinjamMetronomeTimingResult NinjamMetronomeTiming::Compute(const NinjamMetronomeTimingInput& input,
	NinjamMetronomeTimingState& state) noexcept
{
	NinjamMetronomeTimingResult result;

	if (input.intervalLengthSamps == 0u || input.bpm <= 0.0f || input.bpi == 0u
		|| input.remoteSampleRate == 0u || input.deviceSampleRate == 0u || input.numFrames == 0u)
	{
		state.primed = false;
		return result;
	}

	const auto generationChanged = !state.primed
		|| state.intervalLengthSamps != input.intervalLengthSamps
		|| state.bpm != input.bpm
		|| state.bpi != input.bpi
		|| state.remoteSampleRate != input.remoteSampleRate
		|| state.deviceSampleRate != input.deviceSampleRate;

	state.primed = true;
	state.intervalLengthSamps = input.intervalLengthSamps;
	state.bpm = input.bpm;
	state.bpi = input.bpi;
	state.remoteSampleRate = input.remoteSampleRate;
	state.deviceSampleRate = input.deviceSampleRate;

	result.valid = true;
	result.generationReset = generationChanged;

	const auto deviceToRemote = static_cast<double>(input.remoteSampleRate)
		/ static_cast<double>(input.deviceSampleRate);
	const auto phaseStart = static_cast<double>(input.intervalPositionSamps)
		+ deviceToRemote * static_cast<double>(input.outputLatencySamps);
	const auto phaseEnd = phaseStart + deviceToRemote * static_cast<double>(input.numFrames - 1u);
	const auto samplesPerBeat = static_cast<double>(input.remoteSampleRate) * 60.0
		/ static_cast<double>(input.bpm);

	if (!std::isfinite(samplesPerBeat) || samplesPerBeat <= 0.0)
	{
		result.valid = false;
		result.onsetCount = 0u;
		return result;
	}

	const auto addOnsets = [&result, phaseStart, phaseEnd, deviceToRemote](double spacing, bool accent) {
		constexpr double phaseEpsilon = 1e-9;
		auto boundary = std::ceil((phaseStart - phaseEpsilon) / spacing) * spacing;
		while (boundary <= phaseEnd + phaseEpsilon
			&& result.onsetCount < NinjamMetronomeTimingResult::MaxOnsets)
		{
			const auto offset = static_cast<unsigned int>(std::ceil((boundary - phaseStart) / deviceToRemote));
			result.onsets[result.onsetCount++] = { offset, accent };
			boundary += spacing;
		}
	};

	addOnsets(samplesPerBeat, false);
	addOnsets(static_cast<double>(input.intervalLengthSamps), true);
	std::sort(result.onsets.begin(), result.onsets.begin() + result.onsetCount,
		[](const NinjamMetronomeOnset& left, const NinjamMetronomeOnset& right) {
			return left.offset < right.offset;
		});

	return result;
}