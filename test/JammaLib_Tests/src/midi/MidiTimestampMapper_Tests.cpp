#include <limits>

#include "gtest/gtest.h"

#include "midi/MidiTimestampMapper.h"

using engine::MapMidiTimestampToAudioSample;

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