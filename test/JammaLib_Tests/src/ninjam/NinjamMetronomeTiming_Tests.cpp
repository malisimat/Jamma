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
		input.IntervalLengthSamps = 16000u;
		input.Bpm = 120.0f;
		input.Bpi = 8u;
		input.DeviceSampleRate = 8000u;
		input.NumFrames = 128u;
		return input;
	}
}

TEST(NinjamMetronomeTiming, FindsBeatAtBlockStart)
{
	auto input = MakeInput();
	input.IntervalPositionSamps = 4000u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.Valid);
	ASSERT_EQ(result.OnsetCount, 1u);
	EXPECT_EQ(result.Onsets[0].Offset, 0u);
	EXPECT_FALSE(result.Onsets[0].Accent);
}

TEST(NinjamMetronomeTiming, FindsBeatInsideBlock)
{
	auto input = MakeInput();
	input.IntervalPositionSamps = 3950u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.Valid);
	ASSERT_EQ(result.OnsetCount, 1u);
	EXPECT_EQ(result.Onsets[0].Offset, 50u);
	EXPECT_FALSE(result.Onsets[0].Accent);
}

TEST(NinjamMetronomeTiming, ReportsCoincidentIntervalAccent)
{
	auto input = MakeInput();
	input.IntervalPositionSamps = 15980u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.Valid);
	ASSERT_EQ(result.OnsetCount, 2u);
	EXPECT_EQ(result.Onsets[0].Offset, 20u);
	EXPECT_FALSE(result.Onsets[0].Accent);
	EXPECT_EQ(result.Onsets[1].Offset, 20u);
	EXPECT_TRUE(result.Onsets[1].Accent);
}

TEST(NinjamMetronomeTiming, OrdersIntervalAccentBeforeFollowingBeat)
{
	auto input = MakeInput();
	input.IntervalPositionSamps = 15980u;
	input.NumFrames = 4096u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.Valid);
	ASSERT_EQ(result.OnsetCount, 4u);
	EXPECT_EQ(result.Onsets[0].Offset, 20u);
	EXPECT_FALSE(result.Onsets[0].Accent);
	EXPECT_EQ(result.Onsets[1].Offset, 20u);
	EXPECT_TRUE(result.Onsets[1].Accent);
	EXPECT_EQ(result.Onsets[2].Offset, 2020u);
	EXPECT_FALSE(result.Onsets[2].Accent);
	EXPECT_EQ(result.Onsets[3].Offset, 4020u);
	EXPECT_FALSE(result.Onsets[3].Accent);
}

TEST(NinjamMetronomeTiming, UsesIntervalGeometryWhenReportedBpmDisagrees)
{
	auto input = MakeInput();
	input.Bpm = 60.0f; // Deliberately contradictory: canonical spacing is 2000 samples.
	input.IntervalPositionSamps = 1950u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.Valid);
	ASSERT_EQ(result.OnsetCount, 1u);
	EXPECT_EQ(result.Onsets[0].Offset, 50u);
	EXPECT_FALSE(result.Onsets[0].Accent);
}

TEST(NinjamMetronomeTiming, DoesNotResetForBpmMetadataChange)
{
	auto input = MakeInput();
	ninjam::NinjamMetronomeTimingState state;

	const auto first = ninjam::NinjamMetronomeTiming::Compute(input, state);
	input.Bpm = 60.0f;
	const auto changedBpm = ninjam::NinjamMetronomeTiming::Compute(input, state);

	EXPECT_TRUE(first.GenerationReset);
	EXPECT_FALSE(changedBpm.GenerationReset);
}

TEST(NinjamMetronomeTiming, UsesCanonicalDeviceRateSamples)
{
	auto input = MakeInput();
	input.IntervalPositionSamps = 7900u;
	input.IntervalLengthSamps = 32000u;
	input.DeviceSampleRate = 16000u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.Valid);
	ASSERT_EQ(result.OnsetCount, 1u);
	EXPECT_EQ(result.Onsets[0].Offset, 100u);
}

TEST(NinjamMetronomeTiming, AdvancesForOutputLatency)
{
	auto input = MakeInput();
	input.IntervalPositionSamps = 3900u;
	input.OutputLatencySamps = 100u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.Valid);
	ASSERT_EQ(result.OnsetCount, 1u);
	EXPECT_EQ(result.Onsets[0].Offset, 0u);
}

TEST(NinjamMetronomeTiming, RejectsInvalidGeometry)
{
	auto input = MakeInput();
	input.Bpi = 0u;
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	EXPECT_FALSE(result.Valid);
	EXPECT_EQ(result.OnsetCount, 0u);
}

TEST(NinjamMetronomeTiming, TempoChangeStartsFreshGeneration)
{
	auto input = MakeInput();
	ninjam::NinjamMetronomeTimingState state;

	auto first = ninjam::NinjamMetronomeTiming::Compute(input, state);
	EXPECT_TRUE(first.GenerationReset);

	input.IntervalPositionSamps = input.NumFrames;
	auto steady = ninjam::NinjamMetronomeTiming::Compute(input, state);
	EXPECT_FALSE(steady.GenerationReset);

	input.Bpm = 100.0f;
	input.IntervalLengthSamps = 19200u;
	auto changed = ninjam::NinjamMetronomeTiming::Compute(input, state);
	EXPECT_TRUE(changed.Valid);
	EXPECT_TRUE(changed.GenerationReset);
}
