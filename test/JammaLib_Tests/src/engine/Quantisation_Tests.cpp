#include "gtest/gtest.h"
#include "engine/Quantisation.h"

using engine::QuantisationPolicy;
using engine::TapTempoTracker;

TEST(Quantisation, DerivesSeedTimingFromMasterLoop)
{
	QuantisationPolicy policy;

	auto timing = engine::DeduceSeedTiming(48000ul * 8ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(96000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_EQ(4u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, SnapsTapSeedToWholeMasterDivision)
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = 400u;

	const auto masterLoopSamps = 48000u * 8u;
	auto timing = engine::DeduceTapSeedTiming(71000ul, masterLoopSamps, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(76800u, timing->SeedSamps);
	EXPECT_EQ(masterLoopSamps, timing->MasterLoopSamps);
	EXPECT_EQ(5u, timing->SeedCount);
	EXPECT_EQ(4u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(150.0f, timing->Bpm);
	EXPECT_EQ(20u, timing->Bpi);
}

TEST(Quantisation, EnforcesMinimumTapSeed)
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = 400u;

	auto timing = engine::DeduceTapSeedTiming(1000ul, 0ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(19200u, timing->SeedSamps);
	EXPECT_EQ(19200u, timing->MasterLoopSamps);
	EXPECT_EQ(1u, timing->SeedCount);
}

TEST(Quantisation, ConvertsNinjamTempoToIntervalSamples)
{
	EXPECT_EQ(384000u, engine::IntervalSampsFromTempo(120.0f, 16u, 48000u));
	EXPECT_EQ(0u, engine::IntervalSampsFromTempo(0.0f, 16u, 48000u));
	EXPECT_EQ(0u, engine::IntervalSampsFromTempo(120.0f, 0u, 48000u));
	EXPECT_EQ(0u, engine::IntervalSampsFromTempo(120.0f, 16u, 0u));
}

TEST(Quantisation, TapTempoTrackerSmoothsGaps)
{
	QuantisationPolicy policy;
	TapTempoTracker tracker;

	EXPECT_FALSE(tracker.TapAtSample(0ull, 48000u, 0ul, policy).has_value());
	auto timing1 = tracker.TapAtSample(48000ull, 48000u, 0ul, policy);
	auto timing2 = tracker.TapAtSample(100800ull, 48000u, 0ul, policy);

	ASSERT_TRUE(timing1.has_value());
	ASSERT_TRUE(timing2.has_value());
	EXPECT_EQ(48000u, timing1->SeedSamps);
	EXPECT_EQ(50400u, timing2->SeedSamps);
}
