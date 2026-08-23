#include "gtest/gtest.h"
#include "engine/Quantiser.h"
#include "utils/Timer.h"

using engine::Quantiser;

// Pure drift math backing Quantiser::DisciplineRemotePhase.  The correction
// is the shortest-circular-distance dead-band that keeps the local master clock
// phase-locked to the remote NINJAM interval without steady-state jitter.

TEST(RemotePhaseCorrection, ZeroIntervalIsNoOp)
{
	EXPECT_FALSE(Quantiser::RemotePhaseCorrectionDelta(0u, 0u, 0u, 64u).has_value());
}

TEST(RemotePhaseCorrection, SmallDriftWithinThresholdSkips)
{
	// current 500, remote 520 -> drift 20 < threshold 64.
	EXPECT_FALSE(Quantiser::RemotePhaseCorrectionDelta(500u, 520u, 1000u, 64u).has_value());
}

TEST(RemotePhaseCorrection, DriftBeyondThresholdReturnsSignedDelta)
{
	EXPECT_EQ(300, Quantiser::RemotePhaseCorrectionDelta(500u, 800u, 1000u, 64u));
	EXPECT_EQ(-300, Quantiser::RemotePhaseCorrectionDelta(800u, 500u, 1000u, 64u));
}

TEST(RemotePhaseCorrection, UsesShortestCircularDistance)
{
	// current 990, remote 10 -> naive diff -980 but circular distance is 20 < 64.
	EXPECT_FALSE(Quantiser::RemotePhaseCorrectionDelta(990u, 10u, 1000u, 64u).has_value());

	// current 10, remote 990 -> circular distance 20 < 64.
	EXPECT_FALSE(Quantiser::RemotePhaseCorrectionDelta(10u, 990u, 1000u, 64u).has_value());
}

TEST(RemotePhaseCorrection, CircularDriftBeyondThresholdCorrects)
{
	EXPECT_EQ(200, Quantiser::RemotePhaseCorrectionDelta(900u, 100u, 1000u, 64u));
	EXPECT_EQ(-200, Quantiser::RemotePhaseCorrectionDelta(100u, 900u, 1000u, 64u));
}

TEST(RemotePhaseCorrection, ThresholdBoundaryIsInclusive)
{
	// drift exactly equal to threshold triggers a correction (only < threshold skips).
	EXPECT_EQ(64, Quantiser::RemotePhaseCorrectionDelta(500u, 564u, 1000u, 64u));
}

TEST(RemotePhaseCorrection, NormalisesRemotePositionModuloInterval)
{
	// remote position exceeding the interval length is normalised before use.
	EXPECT_EQ(300, Quantiser::RemotePhaseCorrectionDelta(0u, 1300u, 1000u, 64u));
}

TEST(RemotePhaseCorrection, HalfIntervalTieIsPositive)
{
	EXPECT_EQ(500, Quantiser::SignedCircularDifference(0u, 500u, 1000u));
	EXPECT_EQ(500, Quantiser::SignedCircularDifference(500u, 0u, 1000u));
}

namespace
{
	std::shared_ptr<utils::Timer> MakeSeededClock(unsigned long lengthSamps,
		unsigned int offsetSamps)
	{
		auto clock = std::make_shared<utils::Timer>();
		clock->SetQuantisation(lengthSamps, utils::Timer::QUANTISE_MULTIPLE);
		clock->SetSeedSourceLength(lengthSamps);
		if (offsetSamps > 0u)
			clock->Tick(offsetSamps, 0u);
		return clock;
	}
}

TEST(DisciplineRemotePhase, NoClockIsNoOp)
{
	Quantiser quantiser;
	EXPECT_FALSE(quantiser.DisciplineRemotePhase(500u, 1000u).has_value());
}

TEST(DisciplineRemotePhase, UnseededClockIsNoOp)
{
	auto clock = std::make_shared<utils::Timer>();
	Quantiser quantiser;
	quantiser.SetClock(clock);
	EXPECT_FALSE(quantiser.DisciplineRemotePhase(500u, 1000u).has_value());
}

TEST(DisciplineRemotePhase, DifferentIntervalLengthFallsThrough)
{
	// A genuine tempo change (seeded length != interval length) must be left for the
	// accepted-remote-tempo path, not disciplined here.
	auto clock = MakeSeededClock(1000ul, 500u);
	Quantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_FALSE(quantiser.DisciplineRemotePhase(500u, 2000u).has_value());
	EXPECT_EQ(500u, clock->SampOffset());
}

TEST(DisciplineRemotePhase, NearRemoteIntervalCorrectsRoundedTempoAtWrap)
{
	// A server can round a locally requested fractional BPM by one sample per
	// interval. Correct the local master and loop cursors by that residual rather
	// than letting it accumulate as visible drift.
	auto clock = MakeSeededClock(1000ul, 1u);
	Quantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_EQ(-1, quantiser.DisciplineRemotePhase(0u, 1001u));
	EXPECT_TRUE(clock->ConsumePendingCommand());
	EXPECT_EQ(0u, clock->SampOffset());
}

TEST(DisciplineRemotePhase, MatchingPhaseLeavesClockUntouched)
{
	auto clock = MakeSeededClock(1000ul, 500u);
	Quantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_FALSE(quantiser.DisciplineRemotePhase(500u, 1000u).has_value());
	EXPECT_EQ(500u, clock->SampOffset());
}

TEST(DisciplineRemotePhase, DriftBeyondThresholdRealignsClock)
{
	auto clock = MakeSeededClock(1000ul, 500u);
	Quantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_EQ(300, quantiser.DisciplineRemotePhase(800u, 1000u));
	EXPECT_TRUE(clock->ConsumePendingCommand());
	EXPECT_EQ(800u, clock->SampOffset());
}

TEST(DisciplineRemotePhase, ImplausibleSteadyDeltaIsRejected)
{
	auto clock = MakeSeededClock(10000ul, 1000u);
	Quantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_FALSE(quantiser.DisciplineRemotePhase(3000u, 10000u).has_value());
	EXPECT_EQ(1000u, clock->SampOffset());
}

TEST(DisciplineRemotePhase, DeliberateJoinDeltaAppliesInFull)
{
	auto clock = MakeSeededClock(10000ul, 1000u);
	Quantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_TRUE(quantiser.ApplyRemotePhaseCorrection(4000, 10000u));
	EXPECT_TRUE(clock->ConsumePendingCommand());
	EXPECT_EQ(5000u, clock->SampOffset());
}

