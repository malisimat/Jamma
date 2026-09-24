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

	if (input.IntervalLengthSamps == 0u || input.Bpi == 0u || input.NumFrames == 0u)
	{
		state.Primed = false;
		return result;
	}

	const auto generationChanged = !state.Primed
		|| state.IntervalLengthSamps != input.IntervalLengthSamps
		|| state.Bpi != input.Bpi;

	state.Primed = true;
	state.IntervalLengthSamps = input.IntervalLengthSamps;
	state.Bpm = input.Bpm;
	state.Bpi = input.Bpi;
	state.DeviceSampleRate = input.DeviceSampleRate;

	result.Valid = true;
	result.GenerationReset = generationChanged;

	// The interval is the authoritative remote timing geometry applied to local
	// loops.  Derive every beat from it rather than from the separately reported
	// BPM, which can briefly be stale while the server applies a tempo change.
	const auto phaseStart = static_cast<std::uint64_t>(input.IntervalPositionSamps)
		+ input.OutputLatencySamps;
	const auto phaseEnd = phaseStart + input.NumFrames - 1u;
	const auto intervalLength = static_cast<std::uint64_t>(input.IntervalLengthSamps);
	const auto bpi = static_cast<std::uint64_t>(input.Bpi);

	const auto addBeatOnsets = [&result, phaseStart, phaseEnd, intervalLength, bpi]() {
		auto beat = (phaseStart * bpi + intervalLength - 1u) / intervalLength;
		while (result.OnsetCount < NinjamMetronomeTimingResult::MaxOnsets)
		{
			const auto boundary = (beat * intervalLength) / bpi;
			if (boundary > phaseEnd)
				break;
			result.Onsets[result.OnsetCount++] = {
				static_cast<unsigned int>(boundary - phaseStart), false };
			++beat;
		}
	};
	const auto addIntervalAccents = [&result, phaseStart, phaseEnd, intervalLength]() {
		auto interval = (phaseStart + intervalLength - 1u) / intervalLength;
		while (result.OnsetCount < NinjamMetronomeTimingResult::MaxOnsets)
		{
			const auto boundary = interval * intervalLength;
			if (boundary > phaseEnd)
				break;
			result.Onsets[result.OnsetCount++] = {
				static_cast<unsigned int>(boundary - phaseStart), true };
			++interval;
		}
	};

	addBeatOnsets();
	addIntervalAccents();
	std::sort(result.Onsets.begin(), result.Onsets.begin() + result.OnsetCount,
		[](const NinjamMetronomeOnset& left, const NinjamMetronomeOnset& right) {
			return left.Offset < right.Offset;
		});

	return result;
}
