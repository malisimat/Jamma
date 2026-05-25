#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include "gtest/gtest.h"
#include "engine/Quantisation.h"
#include "engine/QuantisationModel.h"

using engine::QuantisationPolicy;
using engine::TapTempoTracker;

TEST(Quantisation, DerivesSeedTimingFromMasterLoop)
{

	auto timing = engine::DeduceSeedTiming(48000ul * 8ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(16u, timing->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, SnapsTapSeedToWholeMasterDivision)
{
	policy.SeedGrainMinMs = 400u;

	const auto masterLoopSamps = 48000u * 8u;
	auto timing = engine::DeduceTapSeedTiming(71000ul, masterLoopSamps, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(76800u, timing->SeedSamps);
	EXPECT_EQ(masterLoopSamps, timing->MasterLoopSamps);
	EXPECT_EQ(5u, timing->SeedCount);
	EXPECT_FLOAT_EQ(37.5f, timing->Bpm);
	EXPECT_EQ(5u, timing->Bpi);
}

// master=132096 @ 44100 Hz (~3 s, bpi=4 @ 80 BPM).
// Tap near 160 BPM (bpi=8): requested seed ≈16512 (374 ms).
// With 400 ms floor the old linear loop capped maxDivisor at 7 and returned bpi=6.
// With 300 ms floor and the sqrt divisor enumeration, bpi=8 (seed=16512) is reachable.
TEST(Quantisation, SnapsTapSeedToHighBpiDivisorViaSqrtSearch)
{
	policy.SeedGrainMinMs = 300u;

	// 132096 = 2^10 * 3 * 43; exact divisors include 8 (seed=16512) and 6 (seed=22016).
	// Requested seed 16512 is closest to divisor 8, so bpi=8 should win.
	const auto masterLoopSamps = 132096ul;
	auto timing = engine::DeduceTapSeedTiming(16512ul, masterLoopSamps, 44100u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(16512u, timing->SeedSamps);
	EXPECT_EQ(masterLoopSamps, timing->MasterLoopSamps);
	EXPECT_EQ(8u, timing->SeedCount);
	EXPECT_EQ(8u, timing->Bpi);
}

TEST(Quantisation, EnforcesMinimumTapSeed)
{
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

	// Master = 8 seconds at 48kHz; seed = 2 seconds (quarter of master).
	// Direct seed timing preserves the supplied seed: BPI is the number of
	// actual seed divisions in the master loop.
	const auto timing = engine::TimingFromSeedAndMaster(96000u, 384000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(96000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_FLOAT_EQ(30.0f, timing->Bpm);
	EXPECT_EQ(4u, timing->Bpi);
}

TEST(Quantisation, TimingFromSeedAndMasterRejectsZeroInputs)
{
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(0u, 384000ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(96000u, 0ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(96000u, 384000ul, 0u, policy).has_value());
}

// ---------------------------------------------------------------------------
// DeduceSeedTiming: seed sizes from master loop length
// ---------------------------------------------------------------------------

TEST(Quantisation, SeedFromMasterNoHalvingNeeded)
{
	// A 2 s master is below the 3 s target maximum, but its raw seed BPM is
	// below the policy floor.  DeduceSeedTiming halves to an actual BPM seed,
	// so BPI equals the number of seed gates in the master.

	auto timing = engine::DeduceSeedTiming(96000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_EQ(96000u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(4u, timing->Bpi);
}

TEST(Quantisation, SeedFromMasterRequiresMultipleHalvings)
{
	// A 32 s master first halves below 3 s, then halves to an actual BPM seed.
	// 1536000 -> 768000 -> 384000 -> 192000 -> 96000 -> 48000 -> 24000.

	auto timing = engine::DeduceSeedTiming(1536000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_EQ(1536000u, timing->MasterLoopSamps);
	EXPECT_EQ(64u, timing->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(64u, timing->Bpi);
}

TEST(Quantisation, SeedFromMasterAt44100SampleRate)
{
	// 8 s at 44100 Hz = 352800 samps.  After target-max halving to 88200,
	// the seed halves twice more so it represents 120 BPM directly.

	auto timing = engine::DeduceSeedTiming(352800ul, 44100u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(22050u, timing->SeedSamps);
	EXPECT_EQ(352800u, timing->MasterLoopSamps);
	EXPECT_EQ(16u, timing->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, SeedFromFirstFourBeatLoopDrawsFourGates)
{

	auto timing = engine::DeduceSeedTiming(124928ul, 44100u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(31232u, timing->SeedSamps);
	EXPECT_EQ(124928u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_NEAR(84.72077f, timing->Bpm, 0.0001f);
	EXPECT_EQ(4u, timing->Bpi);
}

TEST(Quantisation, SeedFromMasterRejectsZeroInputs)
{
	EXPECT_FALSE(engine::DeduceSeedTiming(0ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::DeduceSeedTiming(384000ul, 0u, policy).has_value());
}

// ---------------------------------------------------------------------------
// DeduceTapSeedTiming: seed sizes for common tap tempos (no master)
// ---------------------------------------------------------------------------

TEST(Quantisation, TapSeedAt120BpmNoMaster)
{
	// 120 BPM quarter note = 0.5 s = 24000 samps at 48 kHz.

	auto timing = engine::DeduceTapSeedTiming(24000ul, 0ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
}

TEST(Quantisation, TapSeedAt90BpmNoMaster)
{
	// 90 BPM quarter note = 32000 samps at 48 kHz.

	auto timing = engine::DeduceTapSeedTiming(32000ul, 0ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(32000u, timing->SeedSamps);
	EXPECT_FLOAT_EQ(90.0f, timing->Bpm);
}

TEST(Quantisation, TapSeedAt60BpmNoMaster)
{
	// 60 BPM quarter note = 48000 samps at 48 kHz.
	// Explicit tap timing preserves the physical seed interval.

	auto timing = engine::DeduceTapSeedTiming(48000ul, 0ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(48000u, timing->SeedSamps);
	EXPECT_FLOAT_EQ(60.0f, timing->Bpm);
	EXPECT_EQ(1u, timing->Bpi);
}

// ---------------------------------------------------------------------------
// TapTempoTracker: smoothing and master-snapping
// ---------------------------------------------------------------------------

TEST(Quantisation, TapTempoTrackerAt90Bpm)
{
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
	TapTempoTracker tracker;
	const auto master = 48000ul * 4ul;

	EXPECT_FALSE(tracker.TapAtSample(0ull, 48000u, master, policy).has_value());

	auto timing = tracker.TapAtSample(44000ull, 48000u, master, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(48000u, timing->SeedSamps);
	EXPECT_EQ(192000u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_FLOAT_EQ(60.0f, timing->Bpm);
	EXPECT_EQ(4u, timing->Bpi);
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
	// 100 BPM, 4 BPI at 48 kHz → interval = 115200 samps.
	// Seed = one beat = 60*sr/BPM = 28800 samps; no halving policy applies.
	const unsigned int sr = 48000u;

	const auto interval = engine::IntervalSampsFromTempo(100.0f, 4u, sr);
	ASSERT_EQ(115200u, interval);

	// Derive seed directly as one beat: IntervalSampsFromTempo(bpm, 1, sr).
	const auto seed = engine::IntervalSampsFromTempo(100.0f, 1u, sr);
	ASSERT_EQ(28800u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(28800u, timingOpt->SeedSamps);
	EXPECT_EQ(4u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(100.0f, timingOpt->Bpm);
	EXPECT_EQ(4u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_8bpi)
{
	// 120 BPM, 8 BPI at 48 kHz → interval = 192000 samps.
	// Seed = one beat = 60*sr/BPM = 24000 samps.
	const unsigned int sr = 48000u;

	const auto interval = engine::IntervalSampsFromTempo(120.0f, 8u, sr);
	ASSERT_EQ(192000u, interval);

	const auto seed = engine::IntervalSampsFromTempo(120.0f, 1u, sr);
	ASSERT_EQ(24000u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(24000u, timingOpt->SeedSamps);
	EXPECT_EQ(8u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(8u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_180bpm_16bpi)
{
	// 180 BPM, 16 BPI at 48 kHz → interval = 256000 samps.
	// Seed = one beat = 60*sr/BPM = 16000 samps, so the original BPM and BPI
	// are recovered exactly.
	const unsigned int sr = 48000u;

	const auto interval = engine::IntervalSampsFromTempo(180.0f, 16u, sr);
	ASSERT_EQ(256000u, interval);

	const auto seed = engine::IntervalSampsFromTempo(180.0f, 1u, sr);
	ASSERT_EQ(16000u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(16000u, timingOpt->SeedSamps);
	EXPECT_EQ(16u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(180.0f, timingOpt->Bpm);
	EXPECT_EQ(16u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_16bpi_44100Hz)
{
	// 120 BPM, 16 BPI at 44100 Hz → interval = 352800 samps.
	// Seed = one beat = 60*sr/BPM = 22050 samps.
	const unsigned int sr = 44100u;

	const auto interval = engine::IntervalSampsFromTempo(120.0f, 16u, sr);
	ASSERT_EQ(352800u, interval);

	const auto seed = engine::IntervalSampsFromTempo(120.0f, 1u, sr);
	ASSERT_EQ(22050u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(22050u, timingOpt->SeedSamps);
	EXPECT_EQ(16u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(16u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

// ---------------------------------------------------------------------------
// DeduceTapSeedTimingFromMaster: snap tap gap to nearest whole divisor of
// master, with no seed-size limits.  Used for 'first recorded loop after
// reclock/start' when tap tempo is active.
//
// Policy limits for reference (DefaultSeedGrainMinMs / DefaultSeedGrainTargetMaxMs):
//   Minimum seed:        300 ms  (14400 samps @ 48 kHz)
//   Halving threshold:  3000 ms (144000 samps @ 48 kHz)
// Neither limit is enforced by this function.
// ---------------------------------------------------------------------------

TEST(Quantisation, TapSeedFromMasterExactDivisor)
{
	// tap = 24000 (120 BPM quarter note); master = 384000 (8 s).
	// 384000 / 16 = 24000: exact divisor.

	auto timing = engine::DeduceTapSeedTimingFromMaster(24000ul, 384000ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(16u, timing->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterSnapsToNearestDivisor)
{
	// tap = 26000; master = 384000.
	// Nearest whole-divisor seeds: 384000/15=25600 (dist 400) and
	// 384000/16=24000 (dist 2000).  25600 wins.

	auto timing = engine::DeduceTapSeedTimingFromMaster(26000ul, 384000ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(25600u, timing->SeedSamps);
	EXPECT_EQ(15u, timing->SeedCount);
	EXPECT_FLOAT_EQ(112.5f, timing->Bpm);
	EXPECT_EQ(15u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterBelowMinSizeLimit)
{
	// tap = 2000 (41.7 ms at 48 kHz), below the 300 ms policy minimum.
	// master = 48000; 48000 / 24 = 2000: exact divisor.
	// DeduceTapSeedTiming would clamp to >= 14400 and return 16000;
	// DeduceTapSeedTimingFromMaster must return 2000 (no size limit).

	auto timing = engine::DeduceTapSeedTimingFromMaster(2000ul, 48000ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(2000u, timing->SeedSamps);
	EXPECT_EQ(48000u, timing->MasterLoopSamps);
	EXPECT_EQ(24u, timing->SeedCount);
	EXPECT_FLOAT_EQ(1440.0f, timing->Bpm);
	EXPECT_EQ(24u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterAbovePolicySizeMax)
{
	// tap = 240000 (5 s at 48 kHz), above the 3 s halving threshold.
	// master = 480000; 480000 / 2 = 240000: exact divisor.
	// DeduceSeedTiming would halve to 120000; this function must return 240000.

	auto timing = engine::DeduceTapSeedTimingFromMaster(240000ul, 480000ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(240000u, timing->SeedSamps);
	EXPECT_EQ(480000u, timing->MasterLoopSamps);
	EXPECT_EQ(2u, timing->SeedCount);
	EXPECT_FLOAT_EQ(12.0f, timing->Bpm);
	EXPECT_EQ(2u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterRejectsZeroInputs)
{
	EXPECT_FALSE(engine::DeduceTapSeedTimingFromMaster(0ul, 384000ul, 48000u).has_value());
	EXPECT_FALSE(engine::DeduceTapSeedTimingFromMaster(24000ul, 0ul, 48000u).has_value());
	EXPECT_FALSE(engine::DeduceTapSeedTimingFromMaster(24000ul, 384000ul, 0u).has_value());
}

TEST(QuantisationModel, GateGeometryBuildsHalfFrameInstanceMesh)
{
	auto singleGateVerts = engine::QuantisationModel::BuildGateGeometry(1u, 132.0f, 312.0f, 92.0f);
	auto repeatedGateVerts = engine::QuantisationModel::BuildGateGeometry(8u, 132.0f, 312.0f, 92.0f);
	ASSERT_FALSE(repeatedGateVerts.empty());
	EXPECT_EQ(singleGateVerts, repeatedGateVerts);
	EXPECT_EQ(15u * 6u * 3u, repeatedGateVerts.size());

	auto minX = repeatedGateVerts[0];
	auto maxX = repeatedGateVerts[0];
	auto minY = repeatedGateVerts[1];
	auto maxY = repeatedGateVerts[1];
	auto minZ = repeatedGateVerts[2];
	auto maxZ = repeatedGateVerts[2];
	for (size_t i = 0; i < repeatedGateVerts.size(); i += 3u)
	{
		minX = (std::min)(minX, repeatedGateVerts[i + 0u]);
		maxX = (std::max)(maxX, repeatedGateVerts[i + 0u]);
		minY = (std::min)(minY, repeatedGateVerts[i + 1u]);
		maxY = (std::max)(maxY, repeatedGateVerts[i + 1u]);
		minZ = (std::min)(minZ, repeatedGateVerts[i + 2u]);
		maxZ = (std::max)(maxZ, repeatedGateVerts[i + 2u]);
	}

	EXPECT_LT(minX, 0.0f);
	EXPECT_GT(maxX, 0.0f);
	EXPECT_FLOAT_EQ(-92.0f, minY);
	EXPECT_FLOAT_EQ(92.0f, maxY);
	EXPECT_FLOAT_EQ(132.0f, minZ);
	EXPECT_FLOAT_EQ(312.0f, maxZ);
}
