#include <limits>

#include "gtest/gtest.h"

#include "midi/MidiBlockTiming.h"
#include "midi/MidiClockAnchor.h"
#include "midi/MidiTimestampMapper.h"

using midi::ClassifyMidiSampleInBlock;
using midi::MapMidiTimestampToAudioSample;
using midi::MidiBlockSamplePosition;
using midi::MidiClockAnchor;
using midi::MidiClockAnchorSnapshot;
using midi::RebaseMidiSampleForVstBlock;

TEST(MidiTimestampMapper, MapsMicrosAfterAnchorToSampleCounter)
{
	EXPECT_EQ(148000u, MapMidiTimestampToAudioSample(48000u, 100000u, 1000, 1001000));
}

TEST(MidiTimestampMapper, ZeroSampleRateReturnsAnchor)
{
	EXPECT_EQ(1234u, MapMidiTimestampToAudioSample(0u, 1234u, 1000, 2000));
}

TEST(MidiTimestampMapper, EventAtOrBeforeAnchorReturnsAnchor)
{
	EXPECT_EQ(1234u, MapMidiTimestampToAudioSample(48000u, 1234u, 1000, 1000));
	EXPECT_EQ(1234u, MapMidiTimestampToAudioSample(48000u, 1234u, 1000, 999));
}

TEST(MidiTimestampMapper, OverflowSaturatesInsteadOfWrapping)
{
	const auto max = (std::numeric_limits<std::uint64_t>::max)();
	EXPECT_EQ(max, MapMidiTimestampToAudioSample(48000u, max - 10u, 0, 1000000));
	EXPECT_EQ(max, MapMidiTimestampToAudioSample((std::numeric_limits<unsigned int>::max)(),
		0u,
		0,
		(std::numeric_limits<std::int64_t>::max)()));
}

TEST(MidiBlockTiming, ClassifiesBlockEdgesAndRebasesDueEvents)
{
	constexpr auto rawBlockStart = 1000u;
	constexpr auto shiftedBlockStart = 5000u;
	constexpr auto numSamples = 64u;

	EXPECT_EQ(MidiBlockSamplePosition::Late,
		ClassifyMidiSampleInBlock(rawBlockStart - 1u, rawBlockStart, numSamples));
	EXPECT_EQ(MidiBlockSamplePosition::Due,
		ClassifyMidiSampleInBlock(rawBlockStart, rawBlockStart, numSamples));
	EXPECT_EQ(MidiBlockSamplePosition::Due,
		ClassifyMidiSampleInBlock(rawBlockStart + numSamples - 1u, rawBlockStart, numSamples));
	EXPECT_EQ(MidiBlockSamplePosition::Future,
		ClassifyMidiSampleInBlock(rawBlockStart + numSamples, rawBlockStart, numSamples));

	EXPECT_EQ(shiftedBlockStart,
		RebaseMidiSampleForVstBlock(rawBlockStart - 1u, rawBlockStart, shiftedBlockStart));
	EXPECT_EQ(shiftedBlockStart + numSamples - 1u,
		RebaseMidiSampleForVstBlock(rawBlockStart + numSamples - 1u, rawBlockStart, shiftedBlockStart));
}

TEST(MidiBlockTiming, ClassifiesBlockCrossingSampleCounterWrap)
{
	constexpr auto rawBlockStart = (std::numeric_limits<std::uint32_t>::max)() - 31u;
	constexpr auto numSamples = 64u;

	EXPECT_EQ(MidiBlockSamplePosition::Late,
		ClassifyMidiSampleInBlock(rawBlockStart - 1u, rawBlockStart, numSamples));
	EXPECT_EQ(MidiBlockSamplePosition::Due,
		ClassifyMidiSampleInBlock(16u, rawBlockStart, numSamples));
	EXPECT_EQ(MidiBlockSamplePosition::Future,
		ClassifyMidiSampleInBlock(32u, rawBlockStart, numSamples));
}

TEST(MidiClockAnchor, ReadsOnePublishedGeneration)
{
	MidiClockAnchor anchor;
	midi::PublishMidiClockAnchor(anchor, 987654u, 1234567);

	const auto snapshot = midi::ReadMidiClockAnchor(anchor, {});
	EXPECT_EQ(987654u, snapshot.Sample);
	EXPECT_EQ(1234567, snapshot.SteadyMicros);
}

TEST(MidiClockAnchor, FallsBackWhenWriterIsInProgress)
{
	MidiClockAnchor anchor;
	anchor.Sequence.store(1u, std::memory_order_release);
	anchor.Sample.store(1000u, std::memory_order_relaxed);
	anchor.SteadyMicros.store(2000, std::memory_order_relaxed);

	const MidiClockAnchorSnapshot fallback{ 11u, 22 };
	const auto snapshot = midi::ReadMidiClockAnchor(anchor, fallback);
	EXPECT_EQ(fallback.Sample, snapshot.Sample);
	EXPECT_EQ(fallback.SteadyMicros, snapshot.SteadyMicros);
}