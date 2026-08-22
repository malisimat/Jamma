///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2026 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "NinjamMetronomeTiming.h"

#include <algorithm>
#include <cstdint>

using namespace ninjam;

NinjamMetronomeTimingResult NinjamMetronomeTiming::Compute(const NinjamMetronomeTimingInput& input,
	NinjamMetronomeTimingState& state) noexcept
{
	NinjamMetronomeTimingResult result;

	if (input.intervalLengthSamps == 0u || input.bpi == 0u || input.numFrames == 0u)
	{
		state.primed = false;
		return result;
	}

	const auto generationChanged = !state.primed
		|| state.intervalLengthSamps != input.intervalLengthSamps
		|| state.bpi != input.bpi;

	state.primed = true;
	state.intervalLengthSamps = input.intervalLengthSamps;
	state.bpm = input.bpm;
	state.bpi = input.bpi;
	state.deviceSampleRate = input.deviceSampleRate;

	result.valid = true;
	result.generationReset = generationChanged;

	// The interval is the authoritative remote timing geometry applied to local
	// loops.  Derive every beat from it rather than from the separately reported
	// BPM, which can briefly be stale while the server applies a tempo change.
	const auto phaseStart = static_cast<std::uint64_t>(input.intervalPositionSamps)
		+ input.outputLatencySamps;
	const auto phaseEnd = phaseStart + input.numFrames - 1u;
	const auto intervalLength = static_cast<std::uint64_t>(input.intervalLengthSamps);
	const auto bpi = static_cast<std::uint64_t>(input.bpi);

	const auto addBeatOnsets = [&result, phaseStart, phaseEnd, intervalLength, bpi]() {
		auto beat = (phaseStart * bpi + intervalLength - 1u) / intervalLength;
		while (result.onsetCount < NinjamMetronomeTimingResult::MaxOnsets)
		{
			const auto boundary = (beat * intervalLength) / bpi;
			if (boundary > phaseEnd)
				break;
			result.onsets[result.onsetCount++] = {
				static_cast<unsigned int>(boundary - phaseStart), false };
			++beat;
		}
	};
	const auto addIntervalAccents = [&result, phaseStart, phaseEnd, intervalLength]() {
		auto interval = (phaseStart + intervalLength - 1u) / intervalLength;
		while (result.onsetCount < NinjamMetronomeTimingResult::MaxOnsets)
		{
			const auto boundary = interval * intervalLength;
			if (boundary > phaseEnd)
				break;
			result.onsets[result.onsetCount++] = {
				static_cast<unsigned int>(boundary - phaseStart), true };
			++interval;
		}
	};

	addBeatOnsets();
	addIntervalAccents();
	std::sort(result.onsets.begin(), result.onsets.begin() + result.onsetCount,
		[](const NinjamMetronomeOnset& left, const NinjamMetronomeOnset& right) {
			return left.offset < right.offset;
		});

	return result;
}
