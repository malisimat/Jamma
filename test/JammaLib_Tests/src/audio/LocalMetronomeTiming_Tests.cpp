#include "gtest/gtest.h"
#include "audio/LocalMetronomeTiming.h"
#include "engine/Quantiser.h"
#include "utils/Timer.h"
#include <vector>

TEST(LocalMetronomeTiming, SlowRationalBeatsUsePhysicalFrameOffsets)
{
    const audio::PlaybackRateBlock block{0.5, 0.5, 1.0, 28u};
    ninjam::NinjamMetronomeTimingState state;
    const auto result = audio::LocalMetronomeTiming::Compute(block, 0.0, 7u, 3u, 1.0, state);
    ASSERT_TRUE(result.Valid);
    ASSERT_EQ(8u, result.OnsetCount);
    const unsigned int expected[] = {0u, 0u, 4u, 10u, 14u, 14u, 18u, 24u};
    for (unsigned int i = 0u; i < result.OnsetCount; ++i)
    {
        EXPECT_EQ(expected[i], result.Onsets[i].Offset);
        EXPECT_EQ(i == 1u || i == 5u, result.Onsets[i].Accent);
    }
}

TEST(LocalMetronomeTiming, CallbackPartitionsDoNotRepeatOrLoseFastBeats)
{
    ninjam::NinjamMetronomeTimingState wholeState, splitState;
    const audio::PlaybackRateBlock block{2.0, 2.0, 1.0, 28u};
    const auto whole = audio::LocalMetronomeTiming::Compute(block, 0.0, 21u, 7u, 1.0, wholeState);
    std::vector<unsigned int> split;
    for (unsigned int frame = 0u; frame < block.Frames; ++frame)
    {
        const audio::PlaybackRateBlock one{2.0, 2.0, 1.0, 1u};
        const auto result = audio::LocalMetronomeTiming::Compute(one, (frame * 2u) % 21u, 21u, 7u, 1.0, splitState);
        for (unsigned int i = 0u; i < result.OnsetCount; ++i) split.push_back(frame + result.Onsets[i].Offset);
    }
    ASSERT_EQ(whole.OnsetCount, split.size());
    for (unsigned int i = 0u; i < whole.OnsetCount; ++i) EXPECT_EQ(whole.Onsets[i].Offset, split[i]);
}

TEST(LocalMetronomeTiming, RecordingRateAndCommonRampAreConsumedTogether)
{
    audio::PlaybackRate rate;
    ASSERT_TRUE(rate.Configure(48000u));
    ASSERT_TRUE(rate.SetTarget(2.0, 1u));
    const auto block = rate.DescribeBlock(4096u);
    ninjam::NinjamMetronomeTimingState state;
    const auto result = audio::LocalMetronomeTiming::Compute(block, 0.25, 3001u, 7u, 0.5, state);
    auto cursor = block.Cursor();
    double position = 0.25;
    unsigned int onset = 0u;
    std::uint64_t beat = 1u;
    for (unsigned int frame = 0u; frame < block.Frames; ++frame)
    {
        const auto end = position + cursor.Next() / 0.5;
        while (static_cast<double>((beat * 3001u + 3u) / 7u) < end)
        {
            ASSERT_LT(onset, result.OnsetCount);
            EXPECT_EQ(frame, result.Onsets[onset++].Offset);
            if (beat % 7u == 0u)
            {
                ASSERT_LT(onset, result.OnsetCount);
                EXPECT_EQ(frame, result.Onsets[onset++].Offset);
            }
            ++beat;
        }
        position = end;
    }
    EXPECT_EQ(onset, result.OnsetCount);
}

TEST(LocalMetronomeTiming, TimerKeepsOddFullBpiAndFractionalPpq)
{
    utils::Timer timer;
    timer.SetQuantisation(142u, utils::Timer::QUANTISE_MULTIPLE);
    timer.SetSeedSourceLength(1001u);
    timer.SetMasterBeatsPerInterval(7u);
    timer.SetMasterRecordingPlaybackRate(0.5);
    timer.SetBlockMusicalAdvance(0.25, 0.5);
    timer.Tick(1u, 0u);
    const auto position = timer.LocalMusicalPosition(48000u);
    EXPECT_TRUE(position.IsValid);
    EXPECT_EQ(7, position.BeatsPerInterval);
    EXPECT_DOUBLE_EQ(0.25 * 7.0 / 1001.0, position.Ppq);
    EXPECT_DOUBLE_EQ(60.0 * 48000.0 * 7.0 / 1001.0, position.Tempo);
    EXPECT_EQ(1u, timer.SceneSamplePos());
}

TEST(LocalMetronomeTiming, TapUsesSourceGapButTimeoutStaysPhysical)
{
    engine::TapTempoTracker taps;
    const engine::QuantisationPolicy policy;
    EXPECT_FALSE(taps.TapAtSample(0u, 48000u, 96000ul, policy));
    ASSERT_TRUE(taps.TapAtSample(24000u, 48000u, 96000ul, policy, 12000.0));
    EXPECT_DOUBLE_EQ(12000.0, taps.EstimatedGapSamps());
    EXPECT_FALSE(taps.TapAtSample(120000u, 48000u, 96000ul, policy, 12000.0));
    EXPECT_FALSE(taps.HasEstimate());
}

TEST(LocalMetronomeTiming, ExplicitOddMeterQuantisesDirectRationalBoundaries)
{
    utils::Timer timer;
    timer.SetQuantisation(142u, utils::Timer::QUANTISE_MULTIPLE);
    timer.SetSeedSourceLength(1000u);
    timer.SetMasterBeatsPerInterval(7u);
    EXPECT_EQ(429ul, std::get<0>(timer.QuantiseLength(425ul)));
    EXPECT_EQ(1000ul, std::get<0>(timer.QuantiseLength(995ul)));
    timer.SetQuantisation(142u, utils::Timer::QUANTISE_POWER);
    timer.SetSeedSourceLength(1000u);
    timer.SetMasterBeatsPerInterval(7u);
    EXPECT_EQ(1000ul, std::get<0>(timer.QuantiseLength(995ul)));
}
