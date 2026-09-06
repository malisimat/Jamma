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

	if (input.RemoteIntervalLengthSamps == 0u)
	{
		// No valid NJClient timing yet (e.g. just connected). Only report a
		// reset on the transition into this state so callers don't spam
		// Reset() every block while waiting for timing to become available.
		result.GenerationReset = state.Primed;
		state.Primed = false;
		return result;
	}

	result.Valid = true;

	const auto delayWriteCursorMod = input.DelayWriteCursorSamps % input.RemoteIntervalLengthSamps;
	const auto remoteIntervalPhaseMod = input.RemoteIntervalPhaseSamps % input.RemoteIntervalLengthSamps;

	if (!state.Primed || state.LastRemoteIntervalLengthSamps != input.RemoteIntervalLengthSamps)
	{
		result.GenerationReset = true;
	}
	else
	{
		// Compare the observed remote interval phase against the prior prediction for
		// this block (plan §3.2). An ordinary same-length wrap always
		// matches exactly; only a genuine anomaly (seek/reconnect we didn't
		// otherwise detect) produces a large circular distance.
		const auto predicted = state.PredictedRemoteIntervalPhaseSamps % input.RemoteIntervalLengthSamps;
		const auto forwardDiff = utils::ModNeg(static_cast<int>(remoteIntervalPhaseMod)
			- static_cast<int>(predicted), input.RemoteIntervalLengthSamps);
		const auto circularDiff = std::min(forwardDiff, input.RemoteIntervalLengthSamps - forwardDiff);
		const auto tolerance = std::min(input.NumFrames, input.RemoteIntervalLengthSamps);

		if (circularDiff > tolerance)
		{
			result.GenerationReset = true;
			result.AnomalyDetected = true;
		}
	}

	// DAC delay = write cursor + output latency - remote phase (mod remote length);
	// ADC delay = write cursor - input latency - remote phase (mod remote length).
	const auto dacDelay = utils::ModNeg(static_cast<int>(delayWriteCursorMod)
		+ static_cast<int>(input.OutLatencySamps) - static_cast<int>(remoteIntervalPhaseMod),
		input.RemoteIntervalLengthSamps);
	const auto adcDelay = utils::ModNeg(static_cast<int>(delayWriteCursorMod)
		- static_cast<int>(input.InLatencySamps) - static_cast<int>(remoteIntervalPhaseMod),
		input.RemoteIntervalLengthSamps);

	result.DacDelaySamps = dacDelay + input.NumFrames;
	result.AdcDelaySamps = adcDelay + input.NumFrames;

	state.Primed = true;
	state.LastRemoteIntervalLengthSamps = input.RemoteIntervalLengthSamps;
	state.PredictedRemoteIntervalPhaseSamps = (remoteIntervalPhaseMod + input.NumFrames)
		% input.RemoteIntervalLengthSamps;

	return result;
}
