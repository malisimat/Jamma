#include "gtest/gtest.h"
#include <algorithm>
#include <vector>
#include "ninjam/NinjamConnection.h"
#include "ninjam/NinjamMetronomeTiming.h"

TEST(NinjamTempoCommand, PreservesUsefulFractionalBpmPrecision)
{
	EXPECT_EQ("120", ninjam::NinjamConnection::FormatTempoBpm(120.0f));
	EXPECT_EQ("120.375", ninjam::NinjamConnection::FormatTempoBpm(120.375f));
	EXPECT_EQ("99.01", ninjam::NinjamConnection::FormatTempoBpm(99.01f));
}

static ninjam::NinjamMetronomeTimingInput MakeInput()
{
	ninjam::NinjamMetronomeTimingInput input;
	input.IntervalLengthSamps = 16000u;
	input.Bpm = 120.0f;
	input.Bpi = 8u;
	input.DeviceSampleRate = 8000u;
	input.NumFrames = 128u;
	return input;
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

TEST(NinjamMetronomeTiming, UsesSameRenderPhaseAsLocalLoops)
{
	auto input = MakeInput();
	input.IntervalPositionSamps = 3900u; // Both loops and clicks incur the same DAC latency.
	ninjam::NinjamMetronomeTimingState state;

	const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);

	ASSERT_TRUE(result.Valid);
	ASSERT_EQ(result.OnsetCount, 1u);
	EXPECT_EQ(result.Onsets[0].Offset, 100u);
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

TEST(NinjamMetronomeTiming, RoundedBeatBoundariesEmitExactlyOnceAcrossBlocks)
{
	for (const auto interval : { 1001u, 313600u, 193139u })
	for (const auto bpi : { 1u, 7u, 16u, 32u })
	{
		SCOPED_TRACE(::testing::Message() << interval << "/" << bpi);
		ninjam::NinjamMetronomeTimingInput input;
		input.IntervalLengthSamps = interval;
		input.Bpi = bpi;
		ninjam::NinjamMetronomeTimingState state;
		std::vector<std::uint64_t> actual;
		for (std::uint64_t start = 0; start < interval * 2ull; start += input.NumFrames)
		{
			input.IntervalPositionSamps = static_cast<unsigned int>(start % interval);
			input.NumFrames = static_cast<unsigned int>(std::min(512ull, interval * 2ull - start));
			const auto result = ninjam::NinjamMetronomeTiming::Compute(input, state);
			for (unsigned int i = 0u; i < result.OnsetCount; ++i)
				if (!result.Onsets[i].Accent) actual.push_back(start + result.Onsets[i].Offset);
		}
		ASSERT_EQ(bpi * 2u, actual.size());
		for (unsigned int beat = 0u; beat < bpi * 2u; ++beat)
		{
			const auto expected = (static_cast<std::uint64_t>(beat) * interval + bpi / 2u) / bpi;
			EXPECT_EQ(expected, actual[beat]);
			// A rational boundary rounded down can lie before its exact fraction.
			input.IntervalPositionSamps = static_cast<unsigned int>(expected % interval);
			input.NumFrames = 1u;
			const auto exact = ninjam::NinjamMetronomeTiming::Compute(input, state);
			ASSERT_GT(exact.OnsetCount, 0u);
			EXPECT_EQ(0u, exact.Onsets[0].Offset);
		}
	}
}
