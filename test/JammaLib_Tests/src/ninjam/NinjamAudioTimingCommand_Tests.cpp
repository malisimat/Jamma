#include "gtest/gtest.h"
#include "ninjam/NinjamAudioTimingCommand.h"
#include "ninjam/NinjamLoopAlignment.h"
#include "utils/Timer.h"

using ninjam::NinjamDesiredTransportState;
using ninjam::NinjamDesiredTransportStateMailbox;
using ninjam::NinjamDesiredTimingIntent;
using utils::Timer;

namespace
{
	NinjamDesiredTransportState MakeDesired(std::uint64_t version,
		std::uint64_t generation,
		unsigned long seedLength,
		unsigned int absolutePhase)
	{
		NinjamDesiredTransportState desired;
		desired.Version = version;
		desired.SessionEpoch = 2u;
		desired.Generation = generation;
		desired.Intent = NinjamDesiredTimingIntent::Replacement;
		desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::ContinuousSync;
		desired.HasRemoteTiming = true;
		desired.IntervalLengthSamps = seedLength;
		desired.RemoteGridStepSamps = 100u;
		desired.Quantisation = Timer::QUANTISE_MULTIPLE;
		desired.RemotePhaseSamps = absolutePhase;
		desired.HasObservationSample = true;
		desired.ObservationSample = 12345u;
		return desired;
	}
}

// ── Mailbox: single audio-boundary consume semantics ─────────────────────────

TEST(NinjamDesiredTransportStateMailbox, EmptyMailboxReadsNothing)
{
	NinjamDesiredTransportStateMailbox mailbox;
	EXPECT_FALSE(mailbox.ReadLatest().has_value());
}

TEST(NinjamDesiredTransportStateMailbox, PublishedStateIsCompleteAndRepeatable)
{
	NinjamDesiredTransportStateMailbox mailbox;
	mailbox.Publish(MakeDesired(7u, 3u, 1000ul, 250u));

	const auto first = mailbox.ReadLatest();
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(NinjamDesiredTimingIntent::Replacement, first->Intent);
	EXPECT_EQ(7u, first->Version);
	EXPECT_EQ(2u, first->SessionEpoch);
	EXPECT_EQ(3u, first->Generation);
	EXPECT_EQ(1000ul, first->IntervalLengthSamps);
	EXPECT_EQ(250u, first->RemotePhaseSamps);
	EXPECT_EQ(12345u, first->ObservationSample);
	EXPECT_EQ(first->Version, mailbox.ReadLatest()->Version);
}

TEST(NinjamDesiredTransportStateMailbox, LatestPublicationWinsAcrossASingleBoundary)
{
	NinjamDesiredTransportStateMailbox mailbox;
	mailbox.Publish(MakeDesired(4u, 4u, 1000ul, 100u));
	mailbox.Publish(MakeDesired(5u, 5u, 2000ul, 900u));

	// Two publications between two callback boundaries collapse to the latest so
	// the audio thread never applies a superseded command (latest-wins).
	const auto latest = mailbox.ReadLatest();
	ASSERT_TRUE(latest.has_value());
	EXPECT_EQ(5u, latest->Version);
	EXPECT_EQ(5u, latest->Generation);
	EXPECT_EQ(2000ul, latest->IntervalLengthSamps);
	EXPECT_EQ(900u, latest->RemotePhaseSamps);
}

TEST(NinjamDesiredTransportStateMailbox, NoSyncPublicationIsACompleteLatestValue)
{
	NinjamDesiredTransportStateMailbox mailbox;
	NinjamDesiredTransportState noSync;
	noSync.Version = 8u;
	noSync.SessionEpoch = 3u;
	mailbox.Publish(noSync);

	const auto latest = mailbox.ReadLatest();
	ASSERT_TRUE(latest.has_value());
	EXPECT_EQ(NinjamDesiredTimingIntent::NoSync, latest->Intent);
	EXPECT_EQ(8u, latest->Version);
	EXPECT_EQ(3u, latest->SessionEpoch);
}

TEST(NinjamLoopAlignment, DistinctAnchorsRestoreExactlyAtSharedSceneCoordinates)
{
	const auto audioAnchor = ninjam::CaptureSceneAnchor(1000u, 125u, 300u);
	const auto midiAnchor = ninjam::CaptureSceneAnchor(1000u, 77u, 128u);
	EXPECT_EQ(0u, ninjam::SceneAlignmentResidual(
		ninjam::RestoreScenePhase(3400u, audioAnchor, 300u), 3400u, audioAnchor, 300u));
	EXPECT_EQ(0u, ninjam::SceneAlignmentResidual(
		ninjam::RestoreScenePhase(3400u, midiAnchor, 128u), 3400u, midiAnchor, 128u));
}

TEST(NinjamLoopAlignment, HandlesNegativeCoordinatesAndRejectsNonRecurringGrainMultiple)
{
	EXPECT_EQ(75u, ninjam::PositiveModulo(-25, 100u));
	EXPECT_TRUE(ninjam::IsRemoteTimingCompatible(200u, 100u, 1000u));
	EXPECT_FALSE(ninjam::IsRemoteTimingCompatible(300u, 100u, 1000u));
}

// ── Timer::ApplyCommand: audio-thread application and generation gating ───────

TEST(TimerApplyCommand, ReplaceTimingSetsSeedAndAbsolutePhase)
{
	Timer clock;
	Timer::Command command;
	command.Type = Timer::CommandType::ReplaceTiming;
	command.Generation = 1u;
	command.SeedLengthSamps = 1000ul;
	command.QuantiseSamps = 100u;
	command.Quantisation = Timer::QUANTISE_MULTIPLE;
	command.PhaseDeltaSamps = 250; // absolute phase for a replacement

	EXPECT_TRUE(clock.ApplyCommand(command));
	EXPECT_EQ(1000ul, clock.SeedSourceLength());
	EXPECT_EQ(250u, clock.SampOffset());
}

TEST(TimerApplyCommand, ZeroPhaseReplacementAtHigherGenerationStillApplies)
{
	// Regression for the zero-delta-at-generation>1 defect: a valid replacement
	// that happens to land on phase zero must not be dropped by the gate (§2.5).
	Timer clock;
	Timer::Command first;
	first.Type = Timer::CommandType::ReplaceTiming;
	first.Generation = 5u;
	first.SeedLengthSamps = 1000ul;
	first.PhaseDeltaSamps = 400;
	ASSERT_TRUE(clock.ApplyCommand(first));
	ASSERT_EQ(400u, clock.SampOffset());

	Timer::Command zeroPhase;
	zeroPhase.Type = Timer::CommandType::ReplaceTiming;
	zeroPhase.Generation = 6u;
	zeroPhase.SeedLengthSamps = 2000ul;
	zeroPhase.PhaseDeltaSamps = 0; // legitimate zero absolute phase
	EXPECT_TRUE(clock.ApplyCommand(zeroPhase));
	EXPECT_EQ(2000ul, clock.SeedSourceLength());
	EXPECT_EQ(0u, clock.SampOffset());
}

TEST(TimerApplyCommand, StaleGenerationCommandDoesNotMovePhase)
{
	Timer clock;
	Timer::Command current;
	current.Type = Timer::CommandType::ReplaceTiming;
	current.Generation = 10u;
	current.SeedLengthSamps = 1000ul;
	current.PhaseDeltaSamps = 100;
	ASSERT_TRUE(clock.ApplyCommand(current));
	ASSERT_EQ(100u, clock.SampOffset());

	Timer::Command stale;
	stale.Type = Timer::CommandType::PhaseCorrection;
	stale.Generation = 9u; // older than the audio generation
	stale.PhaseDeltaSamps = 500;
	EXPECT_TRUE(clock.ApplyCommand(stale));
	EXPECT_EQ(100u, clock.SampOffset()); // unchanged
}

TEST(TimerApplyCommand, InvalidateResetsGenerationGate)
{
	Timer clock;
	Timer::Command seed;
	seed.Type = Timer::CommandType::ReplaceTiming;
	seed.Generation = 10u;
	seed.SeedLengthSamps = 1000ul;
	seed.PhaseDeltaSamps = 100;
	ASSERT_TRUE(clock.ApplyCommand(seed));

	Timer::Command invalidate;
	invalidate.Type = Timer::CommandType::Invalidate;
	ASSERT_TRUE(clock.ApplyCommand(invalidate));

	// After invalidation the generation gate is reset, so a fresh low-generation
	// correction is accepted again.
	Timer::Command correction;
	correction.Type = Timer::CommandType::PhaseCorrection;
	correction.Generation = 1u;
	correction.PhaseDeltaSamps = 50;
	EXPECT_TRUE(clock.ApplyCommand(correction));
	EXPECT_EQ(150u, clock.SampOffset());
}

TEST(TimerApplyCommand, PhaseCorrectionWrapsCircularly)
{
	Timer clock;
	Timer::Command seed;
	seed.Type = Timer::CommandType::ReplaceTiming;
	seed.Generation = 1u;
	seed.SeedLengthSamps = 1000ul;
	seed.PhaseDeltaSamps = 900;
	ASSERT_TRUE(clock.ApplyCommand(seed));

	Timer::Command forward;
	forward.Type = Timer::CommandType::PhaseCorrection;
	forward.Generation = 2u;
	forward.PhaseDeltaSamps = 200; // 900 + 200 wraps to 100
	ASSERT_TRUE(clock.ApplyCommand(forward));
	EXPECT_EQ(100u, clock.SampOffset());

	Timer::Command backward;
	backward.Type = Timer::CommandType::PhaseCorrection;
	backward.Generation = 3u;
	backward.PhaseDeltaSamps = -300; // 100 - 300 wraps to 800
	ASSERT_TRUE(clock.ApplyCommand(backward));
	EXPECT_EQ(800u, clock.SampOffset());
}

TEST(TimerApplyCommand, EqualGenerationCommandDoesNotMovePhaseTwice)
{
	Timer clock;
	Timer::Command seed;
	seed.Type = Timer::CommandType::ReplaceTiming;
	seed.Generation = 1u;
	seed.SeedLengthSamps = 1000ul;
	seed.PhaseDeltaSamps = 100;
	ASSERT_TRUE(clock.ApplyCommand(seed));

	Timer::Command correction;
	correction.Type = Timer::CommandType::PhaseCorrection;
	correction.Generation = 2u;
	correction.PhaseDeltaSamps = 75;
	ASSERT_TRUE(clock.ApplyCommand(correction));
	ASSERT_EQ(175u, clock.SampOffset());
	EXPECT_TRUE(clock.ApplyCommand(correction));
	EXPECT_EQ(175u, clock.SampOffset());
}
