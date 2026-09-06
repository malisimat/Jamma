#include "gtest/gtest.h"
#include "ninjam/ExportLaneTiming.h"
#include "utils/MathUtils.h"

using ninjam::ExportLaneTiming;

namespace
{
	// Reference (unwrapped) delay expectations, matching
	// doc/ninjam-live-loop-latency-sync-planC.md §1/§3 exactly:
	// DAC = write cursor + output latency - remote phase (mod remote length);
	// ADC = write cursor - input latency - remote phase (mod remote length).
	unsigned int ExpectedDacDelay(unsigned int delayWriteCursorSamps, unsigned int outLatencySamps,
		unsigned int remoteIntervalPhaseSamps, unsigned int remoteIntervalLengthSamps, unsigned int numFrames)
	{
		return utils::ModNeg(static_cast<int>(delayWriteCursorSamps) + static_cast<int>(outLatencySamps)
			- static_cast<int>(remoteIntervalPhaseSamps), remoteIntervalLengthSamps) + numFrames;
	}

	unsigned int ExpectedAdcDelay(unsigned int delayWriteCursorSamps, unsigned int inLatencySamps,
		unsigned int remoteIntervalPhaseSamps, unsigned int remoteIntervalLengthSamps, unsigned int numFrames)
	{
		return utils::ModNeg(static_cast<int>(delayWriteCursorSamps) - static_cast<int>(inLatencySamps)
			- static_cast<int>(remoteIntervalPhaseSamps), remoteIntervalLengthSamps) + numFrames;
	}
}

// Test 1 (planC §5): absolute phase, steady state. A non-wrapping sequence of
// blocks with non-zero P/latencies must produce exactly (P+i) mod L (via the
// semantic delay formula on every block, and the very first block should be
// reported as a (harmless, mute-until-primed) generation reset only.
TEST(ExportLaneTiming, SteadyStateMatchesFormulaExactly)
{
	const unsigned int length = 17u;
	const unsigned int numFrames = 4u;
	const unsigned int outLatency = 3u;
	const unsigned int inLatency = 2u;
	const unsigned int startPos = 5u;

	ninjam::ExportLaneTimingState state;
	unsigned int n = 0u;
	unsigned int pos = startPos;

	for (auto block = 0; block < 6; block++)
	{
		ninjam::ExportLaneTimingInput input;
		input.DelayWriteCursorSamps = n;
		input.NumFrames = numFrames;
		input.RemoteIntervalPhaseSamps = pos;
		input.RemoteIntervalLengthSamps = length;
		input.InLatencySamps = inLatency;
		input.OutLatencySamps = outLatency;

		const auto result = ExportLaneTiming::Compute(input, state);

		EXPECT_TRUE(result.Valid);
		EXPECT_FALSE(result.AnomalyDetected);
		EXPECT_EQ(result.GenerationReset, block == 0) << "block=" << block;
		EXPECT_EQ(result.DacDelaySamps, ExpectedDacDelay(n, outLatency, pos, length, numFrames)) << "block=" << block;
		EXPECT_EQ(result.AdcDelaySamps, ExpectedAdcDelay(n, inLatency, pos, length, numFrames)) << "block=" << block;

		n += numFrames;
		pos = (pos + numFrames) % length;
	}
}

// Test 4 (planC §5): a same-length interval wrap is NOT a generation reset —
// only the very first block is. Confirms Compute() tracks the wrap silently
// via the mod-L formula rather than treating it as an anomaly.
TEST(ExportLaneTiming, OrdinaryWrapDoesNotReset)
{
	const unsigned int length = 10u;
	const unsigned int numFrames = 4u;

	ninjam::ExportLaneTimingState state;
	unsigned int n = 0u;
	unsigned int pos = 8u; // will wrap 8 -> 2 after one block of 4

	ninjam::ExportLaneTimingInput input;
	input.DelayWriteCursorSamps = n;
	input.NumFrames = numFrames;
	input.RemoteIntervalPhaseSamps = pos;
	input.RemoteIntervalLengthSamps = length;

	auto result = ExportLaneTiming::Compute(input, state);
	ASSERT_TRUE(result.GenerationReset); // first block only

	n += numFrames;
	pos = (pos + numFrames) % length; // = 2

	input.DelayWriteCursorSamps = n;
	input.RemoteIntervalPhaseSamps = pos;
	result = ExportLaneTiming::Compute(input, state);

	EXPECT_FALSE(result.GenerationReset);
	EXPECT_FALSE(result.AnomalyDetected);
	EXPECT_EQ(result.DacDelaySamps, ExpectedDacDelay(n, 0u, pos, length, numFrames));
}

// Test 4 (planC §5): an interval-length change (tempo/BPI vote) always
// triggers a generation reset, without being flagged as an anomaly.
TEST(ExportLaneTiming, LengthChangeTriggersResetWithoutAnomaly)
{
	const unsigned int numFrames = 4u;
	ninjam::ExportLaneTimingState state;

	ninjam::ExportLaneTimingInput input;
	input.DelayWriteCursorSamps = 0u;
	input.NumFrames = numFrames;
	input.RemoteIntervalPhaseSamps = 0u;
	input.RemoteIntervalLengthSamps = 16u;
	auto result = ExportLaneTiming::Compute(input, state);
	ASSERT_TRUE(result.GenerationReset);
	ASSERT_FALSE(result.AnomalyDetected);

	input.DelayWriteCursorSamps = numFrames;
	input.RemoteIntervalPhaseSamps = numFrames;
	result = ExportLaneTiming::Compute(input, state);
	ASSERT_FALSE(result.GenerationReset);

	// BPI/BPM vote changes the interval length.
	input.DelayWriteCursorSamps += numFrames;
	input.RemoteIntervalPhaseSamps = (input.RemoteIntervalPhaseSamps + numFrames);
	input.RemoteIntervalLengthSamps = 24u;
	result = ExportLaneTiming::Compute(input, state);

	EXPECT_TRUE(result.GenerationReset);
	EXPECT_FALSE(result.AnomalyDetected);
}

// Test 5 (planC §5): an anomalous position jump (unrelated to a length
// change) is detected, triggers a reset, and is distinguished from an
// ordinary generation reset via AnomalyDetected.
TEST(ExportLaneTiming, AnomalousPositionJumpDetected)
{
	const unsigned int length = 100u;
	const unsigned int numFrames = 8u;
	ninjam::ExportLaneTimingState state;

	ninjam::ExportLaneTimingInput input;
	input.DelayWriteCursorSamps = 0u;
	input.NumFrames = numFrames;
	input.RemoteIntervalPhaseSamps = 10u;
	input.RemoteIntervalLengthSamps = length;
	auto result = ExportLaneTiming::Compute(input, state);
	ASSERT_TRUE(result.GenerationReset);

	// Prime one more ordinary block so a prediction exists.
	input.DelayWriteCursorSamps += numFrames;
	input.RemoteIntervalPhaseSamps = (input.RemoteIntervalPhaseSamps + numFrames) % length; // 18
	result = ExportLaneTiming::Compute(input, state);
	ASSERT_FALSE(result.GenerationReset);

	// Now jump the remote phase far from the predicted value (18 + 8 = 26), with the
	// same length — simulates an unexpected seek/reconnect.
	input.DelayWriteCursorSamps += numFrames;
	input.RemoteIntervalPhaseSamps = 70u;
	result = ExportLaneTiming::Compute(input, state);

	EXPECT_TRUE(result.GenerationReset);
	EXPECT_TRUE(result.AnomalyDetected);
}

// No NJClient timing yet (remote interval length == 0): Compute() must not claim valid delay
// values, and must only report a reset on the primed->unprimed transition
// (not every block) so callers don't spam Reset() while waiting to connect.
TEST(ExportLaneTiming, NoTimingYetIsInvalidNotSpammedAsReset)
{
	ninjam::ExportLaneTimingState state;

	ninjam::ExportLaneTimingInput input;
	input.DelayWriteCursorSamps = 0u;
	input.NumFrames = 4u;
	input.RemoteIntervalPhaseSamps = 0u;
	input.RemoteIntervalLengthSamps = 0u;

	auto result = ExportLaneTiming::Compute(input, state);
	EXPECT_FALSE(result.Valid);
	EXPECT_FALSE(result.GenerationReset); // was already unprimed

	result = ExportLaneTiming::Compute(input, state);
	EXPECT_FALSE(result.Valid);
	EXPECT_FALSE(result.GenerationReset);
}

// Bounded wrap-block behaviour (planC §5 test 3): the block whose freshly
// read remote phase already reflects a wrap that completed inside the prior
// AudioProc call still gets an exactly-correct delay for its own first sample,
// and the following block is exactly correct again with zero residual
// error -- i.e. no persistent drift is introduced by a wrap.
TEST(ExportLaneTiming, WrapBlockExactThenNoResidualDrift)
{
	const unsigned int length = 12u;
	const unsigned int numFrames = 5u;
	ninjam::ExportLaneTimingState state;

	ninjam::ExportLaneTimingInput input;
	input.DelayWriteCursorSamps = 0u;
	input.NumFrames = numFrames;
	input.RemoteIntervalPhaseSamps = 9u;
	input.RemoteIntervalLengthSamps = length;
	ExportLaneTiming::Compute(input, state); // prime

	// This block's remote phase already reflects the wrap (9 -> 2 after 5 samples).
	input.DelayWriteCursorSamps = numFrames;
	input.RemoteIntervalPhaseSamps = (9u + numFrames) % length; // 2
	auto wrapResult = ExportLaneTiming::Compute(input, state);
	EXPECT_FALSE(wrapResult.GenerationReset);
	EXPECT_EQ(wrapResult.DacDelaySamps, ExpectedDacDelay(input.DelayWriteCursorSamps, 0u,
		input.RemoteIntervalPhaseSamps, length, numFrames));

	// Next block: no drift, still exact.
	input.DelayWriteCursorSamps += numFrames;
	input.RemoteIntervalPhaseSamps = (input.RemoteIntervalPhaseSamps + numFrames) % length;
	auto nextResult = ExportLaneTiming::Compute(input, state);
	EXPECT_FALSE(nextResult.GenerationReset);
	EXPECT_FALSE(nextResult.AnomalyDetected);
	EXPECT_EQ(nextResult.DacDelaySamps, ExpectedDacDelay(input.DelayWriteCursorSamps, 0u,
		input.RemoteIntervalPhaseSamps, length, numFrames));
}
