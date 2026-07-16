#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include "gtest/gtest.h"
#include "io/UserConfig.h"
#include "ninjam/NinjamConnection.h"
#include "timing/TimingQuantiser.h"
#include "graphics/QuantisationModel.h"
#include "midi/MidiQuantisation.h"
#include "utils/Timer.h"

using timing::QuantisationPolicy;
using timing::TapTempoTracker;

TEST(Quantisation, DerivesSeedTimingFromMasterLoop)
{
	QuantisationPolicy policy;

	auto timing = timing::DeduceSeedTiming(48000ul * 8ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(16u, timing->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
	EXPECT_EQ(16u, timing->Bpi);
}

TEST(Quantisation, EnforcesMinimumTapSeed)
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = 400u;

	auto timing = timing::DeduceTapSeedTiming(1000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(19200u, timing->SeedSamps);
	EXPECT_EQ(19200u, timing->MasterLoopSamps);
	EXPECT_EQ(1u, timing->SeedCount);
}

TEST(Quantisation, ConvertsNinjamTempoToIntervalSamples)
{
	EXPECT_EQ(384000u, timing::IntervalSampsFromTempo(120.0f, 16u, 48000u));
	EXPECT_EQ(0u, timing::IntervalSampsFromTempo(0.0f, 16u, 48000u));
	EXPECT_EQ(0u, timing::IntervalSampsFromTempo(120.0f, 0u, 48000u));
	EXPECT_EQ(0u, timing::IntervalSampsFromTempo(120.0f, 16u, 0u));
}

TEST(Quantisation, ResolvePhaseOffsetDragConvertsHorizontalPixelsToMilliseconds)
{
	EXPECT_EQ(2400, timing::ResolvePhaseOffsetDrag(0, 50, 48000u));
	EXPECT_EQ(-2400, timing::ResolvePhaseOffsetDrag(0, -50, 48000u));
	EXPECT_EQ(3400, timing::ResolvePhaseOffsetDrag(1000, 50, 48000u));
	EXPECT_EQ(1000, timing::ResolvePhaseOffsetDrag(1000, 50, 0u));
}

TEST(Quantisation, VisualCounts_FourGrainsQuarterProducesSixteenDivisions)
{
	timing::QuantisationLoopTakeVisual visual{};
	visual.LoopLengthSamps = 1600u;
	visual.GrainSamps = 400u;
	visual.LoopGrains = 4u;
	visual.Fraction = midi::MidiQuantisationFraction::Quarter;

	const auto counts = engine::QuantisationModel::ResolveVisualCounts(visual);
	EXPECT_EQ(4u, counts.GrainFrameCount);
	EXPECT_EQ(100u, counts.StepSamps);
	EXPECT_EQ(16u, counts.FractionDivisionCount);
}

TEST(Quantisation, VisualCounts_FourGrainsEighthProducesThirtyTwoDivisions)
{
	timing::QuantisationLoopTakeVisual visual{};
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
	timing::QuantisationLoopTakeVisual visual{};
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
	const auto timing = timing::TimingFromSeedAndMaster(96000u, 384000ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(96000u, timing->SeedSamps);
	EXPECT_EQ(384000u, timing->MasterLoopSamps);
	EXPECT_EQ(4u, timing->SeedCount);
	EXPECT_FLOAT_EQ(30.0f, timing->Bpm);
	EXPECT_EQ(4u, timing->Bpi);
}

TEST(Quantisation, TimingFromSeedAndMasterRejectsZeroInputs)
{
	EXPECT_FALSE(timing::TimingFromSeedAndMaster(0u, 384000ul, 48000u).has_value());
	EXPECT_FALSE(timing::TimingFromSeedAndMaster(96000u, 0ul, 48000u).has_value());
	EXPECT_FALSE(timing::TimingFromSeedAndMaster(96000u, 384000ul, 0u).has_value());
}

TEST(Quantisation, RemoteTempoProposalAndApplyRoundTrip)
{
	io::UserConfig cfg;
	timing::TimingQuantiser quantiser;
	quantiser.SetClock(std::make_shared<utils::Timer>());

	ninjam::NinjamRemoteSnapshot snapshot;
	snapshot.HasTiming = true;
	snapshot.SampleRate = 44100u;
	snapshot.IntervalLengthSamps = 352800u;
	snapshot.IntervalPositionSamps = 22050u;
	snapshot.Bpm = 120.0f;
	snapshot.Bpi = 16;

	auto proposal = quantiser.ProposeRemoteTempoChange(snapshot, cfg);
	ASSERT_TRUE(proposal.has_value());
	EXPECT_EQ(352800u, proposal->IntervalLengthSamps);
	EXPECT_EQ(22050u, proposal->GrainSamps);
	EXPECT_EQ(16u, proposal->Bpi);
	EXPECT_FLOAT_EQ(120.0f, proposal->Bpm);

	quantiser.ApplyAcceptedRemoteTempo(proposal.value(), {});

	// Same remote timing should not keep proposing once applied.
	EXPECT_FALSE(quantiser.ProposeRemoteTempoChange(snapshot, cfg).has_value());
}

TEST(Quantisation, AcceptedRemoteTempoUsesLocalSampleDomainAndReclocksExistingTransport)
{
	timing::TimingQuantiser quantiser;
	auto clock = std::make_shared<utils::Timer>();
	quantiser.SetClock(clock);
	clock->SetQuantisation(24000u, utils::Timer::QUANTISE_MULTIPLE);
	clock->SetSeedSourceLength(384000u);
	clock->Tick(100000u, 0u);

	timing::PendingRemoteTempoChange change;
	change.IntervalLengthSamps = 352800u;
	change.IntervalPositionSamps = 22050u;
	change.SampleRate = 44100u;
	change.GrainSamps = 22050u;
	change.MasterLoopLengthSamps = 352800u;
	change.Bpm = 120.0f;
	change.Bpi = 16u;

	EXPECT_EQ(-76000, quantiser.ApplyAcceptedRemoteTempo(change, {}, 48000u));
	EXPECT_EQ(384000u, clock->SeedSourceLength());
	EXPECT_EQ(24000u, clock->QuantiseSamps());
	EXPECT_EQ(24000u, clock->SampOffset());
}

TEST(Quantisation, ForceQueueCurrentTempoAsPendingBlocksRemoteProposal)
{
	io::UserConfig cfg;
	timing::TimingQuantiser quantiser;
	quantiser.SetClock(std::make_shared<utils::Timer>());

	ninjam::NinjamRemoteSnapshot seedSnapshot;
	seedSnapshot.HasTiming = true;
	seedSnapshot.SampleRate = 44100u;
	seedSnapshot.IntervalLengthSamps = 352800u;
	seedSnapshot.IntervalPositionSamps = 0u;
	seedSnapshot.Bpm = 120.0f;
	seedSnapshot.Bpi = 16;

	auto seedProposal = quantiser.ProposeRemoteTempoChange(seedSnapshot, cfg);
	ASSERT_TRUE(seedProposal.has_value());
	quantiser.ApplyAcceptedRemoteTempo(seedProposal.value(), {});

	EXPECT_TRUE(quantiser.ForceQueueCurrentTempoAsPending(true, 44100u));
	EXPECT_TRUE(quantiser.HasPendingTempo());

	ninjam::NinjamRemoteSnapshot nextSnapshot;
	nextSnapshot.HasTiming = true;
	nextSnapshot.SampleRate = 44100u;
	nextSnapshot.IntervalLengthSamps = 529200u;
	nextSnapshot.IntervalPositionSamps = 1024u;
	nextSnapshot.Bpm = 100.0f;
	nextSnapshot.Bpi = 20;
	EXPECT_FALSE(quantiser.ProposeRemoteTempoChange(nextSnapshot, cfg).has_value());

	quantiser.ResetPendingTempoSyncState();
	EXPECT_FALSE(quantiser.HasPendingTempo());
}

TEST(Quantisation, ForceQueueCurrentTempoAsPendingSeedsAcceptedRemoteTempo)
{
	io::UserConfig cfg;
	timing::TimingQuantiser quantiser;
	auto clock = std::make_shared<utils::Timer>();
	quantiser.SetClock(clock);

	clock->SetQuantisation(22050u, utils::Timer::QUANTISE_MULTIPLE);
	clock->SetSeedSourceLength(352800u);
	quantiser.QueueLocalTempo(0u, 44100u, cfg);
	ASSERT_TRUE(quantiser.HasPendingTempo());

	EXPECT_TRUE(quantiser.ForceQueueCurrentTempoAsPending(true, 44100u));
	quantiser.ResetPendingTempoSyncState();
	EXPECT_FALSE(quantiser.HasPendingTempo());

	ninjam::NinjamRemoteSnapshot snapshot;
	snapshot.HasTiming = true;
	snapshot.SampleRate = 44100u;
	snapshot.IntervalLengthSamps = 352800u;
	snapshot.IntervalPositionSamps = 0u;
	snapshot.Bpm = 120.0f;
	snapshot.Bpi = 16;

	EXPECT_FALSE(quantiser.ProposeRemoteTempoChange(snapshot, cfg).has_value());
}

TEST(Quantisation, ForceQueueCurrentTempoAsPendingRequiresExistingTempo)
{
	timing::TimingQuantiser quantiser;
	quantiser.SetClock(std::make_shared<utils::Timer>());
	EXPECT_FALSE(quantiser.ForceQueueCurrentTempoAsPending(true, 48000u));
}

TEST(Quantisation, LocallyRequestedRemoteTempoAcknowledgementPreservesLocalClockDomain)
{
	timing::TimingQuantiser quantiser;
	auto clock = std::make_shared<utils::Timer>();
	quantiser.SetClock(clock);
	clock->SetQuantisation(24000u, utils::Timer::QUANTISE_MULTIPLE);
	clock->SetSeedSourceLength(384000u);

	timing::PendingRemoteTempoChange acknowledged;
	acknowledged.IntervalLengthSamps = 352800u;
	acknowledged.SampleRate = 44100u;
	acknowledged.GrainSamps = 22050u;
	acknowledged.MasterLoopLengthSamps = 352800u;
	acknowledged.Bpm = 120.0f;
	acknowledged.Bpi = 16u;
	quantiser.AcknowledgeLocallyRequestedRemoteTempo(acknowledged);

	EXPECT_EQ(384000u, clock->SeedSourceLength());
	EXPECT_EQ(24000u, clock->QuantiseSamps());
	EXPECT_EQ(44100u, quantiser.RemoteSampleRate());
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

	auto timing = timing::DeduceSeedTiming(96000ul, 48000u, policy);

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

	auto timing = timing::DeduceSeedTiming(1536000ul, 48000u, policy);

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

	auto timing = timing::DeduceSeedTiming(352800ul, 44100u, policy);

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

	auto timing = timing::DeduceSeedTiming(124928ul, 44100u, policy);

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
	EXPECT_FALSE(timing::DeduceSeedTiming(0ul, 48000u, policy).has_value());
	EXPECT_FALSE(timing::DeduceSeedTiming(384000ul, 0u, policy).has_value());
}

// ---------------------------------------------------------------------------
// DeduceTapSeedTiming: seed sizes for common tap tempos (no master)
// ---------------------------------------------------------------------------

TEST(Quantisation, TapSeedAt120BpmNoMaster)
{
	// 120 BPM quarter note = 0.5 s = 24000 samps at 48 kHz.
	QuantisationPolicy policy;

	auto timing = timing::DeduceTapSeedTiming(24000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(24000u, timing->SeedSamps);
	EXPECT_FLOAT_EQ(120.0f, timing->Bpm);
}

TEST(Quantisation, TapSeedAt90BpmNoMaster)
{
	// 90 BPM quarter note = 32000 samps at 48 kHz.
	QuantisationPolicy policy;

	auto timing = timing::DeduceTapSeedTiming(32000ul, 48000u, policy);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(32000u, timing->SeedSamps);
	EXPECT_FLOAT_EQ(90.0f, timing->Bpm);
}

TEST(Quantisation, TapSeedAt60BpmNoMaster)
{
	// 60 BPM quarter note = 48000 samps at 48 kHz.
	// Explicit tap timing preserves the physical seed interval.
	QuantisationPolicy policy;

	auto timing = timing::DeduceTapSeedTiming(48000ul, 48000u, policy);

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

	const auto interval = timing::IntervalSampsFromTempo(100.0f, 4u, sr);
	ASSERT_EQ(115200u, interval);

	// Derive seed directly as one beat: IntervalSampsFromTempo(bpm, 1, sr).
	const auto seed = timing::IntervalSampsFromTempo(100.0f, 1u, sr);
	ASSERT_EQ(28800u, seed);

	auto timingOpt = timing::TimingFromSeedAndMaster(seed, interval, sr);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(28800u, timingOpt->SeedSamps);
	EXPECT_EQ(4u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(100.0f, timingOpt->Bpm);
	EXPECT_EQ(4u, timingOpt->Bpi);
	EXPECT_EQ(interval, timing::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_8bpi)
{
	// 120 BPM, 8 BPI at 48 kHz → interval = 192000 samps.
	// Seed = one beat = 60*sr/BPM = 24000 samps.
	const unsigned int sr = 48000u;

	const auto interval = timing::IntervalSampsFromTempo(120.0f, 8u, sr);
	ASSERT_EQ(192000u, interval);

	const auto seed = timing::IntervalSampsFromTempo(120.0f, 1u, sr);
	ASSERT_EQ(24000u, seed);

	auto timingOpt = timing::TimingFromSeedAndMaster(seed, interval, sr);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(24000u, timingOpt->SeedSamps);
	EXPECT_EQ(8u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(8u, timingOpt->Bpi);
	EXPECT_EQ(interval, timing::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_180bpm_16bpi)
{
	// 180 BPM, 16 BPI at 48 kHz → interval = 256000 samps.
	// Seed = one beat = 60*sr/BPM = 16000 samps, so the original BPM and BPI
	// are recovered exactly.
	const unsigned int sr = 48000u;

	const auto interval = timing::IntervalSampsFromTempo(180.0f, 16u, sr);
	ASSERT_EQ(256000u, interval);

	const auto seed = timing::IntervalSampsFromTempo(180.0f, 1u, sr);
	ASSERT_EQ(16000u, seed);

	auto timingOpt = timing::TimingFromSeedAndMaster(seed, interval, sr);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(16000u, timingOpt->SeedSamps);
	EXPECT_EQ(16u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(180.0f, timingOpt->Bpm);
	EXPECT_EQ(16u, timingOpt->Bpi);
	EXPECT_EQ(interval, timing::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
}

TEST(Quantisation, NinjamRoundTrip_120bpm_16bpi_44100Hz)
{
	// 120 BPM, 16 BPI at 44100 Hz → interval = 352800 samps.
	// Seed = one beat = 60*sr/BPM = 22050 samps.
	const unsigned int sr = 44100u;

	const auto interval = timing::IntervalSampsFromTempo(120.0f, 16u, sr);
	ASSERT_EQ(352800u, interval);

	const auto seed = timing::IntervalSampsFromTempo(120.0f, 1u, sr);
	ASSERT_EQ(22050u, seed);

	auto timingOpt = timing::TimingFromSeedAndMaster(seed, interval, sr);
	ASSERT_TRUE(timingOpt.has_value());
	EXPECT_EQ(22050u, timingOpt->SeedSamps);
	EXPECT_EQ(16u, timingOpt->SeedCount);
	EXPECT_FLOAT_EQ(120.0f, timingOpt->Bpm);
	EXPECT_EQ(16u, timingOpt->Bpi);
	EXPECT_EQ(interval, timing::IntervalSampsFromTempo(timingOpt->Bpm, timingOpt->Bpi, sr));
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

	auto timing = timing::DeduceTapSeedTimingFromMaster(24000ul, 384000ul, 48000u);

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

	auto timing = timing::DeduceTapSeedTimingFromMaster(26000ul, 384000ul, 48000u);

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

	auto timing = timing::DeduceTapSeedTimingFromMaster(2000ul, 48000ul, 48000u);

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

	auto timing = timing::DeduceTapSeedTimingFromMaster(240000ul, 480000ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	EXPECT_EQ(240000u, timing->SeedSamps);
	EXPECT_EQ(480000u, timing->MasterLoopSamps);
	EXPECT_EQ(2u, timing->SeedCount);
	EXPECT_FLOAT_EQ(12.0f, timing->Bpm);
	EXPECT_EQ(2u, timing->Bpi);
}

TEST(Quantisation, TapSeedFromMasterRejectsZeroInputs)
{
	EXPECT_FALSE(timing::DeduceTapSeedTimingFromMaster(0ul, 384000ul, 48000u).has_value());
	EXPECT_FALSE(timing::DeduceTapSeedTimingFromMaster(24000ul, 0ul, 48000u).has_value());
	EXPECT_FALSE(timing::DeduceTapSeedTimingFromMaster(24000ul, 384000ul, 0u).has_value());
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
