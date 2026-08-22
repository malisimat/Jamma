#include "gtest/gtest.h"
#include "ninjam/NinjamConnection.h"
#include "ninjam/NinjamMetronomeTiming.h"

TEST(NinjamTempoCommand, PreservesUsefulFractionalBpmPrecision)
{
	EXPECT_EQ("120", ninjam::NinjamConnection::FormatTempoBpm(120.0f));
	EXPECT_EQ("120.375", ninjam::NinjamConnection::FormatTempoBpm(120.375f));
	EXPECT_EQ("99.01", ninjam::NinjamConnection::FormatTempoBpm(99.01f));
}

namespace
{
	ninjam::NinjamMetronomeTimingInput MakeInput()
	{
		ninjam::NinjamMetronomeTimingInput input;
		input.intervalLengthSamps = 16000u;
		input.bpm = 120.0f;
		input.bpi = 8u;
		input.deviceSampleRate = 8000u;
		input.numFrames = 128u;
		return input;
	}
}

TEST(NinjamMetronomeTiming, FindsBeatAtBlockStart)
{
	auto input = MakeInput();
	input.intervalPositionSamps = 4000u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.onsetCount, 1u);
	EXPECT_EQ(result.onsets[0].offset, 0u);
	EXPECT_FALSE(result.onsets[0].accent);
}

TEST(NinjamMetronomeTiming, FindsBeatInsideBlock)
{
	auto input = MakeInput();
	input.intervalPositionSamps = 3950u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.onsetCount, 1u);
	EXPECT_EQ(result.onsets[0].offset, 50u);
	EXPECT_FALSE(result.onsets[0].accent);
}

TEST(NinjamMetronomeTiming, ReportsCoincidentIntervalAccent)
{
	auto input = MakeInput();
	input.intervalPositionSamps = 15980u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.onsetCount, 2u);
	EXPECT_EQ(result.onsets[0].offset, 20u);
	EXPECT_FALSE(result.onsets[0].accent);
	EXPECT_EQ(result.onsets[1].offset, 20u);
	EXPECT_TRUE(result.onsets[1].accent);
}

TEST(NinjamMetronomeTiming, OrdersIntervalAccentBeforeFollowingBeat)
{
	auto input = MakeInput();
	input.intervalPositionSamps = 15980u;
	input.numFrames = 4096u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.onsetCount, 4u);
	EXPECT_EQ(result.onsets[0].offset, 20u);
	EXPECT_FALSE(result.onsets[0].accent);
	EXPECT_EQ(result.onsets[1].offset, 20u);
	EXPECT_TRUE(result.onsets[1].accent);
	EXPECT_EQ(result.onsets[2].offset, 2020u);
	EXPECT_FALSE(result.onsets[2].accent);
	EXPECT_EQ(result.onsets[3].offset, 4020u);
	EXPECT_FALSE(result.onsets[3].accent);
}

TEST(NinjamMetronomeTiming, UsesIntervalGeometryWhenReportedBpmDisagrees)
{
	auto input = MakeInput();
	input.bpm = 60.0f; // Deliberately contradictory: canonical spacing is 2000 samples.
	input.intervalPositionSamps = 1950u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.onsetCount, 1u);
	EXPECT_EQ(result.onsets[0].offset, 50u);
	EXPECT_FALSE(result.onsets[0].accent);
}

TEST(NinjamMetronomeTiming, DoesNotResetForBpmMetadataChange)
{
	auto input = MakeInput();
	ninjam::NinjamMetronomeTimingState state;

	const auto first = ninjam::NinjamMetronomeTiming::Compute(input, state);
	input.bpm = 60.0f;
	const auto changedBpm = ninjam::NinjamMetronomeTiming::Compute(input, state);

	EXPECT_TRUE(first.generationReset);
	EXPECT_FALSE(changedBpm.generationReset);
}

TEST(NinjamMetronomeTiming, UsesCanonicalDeviceRateSamples)
{
	auto input = MakeInput();
	input.intervalPositionSamps = 7900u;
	input.intervalLengthSamps = 32000u;
	input.deviceSampleRate = 16000u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.onsetCount, 1u);
	EXPECT_EQ(result.onsets[0].offset, 100u);
}

TEST(NinjamMetronomeTiming, AdvancesForOutputLatency)
{
	auto input = MakeInput();
	input.intervalPositionSamps = 3900u;
	input.outputLatencySamps = 100u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.onsetCount, 1u);
	EXPECT_EQ(result.onsets[0].offset, 0u);
}

TEST(NinjamMetronomeTiming, RejectsInvalidGeometry)
{
	auto input = MakeInput();
	input.bpi = 0u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	EXPECT_FALSE(result.valid);
	EXPECT_EQ(result.onsetCount, 0u);
}

TEST(NinjamMetronomeTiming, TempoChangeStartsFreshGeneration)
{
	auto input = MakeInput();
	ninjam::NinjamMetronomeTimingState state;

	auto first = ninjam::NinjamMetronomeTiming::Compute(input, state);
	EXPECT_TRUE(first.generationReset);

	input.intervalPositionSamps = input.numFrames;
	auto steady = ninjam::NinjamMetronomeTiming::Compute(input, state);
	EXPECT_FALSE(steady.generationReset);

	input.bpm = 100.0f;
	input.intervalLengthSamps = 19200u;
	auto changed = ninjam::NinjamMetronomeTiming::Compute(input, state);
	EXPECT_TRUE(changed.valid);
	EXPECT_TRUE(changed.generationReset);
}
