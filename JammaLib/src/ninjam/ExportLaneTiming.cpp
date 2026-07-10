///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2026 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "ExportLaneTiming.h"

#include <algorithm>
#include "../utils/MathUtils.h"

using namespace ninjam;

ExportLaneTimingResult ExportLaneTiming::Compute(const ExportLaneTimingInput& input,
	ExportLaneTimingState& state)
{
	ExportLaneTimingResult result{};

	if (input.length == 0u)
	{
		// No valid NJClient timing yet (e.g. just connected). Only report a
		// reset on the transition into this state so callers don't spam
		// Reset() every block while waiting for timing to become available.
		result.generationReset = state.primed;
		state.primed = false;
		return result;
	}

	result.valid = true;

	const auto nMod = input.n % input.length;
	const auto posMod = input.pos % input.length;

	if (!state.primed || state.lastLength != input.length)
	{
		result.generationReset = true;
	}
	else
	{
		// Compare the observed pos against what last block predicted for
		// this block (plan §3.2). An ordinary same-length wrap always
		// matches exactly; only a genuine anomaly (seek/reconnect we didn't
		// otherwise detect) produces a large circular distance.
		const auto predicted = state.predictedPos % input.length;
		const auto forwardDiff = utils::ModNeg(static_cast<int>(posMod) - static_cast<int>(predicted), input.length);
		const auto circularDiff = std::min(forwardDiff, input.length - forwardDiff);
		const auto tolerance = std::min(input.numFrames, input.length);

		if (circularDiff > tolerance)
		{
			result.generationReset = true;
			result.anomalyDetected = true;
		}
	}

	// K_dac = n + outLatency - pos (mod L); K_adc = n - inLatency - pos (mod L)
	const auto kDac = utils::ModNeg(static_cast<int>(nMod) + static_cast<int>(input.outLatencySamps) - static_cast<int>(posMod), input.length);
	const auto kAdc = utils::ModNeg(static_cast<int>(nMod) - static_cast<int>(input.inLatencySamps) - static_cast<int>(posMod), input.length);

	result.dacDelaySamps = kDac + input.numFrames;
	result.adcDelaySamps = kAdc + input.numFrames;

	state.primed = true;
	state.lastLength = input.length;
	state.predictedPos = (posMod + input.numFrames) % input.length;

	return result;
}
