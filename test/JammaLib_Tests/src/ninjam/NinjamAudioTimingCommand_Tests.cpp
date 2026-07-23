#include "gtest/gtest.h"
#include "ninjam/NinjamAudioTimingCommand.h"
#include "utils/Timer.h"

using ninjam::NinjamAudioTimingCommand;
using ninjam::NinjamAudioTimingCommandMailbox;
using ninjam::NinjamTimingCommandType;
using utils::Timer;

namespace
{
	NinjamAudioTimingCommand MakeReplace(std::uint64_t generation,
		unsigned long seedLength,
		unsigned int absolutePhase)
	{
		NinjamAudioTimingCommand command;
		command.Type = NinjamTimingCommandType::ReplaceTiming;
		command.Generation = generation;
		command.SeedLengthSamps = seedLength;
		command.QuantiseSamps = 100u;
		command.Quantisation = Timer::QUANTISE_MULTIPLE;
		command.AbsolutePhaseSamps = absolutePhase;
		command.PhaseObservationSample = 12345u;
		return command;
	}
}

// ── Mailbox: single audio-boundary consume semantics ─────────────────────────

TEST(NinjamAudioTimingCommandMailbox, EmptyMailboxConsumesNothing)
{
	NinjamAudioTimingCommandMailbox mailbox;
	EXPECT_FALSE(mailbox.Consume().has_value());
}

TEST(NinjamAudioTimingCommandMailbox, PublishedCommandIsConsumedExactlyOnce)
{
	NinjamAudioTimingCommandMailbox mailbox;
	mailbox.Publish(MakeReplace(3u, 1000ul, 250u));

	const auto first = mailbox.Consume();
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(NinjamTimingCommandType::ReplaceTiming, first->Type);
	EXPECT_EQ(3u, first->Generation);
	EXPECT_EQ(1000ul, first->SeedLengthSamps);
	EXPECT_EQ(250u, first->AbsolutePhaseSamps);
	EXPECT_EQ(12345u, first->PhaseObservationSample);

	// A second consume with no intervening publication must move nothing.
	EXPECT_FALSE(mailbox.Consume().has_value());
}

TEST(NinjamAudioTimingCommandMailbox, LatestPublicationWinsAcrossASingleBoundary)
{
	NinjamAudioTimingCommandMailbox mailbox;
	mailbox.Publish(MakeReplace(4u, 1000ul, 100u));
	mailbox.Publish(MakeReplace(5u, 2000ul, 900u));

	// Two publications between two callback boundaries collapse to the latest so
	// the audio thread never applies a superseded command (latest-wins).
	const auto consumed = mailbox.Consume();
	ASSERT_TRUE(consumed.has_value());
	EXPECT_EQ(5u, consumed->Generation);
	EXPECT_EQ(2000ul, consumed->SeedLengthSamps);
	EXPECT_EQ(900u, consumed->AbsolutePhaseSamps);
	EXPECT_FALSE(mailbox.Consume().has_value());
}

TEST(NinjamAudioTimingCommandMailbox, InvalidatePublicationIsDeliveredToConsumer)
{
	NinjamAudioTimingCommandMailbox mailbox;
	NinjamAudioTimingCommand invalidate;
	invalidate.Type = NinjamTimingCommandType::Invalidate;
	mailbox.Publish(invalidate);

	const auto consumed = mailbox.Consume();
	ASSERT_TRUE(consumed.has_value());
	EXPECT_EQ(NinjamTimingCommandType::Invalidate, consumed->Type);
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
	forward.Generation = 1u;
	forward.PhaseDeltaSamps = 200; // 900 + 200 wraps to 100
	ASSERT_TRUE(clock.ApplyCommand(forward));
	EXPECT_EQ(100u, clock.SampOffset());

	Timer::Command backward;
	backward.Type = Timer::CommandType::PhaseCorrection;
	backward.Generation = 1u;
	backward.PhaseDeltaSamps = -300; // 100 - 300 wraps to 800
	ASSERT_TRUE(clock.ApplyCommand(backward));
	EXPECT_EQ(800u, clock.SampOffset());
}
