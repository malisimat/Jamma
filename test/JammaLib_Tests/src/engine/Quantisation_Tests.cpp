#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include "gtest/gtest.h"
#include "io/UserConfig.h"
#include "ninjam/NinjamConnection.h"
#include "ninjam/NinjamTiming.h"
#include "engine/Quantiser.h"
#include "engine/Station.h"
#include "engine/LoopTake.h"
#include "engine/Loop.h"
#include "graphics/QuantisationModel.h"
#include "graphics/MidiModel.h"
#include "midi/MidiQuantisation.h"
#include "utils/Timer.h"

using engine::QuantisationPolicy;
using engine::TapTempoTracker;

TEST(QuantisationGeometry, ValidatesExactLocalAudioGeometry)
{
	const auto geometry = engine::LocalAudioGeometry::Create(1003ul, 1001ul, 143u, 7u);
	ASSERT_TRUE(geometry.has_value());
	EXPECT_TRUE(geometry->IsValid());
	EXPECT_FALSE(engine::LocalAudioGeometry::Create(1003ul, 1002ul, 143u, 7u).has_value());
	EXPECT_FALSE(engine::LocalAudioGeometry::Create(1003ul, 1004ul, 251u, 4u).has_value());
}

TEST(QuantisationGrid, CalculatesFractionalCellsDirectlyWithExactEndpoints)
{
	const engine::QuantisationGrid grid{ 7u, engine::QuantisationGridSource::Tap };
	const unsigned long expected[] = { 0ul, 143ul, 286ul, 429ul, 571ul, 714ul, 857ul, 1000ul };
	for (auto index = 0u; index <= 7u; ++index)
		EXPECT_EQ(expected[index], grid.SampleAt(index, 1000ul));
}

TEST(QuantisationGrid, CalculatesRemoteFractionalCellsFromOneInterval)
{
	const engine::QuantisationGrid grid{ 4u, engine::QuantisationGridSource::Remote };
	const unsigned long expected[] = { 0ul, 19746ul, 39493ul, 59239ul, 78985ul };
	for (auto index = 0u; index <= 4u; ++index)
		EXPECT_EQ(expected[index], grid.SampleAt(index, 78985ul));
}

TEST(Quantisation, MasterTapUsesRequestedBpiRatherThanSampleDivisor)
{
	const auto timing = engine::Quantiser::DeduceTapSeedTimingFromMaster(143ul, 1003ul, 48000u);
	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(143u, timing->SeedSamps);
	EXPECT_EQ(1001u, timing->MasterLoopSamps);
	EXPECT_EQ(7u, timing->Bpi);
}

TEST(Quantisation, DerivesSeedTimingFromMasterLoop)
{
	QuantisationPolicy policy;

	auto timing = engine::Quantiser::DeduceSeedTiming(48000ul * 8ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(16u, timing->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, SeedPolicyBoundsConversionBeforeCast)
{
	constexpr auto maxUInt = (std::numeric_limits<unsigned int>::max)();

	QuantisationPolicy policy;
	EXPECT_EQ(14400u, engine::Quantiser::MinSeedSamps(48000u, policy));

	policy.SeedGrainMinMs = 0u;
	EXPECT_EQ(48u, engine::Quantiser::MinSeedSamps(48000u, policy));

	policy.SeedGrainMinMs = 1000u;
	EXPECT_EQ(maxUInt, engine::Quantiser::MinSeedSamps(maxUInt, policy));

	policy.SeedGrainMinMs = 1001u;
	EXPECT_EQ(maxUInt, engine::Quantiser::MinSeedSamps(maxUInt, policy));

	policy.SeedGrainMinMs = maxUInt;
	EXPECT_EQ(maxUInt, engine::Quantiser::MinSeedSamps(1000u, policy));

	policy.SeedGrainMinMs = 1u;
	EXPECT_EQ(4294967u, engine::Quantiser::MinSeedSamps(maxUInt, policy));

	const auto expectConsistentTiming = [](const char* name,
		unsigned long masterLoopSamps,
		unsigned int sampleRate,
		const QuantisationPolicy& candidate)
	{
		SCOPED_TRACE(name);
		const auto timing = engine::Quantiser::DeduceSeedTiming(masterLoopSamps, sampleRate, candidate);
		ASSERT_TRUE(timing.has_value());
		EXPECT_GT(timing->SeedSamps, 0u);
		EXPECT_GT(timing->MasterLoopSamps, 0u);
		EXPECT_GT(timing->SeedCount, 0u);
		EXPECT_TRUE(std::isfinite(timing->Bpm));
		EXPECT_GT(timing->Bpm, 0.0f);
		EXPECT_EQ(timing->SeedCount, timing->Bpi);
		EXPECT_EQ(static_cast<std::uint64_t>(timing->MasterLoopSamps),
			static_cast<std::uint64_t>(timing->SeedSamps) * timing->SeedCount);
	};

	expectConsistentTiming("default", 384000ul, 48000u, QuantisationPolicy{});

	QuantisationPolicy zeroPolicy;
	zeroPolicy.SeedGrainMinMs = 0u;
	zeroPolicy.SeedGrainTargetMaxMs = 0u;
	zeroPolicy.SeedBpmMin = 0u;
	expectConsistentTiming("zero", 48000ul, 48000u, zeroPolicy);

	QuantisationPolicy maxRatePolicy;
	expectConsistentTiming("max supported rate", 3072000ul, 384000u, maxRatePolicy);

	QuantisationPolicy invertedPolicy;
	invertedPolicy.SeedGrainMinMs = 400u;
	invertedPolicy.SeedGrainTargetMaxMs = 300u;
	expectConsistentTiming("minimum exceeds target maximum", 192000ul, 48000u, invertedPolicy);
}

TEST(Quantisation, EnforcesMinimumTapSeed)
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = 400u;

	auto timing = engine::Quantiser::DeduceTapSeedTiming(1000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(19200u, timing->SeedSamps);
	EXPECT_EQ(19200u, timing->MasterLoopSamps);
	EXPECT_EQ(1u, timing->SeedCount);
}

TEST(Quantisation, ConvertsNinjamTempoToIntervalSamples)
{
	EXPECT_EQ(384000u, ninjam::IntervalSampsFromTempo(120.0f, 16u, 48000u));
	EXPECT_EQ(0u, ninjam::IntervalSampsFromTempo(0.0f, 16u, 48000u));
	EXPECT_EQ(0u, ninjam::IntervalSampsFromTempo(120.0f, 0u, 48000u));
	EXPECT_EQ(0u, ninjam::IntervalSampsFromTempo(120.0f, 16u, 0u));
}

TEST(Quantisation, ResolvePhaseOffsetDragConvertsHorizontalPixelsToMilliseconds)
{
	EXPECT_EQ(2400, engine::Quantiser::ResolvePhaseOffsetDrag(0, 50, 48000u));
	EXPECT_EQ(-2400, engine::Quantiser::ResolvePhaseOffsetDrag(0, -50, 48000u));
	EXPECT_EQ(3400, engine::Quantiser::ResolvePhaseOffsetDrag(1000, 50, 48000u));
	EXPECT_EQ(1000, engine::Quantiser::ResolvePhaseOffsetDrag(1000, 50, 0u));
}

TEST(Quantisation, VisualCounts_FourGrainsQuarterProducesSixteenDivisions)
{
	engine::QuantisationLoopTakeVisual visual{};
	visual.LoopLengthSamps = 1600u;
	visual.GrainSamps = 400u;
	visual.LoopGrains = 4u;
	visual.Fraction = midi::MidiQuantisationFraction::Quarter;

	const auto counts = engine::QuantisationModel::ResolveVisualCounts(visual);
	EXPECT_EQ(4u, counts.GrainFrameCount);
	EXPECT_EQ(100u, counts.StepSamps);
	EXPECT_EQ(16u, counts.FractionDivisionCount);
}

TEST(Quantisation, VisualCounts_FractionalSampleStepRetainsAllDivisions)
{
	engine::QuantisationLoopTakeVisual visual{};
	visual.LoopLengthSamps = 404u;
	visual.GrainSamps = 101u;
	visual.LoopGrains = 4u;
	visual.Fraction = midi::MidiQuantisationFraction::Quarter;
	visual.UseAbsoluteLocalGrid = true;
	const auto counts = engine::QuantisationModel::ResolveVisualCounts(visual);
	EXPECT_EQ(4u, counts.GrainFrameCount);
	EXPECT_EQ(16u, counts.FractionDivisionCount);
}

TEST(Quantisation, RemoteNonDividingGridKeepsLegacyVisualCounts)
{
	engine::QuantisationLoopTakeVisual visual{};
	visual.LoopLengthSamps = 1000u;
	visual.GrainSamps = 400u;
	visual.LoopGrains = 2u;
	visual.Fraction = midi::MidiQuantisationFraction::Quarter;
	const auto counts = engine::QuantisationModel::ResolveVisualCounts(visual);
	EXPECT_EQ(2u, counts.GrainFrameCount);
	EXPECT_EQ(10u, counts.FractionDivisionCount);
}

TEST(Quantisation, VisualBoundariesUseRecordedTakeStartAndRoundedGrid)
{
	engine::QuantisationLoopTakeVisual first{};
	first.LoopLengthSamps = 404u;
	first.GrainSamps = 101u;
	first.TransportStartSamps = 39u;
	first.UseAbsoluteLocalGrid = true;
	auto second = first;
	second.TransportStartSamps = 64u;

	const auto firstBoundary = engine::QuantisationModel::VisualBoundaryOffsetSamps(first, 1u, 4u);
	const auto secondBoundary = engine::QuantisationModel::VisualBoundaryOffsetSamps(second, 0u, 4u);
	EXPECT_EQ(37u, firstBoundary);
	EXPECT_EQ(12u, secondBoundary);
	EXPECT_EQ(76u, first.TransportStartSamps + firstBoundary);
	EXPECT_EQ(76u, second.TransportStartSamps + secondBoundary);
}

TEST(Quantisation, VisualCounts_FourGrainsEighthProducesThirtyTwoDivisions)
{
	engine::QuantisationLoopTakeVisual visual{};
	visual.LoopLengthSamps = 1600u;
	visual.GrainSamps = 400u;
	visual.LoopGrains = 4u;
	visual.Fraction = midi::MidiQuantisationFraction::Eighth;

	const auto counts = engine::QuantisationModel::ResolveVisualCounts(visual);
	EXPECT_EQ(4u, counts.GrainFrameCount);
	EXPECT_EQ(50u, counts.StepSamps);
	EXPECT_EQ(32u, counts.FractionDivisionCount);
}

TEST(Quantisation, VisualCounts_SingleGrainQuarterProducesFourDivisions)
{
	engine::QuantisationLoopTakeVisual visual{};
	visual.LoopLengthSamps = 400u;
	visual.GrainSamps = 400u;
	visual.LoopGrains = 1u;
	visual.Fraction = midi::MidiQuantisationFraction::Quarter;

	const auto counts = engine::QuantisationModel::ResolveVisualCounts(visual);
	EXPECT_EQ(1u, counts.GrainFrameCount);
	EXPECT_EQ(100u, counts.StepSamps);
	EXPECT_EQ(4u, counts.FractionDivisionCount);
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
	// Master = 8 seconds at 48kHz; seed = 2 seconds (quarter of master).
	// Direct seed timing preserves the supplied seed: BPI is the number of
	// actual seed divisions in the master loop.
	const auto timing = engine::TimingFromSeedAndMaster(96000u, 384000ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(96000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_FLOAT_EQ(30.0f, timing->Bpm);
	EXPECT_EQ(4u, timing->Bpi);
}

TEST(Quantisation, TimingFromSeedAndMasterRejectsZeroInputs)
{
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(0u, 384000ul, 48000u).has_value());
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(96000u, 0ul, 48000u).has_value());
	EXPECT_FALSE(engine::TimingFromSeedAndMaster(96000u, 384000ul, 0u).has_value());
}

TEST(Quantisation, CurrentTempoTimingUsesActiveClockWhenMasterCacheIsUnset)
{
	auto clock = std::make_shared<utils::Timer>();
	clock->SetQuantisation(16896u, utils::Timer::QUANTISE_POWER);
	clock->SetSeedSourceLength(67584ul);
	engine::Quantiser quantiser;
	quantiser.SetClock(clock);

	const auto timing = quantiser.CurrentTempoTiming(44100u);
	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(16896u, timing->SeedSamps);
	EXPECT_EQ(67584u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->Bpi);
}

// ---------------------------------------------------------------------------
// DeduceSeedTiming: seed sizes from master loop length
// ---------------------------------------------------------------------------

TEST(Quantisation, SeedFromMasterNoHalvingNeeded)
{
	// A 2 s master is below the 3 s target maximum, but its raw seed BPM is
	// below the policy floor.  DeduceSeedTiming halves to an actual BPM seed,
	// so BPI equals the number of seed gates in the master.
	QuantisationPolicy policy;

	auto timing = engine::Quantiser::DeduceSeedTiming(96000ul, 48000u, policy);

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
	QuantisationPolicy policy;

	auto timing = engine::Quantiser::DeduceSeedTiming(1536000ul, 48000u, policy);

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
	QuantisationPolicy policy;

	auto timing = engine::Quantiser::DeduceSeedTiming(352800ul, 44100u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(22050u, timing->SeedSamps);
	EXPECT_EQ(352800u, timing->MasterLoopSamps);
	EXPECT_EQ(16u, timing->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, SeedFromFirstFourBeatLoopDrawsFourGates)
{
	QuantisationPolicy policy;

	auto timing = engine::Quantiser::DeduceSeedTiming(124928ul, 44100u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(31232u, timing->SeedSamps);
	EXPECT_EQ(124928u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_NEAR(84.72077f, timing->Bpm, 0.0001f);
	EXPECT_EQ(4u, timing->Bpi);
}

TEST(Quantisation, SeedFromMasterRejectsZeroInputs)
{
	QuantisationPolicy policy;
	EXPECT_FALSE(engine::Quantiser::DeduceSeedTiming(0ul, 48000u, policy).has_value());
	EXPECT_FALSE(engine::Quantiser::DeduceSeedTiming(384000ul, 0u, policy).has_value());
}

// ---------------------------------------------------------------------------
// DeduceTapSeedTiming: seed sizes for common tap tempos (no master)
// ---------------------------------------------------------------------------

TEST(Quantisation, TapSeedAt120BpmNoMaster)
{
	// 120 BPM quarter note = 0.5 s = 24000 samps at 48 kHz.
	QuantisationPolicy policy;

	auto timing = engine::Quantiser::DeduceTapSeedTiming(24000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
}

TEST(Quantisation, TapSeedAt90BpmNoMaster)
{
	// 90 BPM quarter note = 32000 samps at 48 kHz.
	QuantisationPolicy policy;

	auto timing = engine::Quantiser::DeduceTapSeedTiming(32000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(32000u, timing->SeedSamps);
	EXPECT_FLOAT_EQ(90.0f, timing->Bpm);
}

TEST(Quantisation, TapSeedAt60BpmNoMaster)
{
	// 60 BPM quarter note = 48000 samps at 48 kHz.
	// Explicit tap timing preserves the physical seed interval.
	QuantisationPolicy policy;

	auto timing = engine::Quantiser::DeduceTapSeedTiming(48000ul, 48000u, policy);

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
	EXPECT_FLOAT_EQ(60.0f, timing->Bpm);
	EXPECT_EQ(4u, timing->Bpi);
}

TEST(Quantisation, TapTempoTrackerWithMasterIgnoresPolicySeedFloor)
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = 400u;
	TapTempoTracker tracker;

	const auto master = 48000ul;
	EXPECT_FALSE(tracker.TapAtSample(0ull, 48000u, master, policy).has_value());

	auto timing = tracker.TapAtSample(2000ull, 48000u, master, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(2000u, timing->SeedSamps);
	EXPECT_EQ(master, timing->MasterLoopSamps);
	EXPECT_EQ(24u, timing->SeedCount);
	EXPECT_FLOAT_EQ(1440.0f, timing->Bpm);
	EXPECT_EQ(24u, timing->Bpi);
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

	const auto interval = ninjam::IntervalSampsFromTempo(100.0f, 4u, sr);
	ASSERT_EQ(115200u, interval);

	// Derive seed directly as one beat: IntervalSampsFromTempo(bpm, 1, sr).
	const auto seed = ninjam::IntervalSampsFromTempo(100.0f, 1u, sr);
	ASSERT_EQ(28800u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(28800u, timingOpt->SeedSamps);
	EXPECT_EQ(4u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(100.0f, timingOpt->Bpm);
	EXPECT_EQ(4u, timingOpt->Bpi);
	EXPECT_EQ(interval, ninjam::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_8bpi)
{
	// 120 BPM, 8 BPI at 48 kHz → interval = 192000 samps.
	// Seed = one beat = 60*sr/BPM = 24000 samps.
	const unsigned int sr = 48000u;

	const auto interval = ninjam::IntervalSampsFromTempo(120.0f, 8u, sr);
	ASSERT_EQ(192000u, interval);

	const auto seed = ninjam::IntervalSampsFromTempo(120.0f, 1u, sr);
	ASSERT_EQ(24000u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(24000u, timingOpt->SeedSamps);
	EXPECT_EQ(8u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(8u, timingOpt->Bpi);
	EXPECT_EQ(interval, ninjam::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_180bpm_16bpi)
{
	// 180 BPM, 16 BPI at 48 kHz → interval = 256000 samps.
	// Seed = one beat = 60*sr/BPM = 16000 samps, so the original BPM and BPI
	// are recovered exactly.
	const unsigned int sr = 48000u;

	const auto interval = ninjam::IntervalSampsFromTempo(180.0f, 16u, sr);
	ASSERT_EQ(256000u, interval);

	const auto seed = ninjam::IntervalSampsFromTempo(180.0f, 1u, sr);
	ASSERT_EQ(16000u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(16000u, timingOpt->SeedSamps);
	EXPECT_EQ(16u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(180.0f, timingOpt->Bpm);
	EXPECT_EQ(16u, timingOpt->Bpi);
	EXPECT_EQ(interval, ninjam::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_16bpi_44100Hz)
{
	// 120 BPM, 16 BPI at 44100 Hz → interval = 352800 samps.
	// Seed = one beat = 60*sr/BPM = 22050 samps.
	const unsigned int sr = 44100u;

	const auto interval = ninjam::IntervalSampsFromTempo(120.0f, 16u, sr);
	ASSERT_EQ(352800u, interval);

	const auto seed = ninjam::IntervalSampsFromTempo(120.0f, 1u, sr);
	ASSERT_EQ(22050u, seed);

	auto timingOpt = engine::TimingFromSeedAndMaster(seed, interval, sr);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(22050u, timingOpt->SeedSamps);
	EXPECT_EQ(16u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(16u, timingOpt->Bpi);
	EXPECT_EQ(interval, ninjam::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

// ---------------------------------------------------------------------------
// DeduceTapSeedTimingFromMaster: snap tap gap to nearest whole divisor of
// master, with no seed-size limits.  Used when a tap-tempo estimate is applied
// while a master loop already exists.
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

	auto timing = engine::Quantiser::DeduceTapSeedTimingFromMaster(24000ul, 384000ul, 48000u);

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

	auto timing = engine::Quantiser::DeduceTapSeedTimingFromMaster(26000ul, 384000ul, 48000u);

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
	// DeduceTapSeedTiming would clamp to the 14400-sample no-master floor;
	// DeduceTapSeedTimingFromMaster must return 2000 (no size limit).

	auto timing = engine::Quantiser::DeduceTapSeedTimingFromMaster(2000ul, 48000ul, 48000u);

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

	auto timing = engine::Quantiser::DeduceTapSeedTimingFromMaster(240000ul, 480000ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(240000u, timing->SeedSamps);
	EXPECT_EQ(480000u, timing->MasterLoopSamps);
	EXPECT_EQ(2u, timing->SeedCount);
	EXPECT_FLOAT_EQ(12.0f, timing->Bpm);
	EXPECT_EQ(2u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterRejectsZeroInputs)
{
	EXPECT_FALSE(engine::Quantiser::DeduceTapSeedTimingFromMaster(0ul, 384000ul, 48000u).has_value());
	EXPECT_FALSE(engine::Quantiser::DeduceTapSeedTimingFromMaster(24000ul, 0ul, 48000u).has_value());
	EXPECT_FALSE(engine::Quantiser::DeduceTapSeedTimingFromMaster(24000ul, 384000ul, 0u).has_value());
}

TEST(QuantisationModel, GateGeometryBuildsHalfFrameInstanceMesh)
{
	auto singleGateVerts = engine::QuantisationModel::BuildGateGeometry(1u, 132.0f, 312.0f, 92.0f);
	auto repeatedGateVerts = engine::QuantisationModel::BuildGateGeometry(8u, 132.0f, 312.0f, 92.0f);
	ASSERT_FALSE(repeatedGateVerts.empty());
	EXPECT_EQ(singleGateVerts, repeatedGateVerts);
	EXPECT_EQ(16u * 6u * 3u, repeatedGateVerts.size());

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

TEST(Quantisation, TapSequenceRestartsAtTwoSecondsAndDropsOldSmoothing)
{
    TapTempoTracker tracker;
    engine::QuantisationPolicy policy;
    EXPECT_FALSE(tracker.TapAtSample(0u, 48000u, 288000ul, policy));
    ASSERT_TRUE(tracker.TapAtSample(48000u, 48000u, 288000ul, policy));
    ASSERT_TRUE(tracker.TapAtSample(72000u, 48000u, 288000ul, policy));
    EXPECT_DOUBLE_EQ(36000.0, tracker.EstimatedGapSamps());
    // Exactly two seconds expires the sequence; repeated lone taps do nothing.
    EXPECT_FALSE(tracker.TapAtSample(168000u, 48000u, 288000ul, policy));
    EXPECT_FALSE(tracker.HasEstimate());
    EXPECT_FALSE(tracker.TapAtSample(264001u, 48000u, 288000ul, policy));
    EXPECT_FALSE(tracker.HasEstimate());
    ASSERT_TRUE(tracker.TapAtSample(288001u, 48000u, 288000ul, policy));
    EXPECT_DOUBLE_EQ(24000.0, tracker.EstimatedGapSamps());
}

TEST(Quantisation, TapSequenceAcceptsGapJustUnderTwoSeconds)
{
    TapTempoTracker tracker;
    engine::QuantisationPolicy policy;
    EXPECT_FALSE(tracker.TapAtSample(0u, 48000u, 288000ul, policy));
    EXPECT_TRUE(tracker.TapAtSample(95999u, 48000u, 288000ul, policy));
}

TEST(Quantisation, OverlayWaitsTwoSecondsAfterReleaseBeforeFading)
{
    engine::Quantiser quantiser;
    quantiser.SetOverlayHeld(true);
    const auto after = utils::Timer::GetTime();
    EXPECT_FLOAT_EQ(1.0f, quantiser.OverlayAlpha(after + std::chrono::seconds(20)));
    const auto beforeRelease = utils::Timer::GetTime();
    quantiser.SetOverlayHeld(false);
    const auto released = utils::Timer::GetTime();
    EXPECT_FLOAT_EQ(1.0f, quantiser.OverlayAlpha(beforeRelease + std::chrono::seconds(2)));
    EXPECT_NEAR(0.5f, quantiser.OverlayAlpha(released + std::chrono::seconds(3)), 0.01f);
    EXPECT_FLOAT_EQ(0.0f, quantiser.OverlayAlpha(released + std::chrono::seconds(4)));
    quantiser.PulseOverlay();
    const auto pulse = utils::Timer::GetTime();
    EXPECT_FLOAT_EQ(1.0f, quantiser.OverlayAlpha(pulse + std::chrono::seconds(1)));
    EXPECT_NEAR(0.5f, quantiser.OverlayAlpha(pulse + std::chrono::seconds(3)), 0.01f);
    quantiser.ClearOverlay();
    EXPECT_FLOAT_EQ(0.0f, quantiser.OverlayAlpha(pulse));
}

TEST(Quantisation, InvalidOrNonIncreasingTapClearsSmoothing)
{
    engine::TapTempoTracker tracker;
    engine::QuantisationPolicy policy;
    tracker.TapAtSample(0u, 48000u, 96000ul, policy);
    ASSERT_TRUE(tracker.TapAtSample(24000u, 48000u, 96000ul, policy));
    EXPECT_FALSE(tracker.TapAtSample(24000u, 48000u, 96000ul, policy));
    EXPECT_FALSE(tracker.HasEstimate());
    EXPECT_FALSE(tracker.TapAtSample(48000u, 0u, 96000ul, policy));
    EXPECT_FALSE(tracker.HasEstimate());
}

TEST(Quantisation, FrozenGridCandidatesIncludeTripletsAndPreferSmallerTies)
{
    EXPECT_EQ(12u, engine::Quantiser::NearestPermittedDivision(4u, 12.0));
    EXPECT_EQ(8u, engine::Quantiser::NearestPermittedDivision(4u, 10.0));
    EXPECT_EQ(4u, engine::Quantiser::NearestPermittedDivision(4u, 0.0));
    EXPECT_EQ(128u, engine::Quantiser::NearestPermittedDivision(4u, 10000.0));
}

TEST(Quantisation, TapBaseGridComposesFractionWithoutChangingConstructionOrAuthority)
{
    midi::MidiQuantisationSettings settings;
    settings.Enabled = true;
    settings.GrainSamps = 24000u;
    settings.Fraction = midi::MidiQuantisationFraction::Third;
    settings.BaseIntervalSamps = 96000u;
    settings.BaseDivisions = 8u;
    EXPECT_EQ(96000u, settings.GridInterval());
    EXPECT_EQ(24u, settings.GridDivisions());
    EXPECT_EQ(24000u, settings.GrainSamps);
    settings.RemoteIntervalSamps = 192000u;
    settings.RemoteBpi = 16u;
    settings.RemoteOriginSamps = 501;
    EXPECT_EQ(192000u, settings.GridInterval());
    EXPECT_EQ(24u, settings.GridDivisions());
    EXPECT_TRUE(settings.HasRemoteGrid());
    const auto packed = settings.Pack();
    const auto restored = midi::MidiQuantisationSettings::Unpack(packed);
    EXPECT_EQ(settings.GrainSamps, restored.GrainSamps);
    EXPECT_EQ(settings.Fraction, restored.Fraction);
    EXPECT_FALSE(restored.HasBaseGrid());
}

class QuantisationContractTake : public engine::LoopTake
{
public:
    QuantisationContractTake(const engine::LoopTakeParams& params) :
        LoopTake(params, LoopTake::GetMixerParams(params.Size, audio::MergeMixBehaviourParams{})) {}
    void CompleteAudio(const std::vector<std::shared_ptr<engine::Loop>>& loops)
    {
        _loops = loops;
        _state.store(STATE_PLAYING, std::memory_order_release);
    }
    void AddMidi(const std::shared_ptr<midi::MidiLoop>& loop) { _midiLoops.push_back(loop); _PublishMidiLoopSnapshot(); }
    void CompleteMidi(unsigned long length)
    {
        _midiVisualLoopLength.store(length, std::memory_order_release);
        _state.store(STATE_PLAYING, std::memory_order_release);
    }
};

class QuantisationContractStation : public engine::Station
{
public:
    QuantisationContractStation(bool remote = false) :
        Station(engine::StationParams{}, engine::Station::GetMixerParams({ 100, 100 }, audio::MergeMixBehaviourParams{})), _remote(remote) {}
    bool IsRemote() const noexcept override { return _remote; }
private:
    bool _remote;
};

static std::shared_ptr<QuantisationContractTake> MakeQuantisationContractTake(const char* id, unsigned long length)
{
    engine::LoopTakeParams params;
    params.Id = id;
    params.Size = { 100, 100 };
    auto take = std::make_shared<QuantisationContractTake>(params);
    take->CompleteMidi(length);
    return take;
}

TEST(Quantisation, MidiGridsFollowTakePickerHeightsAndUseTakeHeight)
{
    auto first = MakeQuantisationContractTake("first", 96000ul);
    auto second = MakeQuantisationContractTake("second", 96000ul);
    first->SetModelPosition({ 0.0f, 100.0f, 0.0f });
    second->SetModelPosition({ 0.0f, 200.0f, 0.0f });
    for (const auto& take : { first, second })
    {
        auto loop = std::make_shared<midi::MidiLoop>();
        loop->StartRecord();
        loop->EndRecord(96000u);
        auto model = std::make_shared<graphics::MidiModel>(graphics::MidiModelParams{});
        model->SetModelPosition({ 0.0f, 0.0f, 0.0f });
        model->SetSize({ 80, 20 });
        loop->AttachModel(model);
        take->AddMidi(loop);
    }

    auto visuals = engine::LoopTake::QuantisationVisualsFor({ first, second });
    ASSERT_EQ(2u, visuals.size());
    EXPECT_FLOAT_EQ(100.0f, visuals[0].YCenter);
    EXPECT_FLOAT_EQ(200.0f, visuals[1].YCenter);
    EXPECT_FLOAT_EQ(45.0f, visuals[0].HalfHeight);
    EXPECT_FLOAT_EQ(45.0f, visuals[1].HalfHeight);
    EXPECT_FLOAT_EQ(10.0f, visuals[1].YCenter - visuals[1].HalfHeight
        - (visuals[0].YCenter + visuals[0].HalfHeight));

    // Resizing/repositioning a take must also update its grid geometry.
    second->SetSize({ 100, 160 });
    second->SetModelPosition({ 0.0f, 260.0f, 0.0f });
    visuals = engine::LoopTake::QuantisationVisualsFor({ first, second });
    ASSERT_EQ(2u, visuals.size());
    EXPECT_FLOAT_EQ(260.0f, visuals[1].YCenter);
    EXPECT_FLOAT_EQ(72.0f, visuals[1].HalfHeight);
}

TEST(Quantisation, SoleMidiTapPublishesGridWithoutRewritingSourceLength)
{
    auto take = MakeQuantisationContractTake("midi-master", 96001ul);
    auto station = std::make_shared<QuantisationContractStation>();
    station->AddTake(take);
    engine::Quantiser quantiser;
    auto clock = std::make_shared<utils::Timer>();
    quantiser.SetClock(clock);
    io::UserConfig config;
    quantiser.HandleTapTempo(0u, 48000u, { station }, config);
    quantiser.HandleTapTempo(24000u, 48000u, { station }, config);
    EXPECT_EQ(96001ul, take->VisualLoopLengthSamps());
    EXPECT_EQ(24000u, take->ResolvedMidiQuantisation().GrainSamps);
    EXPECT_EQ(96000u, take->ResolvedMidiQuantisation().BaseIntervalSamps);
    EXPECT_EQ(4u, take->ResolvedMidiQuantisation().BaseDivisions);
}

TEST(Quantisation, AdditionalMidiTakeTapPreservesTransportAndSelectsTripletSubdivision)
{
    auto first = MakeQuantisationContractTake("one", 96000ul);
    auto second = MakeQuantisationContractTake("two", 96000ul);
    auto station = std::make_shared<QuantisationContractStation>();
    station->AddTake(first);
    station->AddTake(second);
    engine::Quantiser quantiser;
    auto clock = std::make_shared<utils::Timer>();
    quantiser.SetClock(clock);
    quantiser.Set(24000u, utils::Timer::QUANTISE_MULTIPLE);
    quantiser.SetMidiGrain(24000u, "test", { station });
    clock->SetSeedSourceLength(96000ul);
    io::UserConfig config;
    quantiser.HandleTapTempo(0u, 48000u, { station }, config);
    quantiser.HandleTapTempo(8000u, 48000u, { station }, config);
    EXPECT_EQ(24000u, clock->QuantiseSamps());
    EXPECT_EQ(96000ul, clock->SeedSourceLength());
    EXPECT_EQ(12u, quantiser.ActiveGridDivisions());
    for (const auto& take : { first, second })
    {
        const auto settings = take->ResolvedMidiQuantisation();
        EXPECT_EQ(4u, settings.BaseDivisions);
        EXPECT_EQ(midi::MidiQuantisationFraction::Third, settings.Fraction);
        EXPECT_EQ(12u, settings.GridDivisions());
        EXPECT_EQ(24000u, settings.GrainSamps);
        EXPECT_EQ(96000ul, take->VisualLoopLengthSamps());
    }
    // A new, slower tap sequence changes only the subdivision again.
    quantiser.HandleTapTempo(200000u, 48000u, { station }, config);
    quantiser.HandleTapTempo(224000u, 48000u, { station }, config);
    for (const auto& take : { first, second })
    {
        const auto settings = take->ResolvedMidiQuantisation();
        EXPECT_EQ(4u, settings.BaseDivisions);
        EXPECT_EQ(midi::MidiQuantisationFraction::Whole, settings.Fraction);
        EXPECT_EQ(4u, settings.GridDivisions());
        EXPECT_EQ(96000ul, take->VisualLoopLengthSamps());
    }
    EXPECT_EQ(24000u, clock->QuantiseSamps());
    EXPECT_EQ(96000ul, clock->SeedSourceLength());
}

TEST(Quantisation, RemoteTapPreservesAcceptedDescriptorAcrossGrainPublication)
{
    auto take = MakeQuantisationContractTake("local", 96000ul);
    auto station = std::make_shared<QuantisationContractStation>();
    station->AddTake(take);
    engine::Quantiser quantiser;
    auto clock = std::make_shared<utils::Timer>();
    quantiser.SetClock(clock);
    quantiser.Set(24000u, utils::Timer::QUANTISE_MULTIPLE);
    clock->SetSeedSourceLength(96000ul);
    engine::RemoteTransportGeometry geometry;
    geometry.IntervalLengthSamps = 192000ul;
    geometry.Bpi = 8u;
    quantiser.SetRemoteMidiGrid(geometry, 431, { station });
    io::UserConfig config;
    quantiser.HandleTapTempo(0u, 48000u, { station }, config);
    quantiser.HandleTapTempo(8000u, 48000u, { station }, config);
    quantiser.SetMidiGrain(24000u, "activation", { station });
    quantiser.SetRemoteMidiGrid(geometry, 431, { station });
    const auto settings = take->ResolvedMidiQuantisation();
    EXPECT_EQ(192000u, settings.RemoteIntervalSamps);
    EXPECT_EQ(8u, settings.RemoteBpi);
    EXPECT_EQ(431, settings.RemoteOriginSamps);
    EXPECT_EQ(8u, settings.BaseDivisions);
    EXPECT_EQ(midi::MidiQuantisationFraction::Third, settings.Fraction);
    EXPECT_EQ(24u, settings.GridDivisions());
    EXPECT_EQ(96000ul, clock->SeedSourceLength());
}

TEST(Quantisation, TapSelectedBeatCountSurvivesIntegerGrainRounding)
{
    const auto timing = engine::Quantiser::DeduceTapSeedTimingFromMaster(11ul, 1000ul, 48000u);
    ASSERT_TRUE(timing);
    EXPECT_EQ(91u, timing->Bpi);
    EXPECT_EQ(10u, timing->SeedSamps);
    EXPECT_EQ(910u, timing->MasterLoopSamps);
}

TEST(Quantisation, MultichannelAudioAndMidiIsOneCompletedTake)
{
    auto take = MakeQuantisationContractTake("stereo-midi", 96000ul);
    std::vector<std::shared_ptr<engine::Loop>> loops;
    for (unsigned int channel = 0u; channel < 2u; ++channel)
    {
        engine::LoopParams params;
        params.Wav = "tap-test";
        params.Size = { 80, 80 };
        audio::WireMixBehaviourParams wire;
        wire.Channels = { channel };
        auto loop = std::make_shared<engine::Loop>(params, engine::Loop::GetMixerParams({ 80, 80 }, wire));
        loop->Record();
        std::vector<float> samples(constants::MaxLoopFadeSamps + 96000u, 0.25f);
        base::AudioWriteRequest request;
        request.samples = samples.data();
        request.numSamps = static_cast<unsigned int>(samples.size());
        request.stride = 1u;
        request.fadeCurrent = 0.0f;
        request.fadeNew = 1.0f;
        request.source = base::Audible::AUDIOSOURCE_ADC;
        loop->OnBlockWrite(request, 0);
        loop->EndWrite(request.numSamps, true);
        loop->Play(constants::MaxLoopFadeSamps, 96000ul, false);
        loops.push_back(loop);
    }
    take->CompleteAudio(loops);
    take->SetModelPosition({ 0.0f, 100.0f, 0.0f });
    auto sibling = MakeQuantisationContractTake("audio-sibling", 0ul);
    sibling->CompleteAudio(loops);
    sibling->SetModelPosition({ 0.0f, 200.0f, 0.0f });
    const auto visuals = engine::LoopTake::QuantisationVisualsFor({ take, sibling });
    ASSERT_EQ(2u, visuals.size());
    EXPECT_FLOAT_EQ(100.0f, visuals[0].YCenter);
    EXPECT_FLOAT_EQ(200.0f, visuals[1].YCenter);
    EXPECT_FLOAT_EQ(45.0f, visuals[0].HalfHeight);
    EXPECT_FLOAT_EQ(45.0f, visuals[1].HalfHeight);
    auto station = std::make_shared<QuantisationContractStation>();
    station->AddTake(take);
    engine::Quantiser quantiser;
    auto clock = std::make_shared<utils::Timer>();
    quantiser.SetClock(clock);
    io::UserConfig config;
    quantiser.HandleTapTempo(0u, 48000u, { station }, config);
    quantiser.HandleTapTempo(32000u, 48000u, { station }, config);
    EXPECT_EQ(32000u, clock->QuantiseSamps());
    EXPECT_EQ(3u, quantiser.ActiveGridDivisions());
    for (const auto& loop : loops)
    {
        EXPECT_EQ(96000ul, loop->LoopLength());
        EXPECT_EQ(96000ul, loop->PhysicalLoopLength());
    }
}

TEST(QuantisationController, RapidModifierTransitionsKeepCurrentOpacity)
{
	graphics::CtrlHandleOverlay overlay;
	engine::Quantiser quantiser;
	std::vector<std::shared_ptr<engine::Station>> stations;
	engine::QuantiserController controller(overlay, quantiser, stations);
	engine::QuantisationInteractionContext context;
	context.CursorPos = { 200, 200 };
	context.ViewportSize = { 800, 600 };
	const engine::QuantiserController::ChildResolver resolve = [](const auto&) { return std::shared_ptr<base::GuiElement>{}; };
	const auto start = utils::Timer::GetTime();
	controller.OnCtrlModifierChanged(true, start, context, resolve);
	controller.Tick(start + std::chrono::milliseconds(60));
	EXPECT_NEAR(0.5f, controller.PanelAlpha(), 0.001f);
	controller.OnCtrlModifierChanged(false, start + std::chrono::milliseconds(60), context, resolve);
	controller.Tick(start + std::chrono::milliseconds(60));
	EXPECT_NEAR(0.5f, controller.PanelAlpha(), 0.001f);
	controller.Tick(start + std::chrono::milliseconds(210));
	const auto fading = controller.PanelAlpha();
	controller.OnCtrlModifierChanged(true, start + std::chrono::milliseconds(210), context, resolve);
	controller.Tick(start + std::chrono::milliseconds(210));
	EXPECT_NEAR(fading, controller.PanelAlpha(), 0.001f);
	controller.Tick(start + std::chrono::milliseconds(330));
	EXPECT_FLOAT_EQ(1.0f, controller.PanelAlpha());
	EXPECT_LT(quantiser.OverlayAlpha(start + std::chrono::seconds(10)), 0.001f);
}

TEST(QuantisationController, CapturedSelectionSurvivesChangesAndCancelPreservesCtrlAndSpace)
{
	auto first = MakeQuantisationContractTake("captured", 96000ul);
	auto second = MakeQuantisationContractTake("hovered", 96000ul);
	auto station = std::make_shared<QuantisationContractStation>();
	station->AddTake(first);
	station->AddTake(second);
	first->Select();
	graphics::CtrlHandleOverlay overlay;
	engine::Quantiser quantiser;
	std::vector<std::shared_ptr<engine::Station>> stations = { station };
	engine::QuantiserController controller(overlay, quantiser, stations);
	engine::QuantisationInteractionContext context;
	context.SelectDepth = base::DEPTH_LOOPTAKE;
	context.CursorPos = { 200, 200 };
	context.ViewportSize = { 800, 600 };
	const engine::QuantiserController::ChildResolver resolve = [second](const auto&) { return second; };
	const auto start = utils::Timer::GetTime();
	controller.OnCtrlModifierChanged(true, start, context, resolve);
	first->DeSelect();
	second->Select();
	actions::TouchAction press;
	press.State = actions::TouchAction::TOUCH_DOWN;
	press.Index = 0;
	press.SetActionTime(start);
	press.Position = overlay.ButtonCenter(1).value();
	ASSERT_TRUE(controller.TryHandleTouchAction(press, 48000u, true, context, resolve)->IsEaten);
	controller.Tick(start);
	EXPECT_FLOAT_EQ(0.0f, controller.PanelAlpha()); // A press during entry must not jump to full alpha.
	actions::TouchMoveAction move;
	move.Position = press.Position;
	move.Position.Y += 32;
	controller.TryHandleTouchMove(move, 48000u);
	EXPECT_EQ(midi::MidiQuantisationFraction::Third, first->MidiQuantisation().Fraction);
	EXPECT_EQ(midi::MidiQuantisationFraction::Quarter, second->MidiQuantisation().Fraction);
	quantiser.SetOverlayHeld(true);
	controller.CancelInteraction();
	EXPECT_FALSE(controller.OwnsPointer());
	EXPECT_TRUE(controller.EditModeActive());
	EXPECT_FLOAT_EQ(1.0f, quantiser.OverlayAlpha(utils::Timer::GetTime() + std::chrono::seconds(20)));
	quantiser.SetOverlayHeld(false);
	EXPECT_FLOAT_EQ(0.0f, quantiser.OverlayAlpha(utils::Timer::GetTime() + std::chrono::seconds(20)));
	controller.CancelInteraction(true);
	EXPECT_FALSE(controller.EditModeActive());
}

TEST(QuantisationController, LoopDragChangesCapturedMidiStreamAndDeletionReleasesGesture)
{
	auto take = MakeQuantisationContractTake("midi-scope", 96000ul);
	auto first = std::make_shared<midi::MidiLoop>();
	auto sibling = std::make_shared<midi::MidiLoop>();
	take->AddMidi(first);
	take->AddMidi(sibling);
	auto station = std::make_shared<QuantisationContractStation>();
	station->AddTake(take);
	graphics::CtrlHandleOverlay overlay;
	engine::Quantiser quantiser;
	std::vector<std::shared_ptr<engine::Station>> stations = { station };
	engine::QuantiserController controller(overlay, quantiser, stations);
	engine::QuantisationInteractionContext context;
	context.SelectDepth = base::DEPTH_LOOP;
	context.SelectedMidiLoops = { first };
	context.HoveredMidiLoop = sibling;
	context.CursorPos = { 200, 200 };
	context.ViewportSize = { 800, 600 };
	const engine::QuantiserController::ChildResolver resolve = [](const auto&) { return std::shared_ptr<base::GuiElement>{}; };
	controller.OnCtrlModifierChanged(true, utils::Timer::GetTime(), context, resolve);
	actions::TouchAction press;
	press.State = actions::TouchAction::TOUCH_DOWN;
	press.Index = 0;
	press.Position = overlay.ButtonCenter(1).value();
	ASSERT_TRUE(controller.TryHandleTouchAction(press, 48000u, true, context, resolve)->IsEaten);
	actions::TouchMoveAction move;
	move.Position = press.Position;
	move.Position.Y += 32;
	controller.TryHandleTouchMove(move, 48000u);
	ASSERT_TRUE(first->GetLoopQuantisationOverride().Fraction.has_value());
	EXPECT_EQ(midi::MidiQuantisationFraction::Third, first->GetLoopQuantisationOverride().Fraction.value());
	EXPECT_FALSE(sibling->GetLoopQuantisationOverride().Fraction.has_value());
	stations.clear();
	controller.Tick(utils::Timer::GetTime());
	EXPECT_FALSE(controller.OwnsPointer());
	EXPECT_TRUE(controller.EditModeActive());
	EXPECT_FLOAT_EQ(0.0f, quantiser.OverlayAlpha(utils::Timer::GetTime() + std::chrono::seconds(20)));
}
