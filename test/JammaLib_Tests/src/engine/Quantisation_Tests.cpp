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

TEST(Quantisation, TimingFromSeedAndMasterDerivesBpmAndBpi)
{
	QuantisationPolicy policy;

	// Master = 8 seconds at 48kHz; seed = 2 seconds (quarter of master).
	// seed=96000 => raw bpm=30; doubles beatsPerSeed until bpm >= SeedBpmMin(80):
	// beatsPerSeed=4, bpm=120. seedCount=4. bpi=4*4=16.
	const auto timing = engine::TimingFromSeedAndMaster(96000u, 384000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(96000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_EQ(4u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, TimingFromSeedAndMasterRejectsZeroInputs)
{
	QuantisationPolicy policy;
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(0u, 384000ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(96000u, 0ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(96000u, 384000ul, 0u, policy).has_value());
}

// ---------------------------------------------------------------------------
// DeduceSeedTiming: seed sizes from master loop length
// ---------------------------------------------------------------------------

TEST(Quantisation, SeedFromMasterNoHalvingNeeded)
{
	// A 2 s master (96000 samps at 48 kHz) is already below the 3 s target
	// maximum, so DeduceSeedTiming must leave it unchanged.
	QuantisationPolicy policy;

	auto timing = engine::DeduceSeedTiming(96000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(96000u, timing->SeedSamps);
	EXPECT_EQ(96000u, timing->MasterLoopSamps);
	EXPECT_EQ(1u, timing->SeedCount);
	// Raw BPM = 30 < 80: doubled twice → beatsPerSeed = 4, BPM = 120, BPI = 4.
	EXPECT_EQ(4u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(4u, timing->Bpi);
}

TEST(Quantisation, SeedFromMasterRequiresMultipleHalvings)
{
	// A 32 s master needs four halvings before the seed falls below 3 s.
	// 1536000 → 768000 → 384000 → 192000 → 96000 (first value < 144000).
	QuantisationPolicy policy;

	auto timing = engine::DeduceSeedTiming(1536000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(96000u, timing->SeedSamps);
	EXPECT_EQ(1536000u, timing->MasterLoopSamps);
	EXPECT_EQ(16u, timing->SeedCount);
	EXPECT_EQ(4u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(64u, timing->Bpi);
}

TEST(Quantisation, SeedFromMasterAt44100SampleRate)
{
	// 8 s at 44100 Hz = 352800 samps.  targetMaxSeed = 132300.
	// 352800/2 = 176400 > 132300; /2 = 88200 < 132300 → seed = 88200.
	QuantisationPolicy policy;

	auto timing = engine::DeduceSeedTiming(352800ul, 44100u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(88200u, timing->SeedSamps);
	EXPECT_EQ(352800u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_EQ(4u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, SeedFromMasterRejectsZeroInputs)
{
	QuantisationPolicy policy;
	EXPECT_FALSE(engine::DeduceSeedTiming(0ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::DeduceSeedTiming(384000ul, 0u, policy).has_value());
}

// ---------------------------------------------------------------------------
// DeduceTapSeedTiming: seed sizes for common tap tempos (no master)
// ---------------------------------------------------------------------------

TEST(Quantisation, TapSeedAt120BpmNoMaster)
{
	// 120 BPM quarter note = 0.5 s = 24000 samps at 48 kHz.
	// Raw BPM = 120 >= 80: no doubling; beatsPerSeed = 1.
	QuantisationPolicy policy;

	auto timing = engine::DeduceTapSeedTiming(24000ul, 0ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_EQ(1u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
}

TEST(Quantisation, TapSeedAt90BpmNoMaster)
{
	// 90 BPM quarter note = 32000 samps at 48 kHz.
	// Raw BPM = 90 >= 80: no doubling; beatsPerSeed = 1.
	QuantisationPolicy policy;

	auto timing = engine::DeduceTapSeedTiming(32000ul, 0ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(32000u, timing->SeedSamps);
	EXPECT_EQ(1u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(90.0f, timing->Bpm);
}

TEST(Quantisation, TapSeedAt60BpmNoMaster)
{
	// 60 BPM quarter note = 48000 samps at 48 kHz.
	// Raw BPM = 60 < 80: beatsPerSeed doubled to 2, reported BPM = 120.
	// This preserves the physical seed interval while keeping BPM in range.
	QuantisationPolicy policy;

	auto timing = engine::DeduceTapSeedTiming(48000ul, 0ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(48000u, timing->SeedSamps);
	EXPECT_EQ(2u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	// seedCount = 1 (no master), bpi = 1 * 2 = 2.
	EXPECT_EQ(2u, timing->Bpi);
}

// ---------------------------------------------------------------------------
// TapTempoTracker: smoothing and master-snapping
// ---------------------------------------------------------------------------

TEST(Quantisation, TapTempoTrackerAt90Bpm)
{
	QuantisationPolicy policy;
	TapTempoTracker tracker;

	// First tap sets the reference; no result.
	EXPECT_FALSE(tracker.TapAtSample(0ull, 48000u, 0ul, policy).has_value());

	// Second tap 32000 samps later: gap = 32000 → 90 BPM.
	auto timing = tracker.TapAtSample(32000ull, 48000u, 0ul, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(32000u, timing->SeedSamps);
	EXPECT_FLOAT_EQ(90.0f, timing->Bpm);
}

TEST(Quantisation, TapTempoTrackerSnapsToMasterDivision)
{
	// Master = 4 s = 192000 samps at 48 kHz.
	// Tapping at ~44000 samps (just under 1 s) should snap to the nearest
	// whole division of the master: 192000/4 = 48000.
	QuantisationPolicy policy;
	TapTempoTracker tracker;
	const auto master = 48000ul * 4ul;

	EXPECT_FALSE(tracker.TapAtSample(0ull, 48000u, master, policy).has_value());

	auto timing = tracker.TapAtSample(44000ull, 48000u, master, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(48000u, timing->SeedSamps);
	EXPECT_EQ(192000u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	// Raw BPM = 60 < 80: beatsPerSeed = 2, BPM = 120, BPI = 4*2 = 8.
	EXPECT_EQ(2u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(8u, timing->Bpi);
}

// ---------------------------------------------------------------------------
// NINJAM round-trips: BPM + BPI → interval → seed → derived BPM/BPI
//
// For each case the derived BPM and BPI must reproduce the same interval
// length via IntervalSampsFromTempo, even if the numeric BPM/BPI values
// differ from the original (an equivalent musical expression is acceptable).
// ---------------------------------------------------------------------------

TEST(Quantisation, NinjamRoundTrip_100bpm_4bpi)
{
	// 100 BPM, 4 BPI at 48 kHz → interval = 115200 samps (< 3 s; no halving).
	QuantisationPolicy policy;
	const unsigned int sr = 48000u;

	const auto interval = engine::IntervalSampsFromTempo(100.0f, 4u, sr);
	ASSERT_EQ(115200u, interval);

	auto seedOpt = engine::DeduceSeedTiming(interval, sr, policy);
	ASSERT_TRUE(seedOpt.has_value());
	EXPECT_EQ(115200u, seedOpt->SeedSamps);  // no halving: seed equals master
	EXPECT_EQ(1u, seedOpt->SeedCount);

	auto timingOpt = engine::TimingFromSeedAndMaster(seedOpt->SeedSamps, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_FLOAT_EQ(100.0f, timingOpt->Bpm);
	EXPECT_EQ(4u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_8bpi)
{
	// 120 BPM, 8 BPI at 48 kHz → interval = 192000 samps (> 3 s; halved once
	// to seed = 96000).
	QuantisationPolicy policy;
	const unsigned int sr = 48000u;

	const auto interval = engine::IntervalSampsFromTempo(120.0f, 8u, sr);
	ASSERT_EQ(192000u, interval);

	auto seedOpt = engine::DeduceSeedTiming(interval, sr, policy);
	ASSERT_TRUE(seedOpt.has_value());
	EXPECT_EQ(96000u, seedOpt->SeedSamps);
	EXPECT_EQ(2u, seedOpt->SeedCount);

	auto timingOpt = engine::TimingFromSeedAndMaster(seedOpt->SeedSamps, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(8u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_180bpm_16bpi)
{
	// 180 BPM, 16 BPI at 48 kHz → interval = 256000 samps.
	// Seed is halved once to 128000.  The algorithm normalises to a BPM >= 80,
	// yielding 90 BPM / 8 BPI — a different numeric expression of the same
	// interval length.
	QuantisationPolicy policy;
	const unsigned int sr = 48000u;

	const auto interval = engine::IntervalSampsFromTempo(180.0f, 16u, sr);
	ASSERT_EQ(256000u, interval);

	auto seedOpt = engine::DeduceSeedTiming(interval, sr, policy);
	ASSERT_TRUE(seedOpt.has_value());
	EXPECT_EQ(128000u, seedOpt->SeedSamps);
	EXPECT_EQ(2u, seedOpt->SeedCount);

	auto timingOpt = engine::TimingFromSeedAndMaster(seedOpt->SeedSamps, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_FLOAT_EQ(90.0f, timingOpt->Bpm);
	EXPECT_EQ(8u, timingOpt->Bpi);
	// Round-trip: same interval even though BPM/BPI values differ.
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_16bpi_44100Hz)
{
	// Same tempo as the basic 48 kHz test but at 44100 Hz.
	QuantisationPolicy policy;
	const unsigned int sr = 44100u;

	const auto interval = engine::IntervalSampsFromTempo(120.0f, 16u, sr);
	ASSERT_EQ(352800u, interval);

	auto seedOpt = engine::DeduceSeedTiming(interval, sr, policy);
	ASSERT_TRUE(seedOpt.has_value());
	EXPECT_EQ(88200u, seedOpt->SeedSamps);
	EXPECT_EQ(4u, seedOpt->SeedCount);

	auto timingOpt = engine::TimingFromSeedAndMaster(seedOpt->SeedSamps, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(16u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}
