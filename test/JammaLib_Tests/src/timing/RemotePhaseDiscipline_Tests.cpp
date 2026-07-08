#include "gtest/gtest.h"
#include "timing/TimingQuantiser.h"
#include "utils/Timer.h"

using timing::TimingQuantiser;

// Pure drift math backing TimingQuantiser::DisciplineRemotePhase.  The correction
// is the shortest-circular-distance dead-band that keeps the local master clock
// phase-locked to the remote NINJAM interval without steady-state jitter.

TEST(RemotePhaseCorrection, ZeroIntervalIsNoOp)
{
	EXPECT_FALSE(TimingQuantiser::RemotePhaseCorrectionOffset(0u, 0u, 0u, 64u).has_value());
}

TEST(RemotePhaseCorrection, SmallDriftWithinThresholdSkips)
{
	// current 500, remote 520 -> drift 20 < threshold 64.
	EXPECT_FALSE(TimingQuantiser::RemotePhaseCorrectionOffset(500u, 520u, 1000u, 64u).has_value());
}

TEST(RemotePhaseCorrection, DriftBeyondThresholdCorrectsToRemotePos)
{
	// current 500, remote 800 -> drift 300 >= 64 -> correct to 800.
	auto corrected = TimingQuantiser::RemotePhaseCorrectionOffset(500u, 800u, 1000u, 64u);
	ASSERT_TRUE(corrected.has_value());
	EXPECT_EQ(800u, *corrected);
}

TEST(RemotePhaseCorrection, UsesShortestCircularDistance)
{
	// current 990, remote 10 -> naive diff -980 but circular distance is 20 < 64.
	EXPECT_FALSE(TimingQuantiser::RemotePhaseCorrectionOffset(990u, 10u, 1000u, 64u).has_value());

	// current 10, remote 990 -> circular distance 20 < 64.
	EXPECT_FALSE(TimingQuantiser::RemotePhaseCorrectionOffset(10u, 990u, 1000u, 64u).has_value());
}

TEST(RemotePhaseCorrection, CircularDriftBeyondThresholdCorrects)
{
	// current 900, remote 100 -> circular distance 200 >= 64 -> correct to 100.
	auto corrected = TimingQuantiser::RemotePhaseCorrectionOffset(900u, 100u, 1000u, 64u);
	ASSERT_TRUE(corrected.has_value());
	EXPECT_EQ(100u, *corrected);
}

TEST(RemotePhaseCorrection, ThresholdBoundaryIsInclusive)
{
	// drift exactly equal to threshold triggers a correction (only < threshold skips).
	auto corrected = TimingQuantiser::RemotePhaseCorrectionOffset(500u, 564u, 1000u, 64u);
	ASSERT_TRUE(corrected.has_value());
	EXPECT_EQ(564u, *corrected);
}

TEST(RemotePhaseCorrection, NormalisesRemotePositionModuloInterval)
{
	// remote position exceeding the interval length is normalised before use.
	auto corrected = TimingQuantiser::RemotePhaseCorrectionOffset(0u, 1300u, 1000u, 64u);
	ASSERT_TRUE(corrected.has_value());
	EXPECT_EQ(300u, *corrected);
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
	TimingQuantiser quantiser;
	EXPECT_FALSE(quantiser.DisciplineRemotePhase(500u, 1000u));
}

TEST(DisciplineRemotePhase, UnseededClockIsNoOp)
{
	auto clock = std::make_shared<utils::Timer>();
	TimingQuantiser quantiser;
	quantiser.SetClock(clock);
	EXPECT_FALSE(quantiser.DisciplineRemotePhase(500u, 1000u));
}

TEST(DisciplineRemotePhase, DifferentIntervalLengthFallsThrough)
{
	// A genuine tempo change (seeded length != interval length) must be left for the
	// accepted-remote-tempo path, not disciplined here.
	auto clock = MakeSeededClock(1000ul, 500u);
	TimingQuantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_FALSE(quantiser.DisciplineRemotePhase(500u, 2000u));
	EXPECT_EQ(500u, clock->SampOffset());
}

TEST(DisciplineRemotePhase, SmallDriftLeavesClockUntouched)
{
	auto clock = MakeSeededClock(1000ul, 500u);
	TimingQuantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_FALSE(quantiser.DisciplineRemotePhase(530u, 1000u));
	EXPECT_EQ(500u, clock->SampOffset());
}

TEST(DisciplineRemotePhase, DriftBeyondThresholdRealignsClock)
{
	auto clock = MakeSeededClock(1000ul, 500u);
	TimingQuantiser quantiser;
	quantiser.SetClock(clock);

	EXPECT_TRUE(quantiser.DisciplineRemotePhase(800u, 1000u));
	EXPECT_EQ(800u, clock->SampOffset());
}

