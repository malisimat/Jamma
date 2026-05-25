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
	// 100 BPM, 4 BPI at 48 kHz → interval = 115200 samps.
	// Seed = one beat = 60*sr/BPM = 28800 samps; no halving policy applies.
	QuantisationPolicy policy;
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
	EXPECT_EQ(1u, timingOpt->BeatsPerSeed);  // BPM 100 >= 80: no doubling
	EXPECT_FLOAT_EQ(100.0f, timingOpt->Bpm);
	EXPECT_EQ(4u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_8bpi)
{
	// 120 BPM, 8 BPI at 48 kHz → interval = 192000 samps.
	// Seed = one beat = 60*sr/BPM = 24000 samps.
	QuantisationPolicy policy;
	const unsigned int sr = 48000u;

	const auto interval = engine::IntervalSampsFromTempo(120.0f, 8u, sr);
	ASSERT_EQ(192000u, interval);

	const auto seed = engine::IntervalSampsFromTempo(120.0f, 1u, sr);
	ASSERT_EQ(24000u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(24000u, timingOpt->SeedSamps);
	EXPECT_EQ(8u, timingOpt->SeedCount);
	EXPECT_EQ(1u, timingOpt->BeatsPerSeed);  // BPM 120 >= 80: no doubling
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(8u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_180bpm_16bpi)
{
	// 180 BPM, 16 BPI at 48 kHz → interval = 256000 samps.
	// Seed = one beat = 60*sr/BPM = 16000 samps; BPM 180 >= 80: no doubling,
	// so the original BPM and BPI are recovered exactly.
	QuantisationPolicy policy;
	const unsigned int sr = 48000u;

	const auto interval = engine::IntervalSampsFromTempo(180.0f, 16u, sr);
	ASSERT_EQ(256000u, interval);

	const auto seed = engine::IntervalSampsFromTempo(180.0f, 1u, sr);
	ASSERT_EQ(16000u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(16000u, timingOpt->SeedSamps);
	EXPECT_EQ(16u, timingOpt->SeedCount);
	EXPECT_EQ(1u, timingOpt->BeatsPerSeed);  // BPM 180 >= 80: no doubling
	EXPECT_FLOAT_EQ(180.0f, timingOpt->Bpm);
	EXPECT_EQ(16u, timingOpt->Bpi);
	EXPECT_EQ(interval, engine::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_16bpi_44100Hz)
{
	// 120 BPM, 16 BPI at 44100 Hz → interval = 352800 samps.
	// Seed = one beat = 60*sr/BPM = 22050 samps.
	QuantisationPolicy policy;
	const unsigned int sr = 44100u;

	const auto interval = engine::IntervalSampsFromTempo(120.0f, 16u, sr);
	ASSERT_EQ(352800u, interval);

	const auto seed = engine::IntervalSampsFromTempo(120.0f, 1u, sr);
	ASSERT_EQ(22050u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr, policy);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(22050u, timingOpt->SeedSamps);
	EXPECT_EQ(16u, timingOpt->SeedCount);
	EXPECT_EQ(1u, timingOpt->BeatsPerSeed);  // BPM 120 >= 80: no doubling
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
//   Minimum seed:        400 ms  (19200 samps @ 48 kHz)
//   Halving threshold:  3000 ms (144000 samps @ 48 kHz)
// Neither limit is enforced by this function.
// ---------------------------------------------------------------------------

TEST(Quantisation, TapSeedFromMasterExactDivisor)
{
	// tap = 24000 (120 BPM quarter note); master = 384000 (8 s).
	// 384000 / 16 = 24000: exact divisor.  BPM 120 >= 80: beatsPerSeed = 1.
	QuantisationPolicy policy;

	auto timing = engine::DeduceTapSeedTimingFromMaster(24000ul, 384000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(16u, timing->SeedCount);
	EXPECT_EQ(1u, timing->BeatsPerSeed);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterSnapsToNearestDivisor)
{
	// tap = 26000; master = 384000.
	// Nearest whole-divisor seeds: 384000/15=25600 (dist 400) and
	// 384000/16=24000 (dist 2000).  25600 wins.
	QuantisationPolicy policy;

	auto timing = engine::DeduceTapSeedTimingFromMaster(26000ul, 384000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(25600u, timing->SeedSamps);
	EXPECT_EQ(15u, timing->SeedCount);
	EXPECT_EQ(1u, timing->BeatsPerSeed);  // 112.5 BPM >= 80: no doubling
	EXPECT_FLOAT_EQ(112.5f, timing->Bpm);
	EXPECT_EQ(15u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterBelowMinSizeLimit)
{
	// tap = 2000 (41.7 ms at 48 kHz), below the 400 ms policy minimum.
	// master = 48000; 48000 / 24 = 2000: exact divisor.
	// DeduceTapSeedTiming would clamp to >= 19200 and return 24000;
	// DeduceTapSeedTimingFromMaster must return 2000 (no size limit).
	QuantisationPolicy policy;

	auto timing = engine::DeduceTapSeedTimingFromMaster(2000ul, 48000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(2000u, timing->SeedSamps);
	EXPECT_EQ(48000u, timing->MasterLoopSamps);
	EXPECT_EQ(24u, timing->SeedCount);
	EXPECT_EQ(1u, timing->BeatsPerSeed);  // 1440 BPM >= 80: no doubling
	EXPECT_FLOAT_EQ(1440.0f, timing->Bpm);
	EXPECT_EQ(24u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterAbovePolicySizeMax)
{
	// tap = 240000 (5 s at 48 kHz), above the 3 s halving threshold.
	// master = 480000; 480000 / 2 = 240000: exact divisor.
	// DeduceSeedTiming would halve to 120000; this function must return 240000.
	QuantisationPolicy policy;

	auto timing = engine::DeduceTapSeedTimingFromMaster(240000ul, 480000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(240000u, timing->SeedSamps);
	EXPECT_EQ(480000u, timing->MasterLoopSamps);
	EXPECT_EQ(2u, timing->SeedCount);
	EXPECT_EQ(8u, timing->BeatsPerSeed);  // 12 BPM -> doubled 3x -> 96 BPM
	EXPECT_FLOAT_EQ(96.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterRejectsZeroInputs)
{
	QuantisationPolicy policy;
	EXPECT_FALSE(engine::DeduceTapSeedTimingFromMaster(0ul, 384000ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::DeduceTapSeedTimingFromMaster(24000ul, 0ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::DeduceTapSeedTimingFromMaster(24000ul, 384000ul, 0u, policy).has_value());
}

TEST(QuantisationModel, GateGeometryBuildsStarShapedPrisms)
{
	auto verts = engine::QuantisationModel::BuildGateGeometry(8u, 132.0f, 312.0f, 92.0f);
	ASSERT_FALSE(verts.empty());
	EXPECT_EQ(8u * 36u * 3u, verts.size());

	std::vector<std::pair<int, int>> uniquePoints;
	for (size_t i = 0; i < 36u; ++i)
	{
		const auto x = static_cast<int>(std::round(verts[(i * 3u) + 0u] * 100.0f));
		const auto z = static_cast<int>(std::round(verts[(i * 3u) + 2u] * 100.0f));
		const auto point = std::make_pair(x, z);
		if (std::find(uniquePoints.begin(), uniquePoints.end(), point) == uniquePoints.end())
			uniquePoints.push_back(point);
	}

	EXPECT_GE(uniquePoints.size(), 4u);
}
