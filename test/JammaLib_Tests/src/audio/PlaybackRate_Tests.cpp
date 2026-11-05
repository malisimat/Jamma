#include "gtest/gtest.h"
#include "audio/PlaybackRate.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <thread>

TEST(PlaybackRate, InvalidTargetsAndStaleRevisionsLeaveAcceptedStateUnchanged)
{
	audio::PlaybackRate rate;
	ASSERT_TRUE(rate.Configure(48000u));
	ASSERT_TRUE(rate.SetTarget(0.5, 2u));
	for (const auto invalid : { 0.0, -1.0, 0.49, 2.01,
		std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
		EXPECT_FALSE(rate.SetTarget(invalid, 3u));
	EXPECT_FALSE(rate.SetTarget(2.0, 1u));
	EXPECT_FALSE(rate.SetTarget(2.0, 2u));
	EXPECT_EQ(rate.Target(), 0.5);
	EXPECT_EQ(rate.Current(), 1.0);
}

TEST(PlaybackRate, UnityAndRecordingRatiosAreExact)
{
	for (const auto global : { 0.5, 1.0, 2.0 })
	{
		audio::PlaybackRate rate;
		ASSERT_TRUE(rate.Jump(global));
		const auto block = rate.DescribeBlock(257u);
		EXPECT_EQ(block.Advance(257u, global), 257.0);
		EXPECT_EQ(block.Advance(257u, 2.0), 257.0 * global / 2.0);
		EXPECT_EQ(block.Advance(257u, 0.5), 257.0 * global / 0.5);
	}
}

TEST(PlaybackRate, RecurrencePrecedesIntegrationAndReadersDoNotMutateOwner)
{
	audio::PlaybackRateBlock block{ 1.0, 2.0, 2.0, 3u, 7u };
	auto first = block.Cursor();
	auto second = block.Cursor();
	EXPECT_EQ(first.Next(), 1.5);
	EXPECT_EQ(first.Next(), 1.75);
	EXPECT_EQ(second.Next(), 1.5);
	EXPECT_EQ(block.Advance(3u), 1.5 + 1.75 + 1.875);
	EXPECT_EQ(block.RateAt(0u), 1.5);
}

TEST(PlaybackRate, PartitionAndRetargetingPreserveCommonProgress)
{
	audio::PlaybackRate single;
	audio::PlaybackRate partitioned;
	ASSERT_TRUE(single.Configure(48000u));
	ASSERT_TRUE(partitioned.Configure(48000u));
	ASSERT_TRUE(single.SetTarget(0.5, 1u));
	ASSERT_TRUE(partitioned.SetTarget(0.5, 1u));
	audio::SourcePosition reference;
	audio::SourcePosition divided;
	for (unsigned int phase = 0u; phase < 3u; ++phase)
	{
		const auto whole = single.DescribeBlock(4096u);
		ASSERT_TRUE(reference.Advance(whole.Advance(whole.Frames)));
		ASSERT_TRUE(single.Commit(whole));
		unsigned int remaining = 4096u;
		for (unsigned int count = 1u; remaining > 0u; count = count % 127u + 1u)
		{
			const auto size = std::min(remaining, count);
			const auto block = partitioned.DescribeBlock(size);
			ASSERT_TRUE(divided.Advance(block.Advance(size)));
			ASSERT_TRUE(partitioned.Commit(block));
			remaining -= size;
		}
		EXPECT_EQ(single.Current(), partitioned.Current());
		EXPECT_NEAR(reference.Difference(divided), 0.0, 1e-9);
		ASSERT_TRUE(single.SetTarget(phase == 0u ? 2.0 : 1.0, phase + 2u));
		ASSERT_TRUE(partitioned.SetTarget(phase == 0u ? 2.0 : 1.0, phase + 2u));
	}
}

TEST(PlaybackRate, SettlesExactlyAndDeviceRestartPreservesState)
{
	audio::PlaybackRate rate;
	ASSERT_TRUE(rate.Configure(48000u));
	ASSERT_TRUE(rate.SetTarget(2.0, 1u));
	ASSERT_TRUE(rate.Commit(rate.DescribeBlock(50u)));
	const auto before = rate.Current();
	EXPECT_FALSE(rate.Configure(0u));
	EXPECT_FALSE(rate.Configure(48000u, 0.0));
	ASSERT_TRUE(rate.Configure(96000u));
	EXPECT_EQ(rate.Current(), before);
	EXPECT_EQ(rate.Target(), 2.0);
	ASSERT_TRUE(rate.Commit(rate.DescribeBlock(48000u)));
	EXPECT_TRUE(rate.IsSettled());
	EXPECT_EQ(rate.Current(), 2.0);
}

TEST(PlaybackRate, WideCoordinateRetainsFractionBeyondDoubleIntegerPrecision)
{
	audio::SourcePosition position{ (std::int64_t{ 1 } << 54), 0.125 };
	const auto start = position;
	for (unsigned int frame = 0; frame < 10000u; ++frame)
		ASSERT_TRUE(position.Advance(0.25));
	EXPECT_EQ(position.Difference(start), 2500.0);
	EXPECT_EQ(position.Fraction, 0.125);
	position = { std::numeric_limits<std::int64_t>::max(), 0.5 };
	EXPECT_FALSE(position.Advance(1.0));
	EXPECT_EQ(position.Whole, std::numeric_limits<std::int64_t>::max());
	EXPECT_EQ(position.Fraction, 0.5);
}

TEST(PlaybackRate, CorrectionsRemainPositiveAndSettleWithoutDroppingResidual)
{
	for (const auto initialError : { -100.0, 100.0 })
	{
		audio::SourcePhaseCorrection correction;
		correction.SetError(initialError);
		double total = 0.0;
		for (unsigned int frame = 0u; frame < 100000u && !correction.IsSettled(); ++frame)
		{
			const auto adjustment = correction.Apply(0.25, 480.0);
			EXPECT_LE(std::abs(adjustment), 0.25 * 0.25);
			EXPECT_GT(0.25 + adjustment, 0.0);
			total += adjustment;
		}
		EXPECT_TRUE(correction.IsSettled());
		EXPECT_NEAR(total, initialError, 1e-10);
	}
}

TEST(PlaybackRate, RemoteVelocityTransitionClosesIdealPhaseDebt)
{
	for (const auto oldVelocity : { 1.0 / 640.0, 640.0 })
	{
		audio::SourceVelocityTransition transition;
		transition.Initialise(oldVelocity);
		double audible = 0.0;
		double ideal = 0.0;
		for (unsigned int frame = 0u; frame < 100000u; ++frame)
		{
			const auto advance = transition.Next(0.25, 4.0);
			EXPECT_GT(advance, 0.0);
			EXPECT_LE(advance, transition.Velocity() * 1.25);
			EXPECT_GE(advance, transition.Velocity() * 0.75);
			audible += advance;
			ideal += 0.25;
		}
		EXPECT_TRUE(transition.IsSettled(0.25));
		EXPECT_NEAR(audible, ideal, 1e-6);
	}
}

TEST(PlaybackRate, HistoryProtectsPinnedSegmentsAndRejectsMissingCoverage)
{
	audio::PlaybackRateHistory history(2u);
	ASSERT_TRUE(history.Append(100u, { 1.0, 1.0, 480.0, 10u, 1u }));
	ASSERT_TRUE(history.Append(110u, { 1.0, 2.0, 2.0, 10u, 2u }));
	EXPECT_FALSE(history.Query(99u));
	EXPECT_FALSE(history.Query(121u));
	EXPECT_FALSE(history.Append(120u, { 2.0, 2.0, 480.0, 10u, 3u }));
	ASSERT_TRUE(history.Query(100u));
	EXPECT_EQ(history.QueryDifference(100u, 110u, 0.5), 20.0);
	const auto sliced = history.DescribeAt(113u);
	ASSERT_TRUE(sliced);
	EXPECT_EQ(sliced->Frames, 7u);
	auto cursor = sliced->Cursor();
	EXPECT_EQ(cursor.Next(), history.Query(113u)->Rate);
	EXPECT_NEAR(sliced->Advance(7u), *history.QueryDifference(113u, 120u), 1e-12);
	history.RetainFrom(110u);
	ASSERT_TRUE(history.Append(120u, { 2.0, 2.0, 480.0, 10u, 3u }));
	EXPECT_FALSE(history.Query(100u));
	EXPECT_TRUE(history.Query(110u));
	EXPECT_FALSE(history.Append(140u, { 2.0, 2.0, 480.0, 10u, 3u }));
}

TEST(PlaybackRate, SourceTransitionDoesNotSmoothCommonRateTwice)
{
	audio::PlaybackRateBlock ordinary{ 1.0, 2.0, 480.0, 1000u, 1u };
	ordinary.SourceScale = 2.0;
	auto transitioned = ordinary;
	transitioned.HasSourceTransition = true;
	transitioned.InitialSourceVelocity = 2.0;
	auto reference = ordinary.Cursor();
	auto replay = transitioned.Cursor();
	for (unsigned int frame = 0u; frame < ordinary.Frames; ++frame)
	{
		EXPECT_EQ(reference.Next(), replay.Next());
		EXPECT_EQ(replay.SourcePhaseError(), 0.0);
		EXPECT_EQ(replay.Correction(), 0.0);
	}
}

TEST(PlaybackRate, SourceTransitionAndHistorySlicesReplaySameTrajectory)
{
	audio::PlaybackRateBlock block{ 1.0, 0.5, 48.0, 100u, 1u };
	block.SourceScale = 2.0;
	block.HasSourceTransition = true;
	block.InitialSourceVelocity = 20.0;
	block.InitialSourcePhaseError = -13.0;
	audio::PlaybackRateHistory history(2u);
	ASSERT_TRUE(history.Append(0u, block));
	const auto slice = history.DescribeAt(30u);
	ASSERT_TRUE(slice);
	auto original = block.Cursor();
	for (unsigned int frame = 0u; frame < 30u; ++frame) original.Next();
	auto replay = slice->Cursor();
	for (unsigned int frame = 30u; frame < 100u; ++frame)
	{
		EXPECT_EQ(original.Next(), replay.Next());
		EXPECT_EQ(original.SourceVelocity(), replay.SourceVelocity());
		EXPECT_EQ(original.SourcePhaseError(), replay.SourcePhaseError());
	}
}

TEST(PlaybackRate, HistoryCoalescesSteadyBlocksWithoutConsumingPinnedCapacity)
{
	audio::PlaybackRateHistory history(1u);
	for (unsigned int frame = 0u; frame < 10000u; ++frame)
		ASSERT_TRUE(history.Append(frame, { 0.5, 0.5, 480.0, 1u, 1u }));
	EXPECT_EQ(history.Size(), 1u);
	EXPECT_EQ(history.QueryDifference(0u, 10000u), 5000.0);
	EXPECT_FALSE(history.Append(10000u, { 0.5, 2.0, 480.0, 1u, 2u }));
}

TEST(PlaybackRate, ConcurrentHistoryReadersObserveOneCompleteSegment)
{
	audio::PlaybackRateHistory history(16u);
	std::atomic<bool> done{ false };
	std::atomic<std::uint64_t> newest{ 0u };
	std::atomic<unsigned int> invalid{ 0u };
	std::thread reader([&]
		{
			while (!done.load())
			{
				const auto frame = newest.load();
				const auto point = history.Query(frame);
				if (!point) continue;
				const auto expected = static_cast<std::int64_t>((frame / 2u) * 3u + frame % 2u);
				if (point->Coordinate.Whole != expected || point->Coordinate.Fraction != 0.0 ||
					point->Rate != (frame % 2u == 0u ? 1.0 : 2.0) || point->Revision != frame)
					invalid.fetch_add(1u);
			}
		});
	for (std::uint64_t frame = 0u; frame < 10000u; ++frame)
	{
		history.RetainFrom(frame);
		const auto rate = frame % 2u == 0u ? 1.0 : 2.0;
		if (!history.Append(frame, { rate, rate, 480.0, 1u, frame })) invalid.fetch_add(1u);
		newest.store(frame);
	}
	done.store(true);
	reader.join();
	EXPECT_EQ(invalid.load(), 0u);
}

TEST(PlaybackRateHistory, UnknownPhaseEditRequiresReservedSlotEvenWhenRateContinues)
{
    audio::PlaybackRateHistory history(1u);
    const audio::PlaybackRateBlock block{1.0, 1.0, 1.0, 8u, 1u};
    ASSERT_TRUE(history.Append(0u, block));
    history.RetainFrom(0u);
    EXPECT_TRUE(history.CanAppend(8u, block));
    EXPECT_FALSE(history.CanAppend(8u, block, false));
    history.RetainFrom(8u);
    EXPECT_TRUE(history.CanAppend(8u, block, false));
}