#include "gtest/gtest.h"
#include "ninjam/ExportLaneTiming.h"
#include "utils/MathUtils.h"

using ninjam::ExportLaneTiming;

namespace
{
	// Reference (unwrapped) expectation for K, matching
	// doc/ninjam-live-loop-latency-sync-planC.md §1/§3 exactly:
	//   K_dac = n + outLatency - pos (mod L); K_adc = n - inLatency - pos (mod L)
	unsigned int ExpectedDacDelay(unsigned int n, unsigned int outLatency, unsigned int pos, unsigned int length, unsigned int numFrames)
	{
		return utils::ModNeg(static_cast<int>(n) + static_cast<int>(outLatency) - static_cast<int>(pos), length) + numFrames;
	}

	unsigned int ExpectedAdcDelay(unsigned int n, unsigned int inLatency, unsigned int pos, unsigned int length, unsigned int numFrames)
	{
		return utils::ModNeg(static_cast<int>(n) - static_cast<int>(inLatency) - static_cast<int>(pos), length) + numFrames;
	}
}

// Test 1 (planC §5): absolute phase, steady state. A non-wrapping sequence of
// blocks with non-zero P/latencies must produce exactly (P+i) mod L (via the
// K_dac/K_adc formula) on every block, and the very first block should be
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
		input.n = n;
		input.numFrames = numFrames;
		input.pos = pos;
		input.length = length;
		input.inLatencySamps = inLatency;
		input.outLatencySamps = outLatency;

		const auto result = ExportLaneTiming::Compute(input, state);

		EXPECT_TRUE(result.valid);
		EXPECT_FALSE(result.anomalyDetected);
		EXPECT_EQ(result.generationReset, block == 0) << "block=" << block;
		EXPECT_EQ(result.dacDelaySamps, ExpectedDacDelay(n, outLatency, pos, length, numFrames)) << "block=" << block;
		EXPECT_EQ(result.adcDelaySamps, ExpectedAdcDelay(n, inLatency, pos, length, numFrames)) << "block=" << block;

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
	input.n = n;
	input.numFrames = numFrames;
	input.pos = pos;
	input.length = length;

	auto result = ExportLaneTiming::Compute(input, state);
	ASSERT_TRUE(result.generationReset); // first block only

	n += numFrames;
	pos = (pos + numFrames) % length; // = 2

	input.n = n;
	input.pos = pos;
	result = ExportLaneTiming::Compute(input, state);

	EXPECT_FALSE(result.generationReset);
	EXPECT_FALSE(result.anomalyDetected);
	EXPECT_EQ(result.dacDelaySamps, ExpectedDacDelay(n, 0u, pos, length, numFrames));
}

// Test 4 (planC §5): an interval-length change (tempo/BPI vote) always
// triggers a generation reset, without being flagged as an anomaly.
TEST(ExportLaneTiming, LengthChangeTriggersResetWithoutAnomaly)
{
	const unsigned int numFrames = 4u;
	ninjam::ExportLaneTimingState state;

	ninjam::ExportLaneTimingInput input;
	input.n = 0u;
	input.numFrames = numFrames;
	input.pos = 0u;
	input.length = 16u;
	auto result = ExportLaneTiming::Compute(input, state);
	ASSERT_TRUE(result.generationReset);
	ASSERT_FALSE(result.anomalyDetected);

	input.n = numFrames;
	input.pos = numFrames;
	result = ExportLaneTiming::Compute(input, state);
	ASSERT_FALSE(result.generationReset);

	// BPI/BPM vote changes the interval length.
	input.n += numFrames;
	input.pos = (input.pos + numFrames);
	input.length = 24u;
	result = ExportLaneTiming::Compute(input, state);

	EXPECT_TRUE(result.generationReset);
	EXPECT_FALSE(result.anomalyDetected);
}

// Test 5 (planC §5): an anomalous position jump (unrelated to a length
// change) is detected, triggers a reset, and is distinguished from an
// ordinary generation reset via anomalyDetected.
TEST(ExportLaneTiming, AnomalousPositionJumpDetected)
{
	const unsigned int length = 100u;
	const unsigned int numFrames = 8u;
	ninjam::ExportLaneTimingState state;

	ninjam::ExportLaneTimingInput input;
	input.n = 0u;
	input.numFrames = numFrames;
	input.pos = 10u;
	input.length = length;
	auto result = ExportLaneTiming::Compute(input, state);
	ASSERT_TRUE(result.generationReset);

	// Prime one more ordinary block so a prediction exists.
	input.n += numFrames;
	input.pos = (input.pos + numFrames) % length; // 18
	result = ExportLaneTiming::Compute(input, state);
	ASSERT_FALSE(result.generationReset);

	// Now jump pos far away from the predicted value (18 + 8 = 26), with the
	// same length — simulates an unexpected seek/reconnect.
	input.n += numFrames;
	input.pos = 70u;
	result = ExportLaneTiming::Compute(input, state);

	EXPECT_TRUE(result.generationReset);
	EXPECT_TRUE(result.anomalyDetected);
}

// No NJClient timing yet (length == 0): Compute() must not claim valid K
// values, and must only report a reset on the primed->unprimed transition
// (not every block) so callers don't spam Reset() while waiting to connect.
TEST(ExportLaneTiming, NoTimingYetIsInvalidNotSpammedAsReset)
{
	ninjam::ExportLaneTimingState state;

	ninjam::ExportLaneTimingInput input;
	input.n = 0u;
	input.numFrames = 4u;
	input.pos = 0u;
	input.length = 0u;

	auto result = ExportLaneTiming::Compute(input, state);
	EXPECT_FALSE(result.valid);
	EXPECT_FALSE(result.generationReset); // was already unprimed

	result = ExportLaneTiming::Compute(input, state);
	EXPECT_FALSE(result.valid);
	EXPECT_FALSE(result.generationReset);
}

// Bounded wrap-block behaviour (planC §5 test 3): the block whose freshly
// read pos already reflects a wrap that completed inside the prior
// AudioProc call still gets an exactly-correct K for its own first sample,
// and the following block is exactly correct again with zero residual
// error -- i.e. no persistent drift is introduced by a wrap.
TEST(ExportLaneTiming, WrapBlockExactThenNoResidualDrift)
{
	const unsigned int length = 12u;
	const unsigned int numFrames = 5u;
	ninjam::ExportLaneTimingState state;

	ninjam::ExportLaneTimingInput input;
	input.n = 0u;
	input.numFrames = numFrames;
	input.pos = 9u;
	input.length = length;
	ExportLaneTiming::Compute(input, state); // prime

	// This block's pos already reflects the wrap (9 -> 2 after 5 samples).
	input.n = numFrames;
	input.pos = (9u + numFrames) % length; // 2
	auto wrapResult = ExportLaneTiming::Compute(input, state);
	EXPECT_FALSE(wrapResult.generationReset);
	EXPECT_EQ(wrapResult.dacDelaySamps, ExpectedDacDelay(input.n, 0u, input.pos, length, numFrames));

	// Next block: no drift, still exact.
	input.n += numFrames;
	input.pos = (input.pos + numFrames) % length;
	auto nextResult = ExportLaneTiming::Compute(input, state);
	EXPECT_FALSE(nextResult.generationReset);
	EXPECT_FALSE(nextResult.anomalyDetected);
	EXPECT_EQ(nextResult.dacDelaySamps, ExpectedDacDelay(input.n, 0u, input.pos, length, numFrames));
}
