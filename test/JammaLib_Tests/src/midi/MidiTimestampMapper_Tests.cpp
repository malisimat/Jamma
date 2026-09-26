#include <limits>

#include "gtest/gtest.h"

#include "midi/MidiBlockTiming.h"
#include "midi/MidiClockAnchor.h"
#include "midi/MidiTimestampMapper.h"

using midi::ClassifyMidiSampleInBlock;
using midi::MapMidiTimestampToAudioSample;
using midi::MidiDriverTimestampMapper;
using midi::MidiTimestampSource;
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

TEST(MidiTimestampMapper, EventBeforeAnchorMapsBackAndClampsAtZero)
{
	EXPECT_EQ(1234u, MapMidiTimestampToAudioSample(48000u, 1234u, 1000, 1000));
	EXPECT_EQ(1186u, MapMidiTimestampToAudioSample(48000u, 1234u, 1000000, 999000));
	EXPECT_EQ(0u, MapMidiTimestampToAudioSample(48000u, 12u, 1000000, 0));
}

TEST(MidiTimestampMapper, PriorAndFutureEventsPreserveOrderAcrossSampleCounterWrap)
{
	constexpr auto wrap = static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1ull;
	const auto anchor = wrap - 24ull;
	EXPECT_EQ(wrap - 72ull, MapMidiTimestampToAudioSample(48000u, anchor, 1000000, 999000));
	EXPECT_EQ(wrap + 24ull, MapMidiTimestampToAudioSample(48000u, anchor, 1000000, 1001000));
	EXPECT_EQ(24u, static_cast<std::uint32_t>(MapMidiTimestampToAudioSample(48000u,
		anchor, 1000000, 1001000)));
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

TEST(MidiDriverTimestampMapper, SeedsFirstEventAndPreservesDriverSpacingDespiteCallbackDelay)
{
	MidiDriverTimestampMapper mapper;
	const auto first = mapper.Map(0.0, 1000000);
	const auto same = mapper.Map(0.0, 1000400);
	const auto delayed = mapper.Map(0.010, 1030000);
	EXPECT_EQ(MidiTimestampSource::InitialArrival, first.Source);
	EXPECT_EQ(1000000, first.EventMicros);
	EXPECT_EQ(MidiTimestampSource::DriverDelta, same.Source);
	EXPECT_EQ(first.EventMicros, same.EventMicros);
	EXPECT_EQ(MidiTimestampSource::DriverDelta, delayed.Source);
	EXPECT_EQ(1010000, delayed.EventMicros);
}

TEST(MidiDriverTimestampMapper, InvalidAndDiscontinuousDeltasReseedWithoutReversingTime)
{
	MidiDriverTimestampMapper mapper;
	mapper.Map(0.0, 1000000);
	const auto invalid = mapper.Map((std::numeric_limits<double>::quiet_NaN)(), 1020000);
	const auto negative = mapper.Map(-0.001, 1021000);
	const auto huge = mapper.Map(61.0, 1022000);
	const auto future = mapper.Map(0.5, 1023000);
	const auto resumed = mapper.Map(0.001, 1024000);
	EXPECT_EQ(MidiTimestampSource::InvalidDeltaFallback, invalid.Source);
	EXPECT_EQ(1020000, invalid.EventMicros);
	EXPECT_EQ(MidiTimestampSource::InvalidDeltaFallback, negative.Source);
	EXPECT_EQ(1021000, negative.EventMicros);
	EXPECT_EQ(MidiTimestampSource::InvalidDeltaFallback, huge.Source);
	EXPECT_EQ(1022000, huge.EventMicros);
	EXPECT_EQ(MidiTimestampSource::DiscontinuityFallback, future.Source);
	EXPECT_EQ(1023000, future.EventMicros);
	EXPECT_EQ(MidiTimestampSource::DriverDelta, resumed.Source);
	EXPECT_EQ(1024000, resumed.EventMicros);
}

TEST(MidiDriverTimestampMapper, ResetStartsASeparateDeviceEpoch)
{
	MidiDriverTimestampMapper mapper;
	mapper.Map(0.0, 1000000);
	mapper.Map(0.010, 1010000);
	mapper.Reset();
	const auto reconnected = mapper.Map(0.0, 2000000);
	EXPECT_EQ(MidiTimestampSource::InitialArrival, reconnected.Source);
	EXPECT_EQ(2000000, reconnected.EventMicros);
}
