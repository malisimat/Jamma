#include <vector>

#include "gtest/gtest.h"
#include "midi/MidiEvent.h"
#include "midi/MidiLoop.h"
#include "midi/MidiNote.h"

using engine::MidiEvent;
using engine::MidiLoop;
using engine::MidiNote;

namespace
{
	std::vector<engine::MidiNote> Extract(const std::vector<MidiEvent>& events,
	                                          std::uint32_t loopLengthSamps)
	{
		return MidiNote::ExtractSpans(events.data(), events.size(), loopLengthSamps);
	}
}

TEST(MidiNote, PairsNoteOnAndNoteOffIntoDurationSpan)
{
	const std::vector<MidiEvent> events{
		MidiEvent::MakeNoteOn(100u, 2, 60, 96),
		MidiEvent::MakeNoteOff(340u, 2, 60)
	};

	const auto spans = Extract(events, 1000u);

	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(100u, spans[0].StartSample);
	EXPECT_EQ(240u, spans[0].DurationSamples);
	EXPECT_EQ(2u, spans[0].Channel);
	EXPECT_EQ(60u, spans[0].Note);
	EXPECT_EQ(96u, spans[0].Velocity);
}

TEST(MidiNote, TreatsVelocityZeroNoteOnAsNoteOff)
{
	const std::vector<MidiEvent> events{
		MidiEvent::MakeNoteOn(25u, 0, 64, 80),
		MidiEvent::MakeNoteOn(125u, 0, 64, 0)
	};

	const auto spans = Extract(events, 500u);

	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(25u, spans[0].StartSample);
	EXPECT_EQ(100u, spans[0].DurationSamples);
	EXPECT_EQ(64u, spans[0].Note);
	EXPECT_EQ(80u, spans[0].Velocity);
}

TEST(MidiNote, ClampsUnmatchedNoteOnToLoopEnd)
{
	const std::vector<MidiEvent> events{
		MidiEvent::MakeNoteOn(700u, 1, 72, 110)
	};

	const auto spans = Extract(events, 1000u);

	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(700u, spans[0].StartSample);
	EXPECT_EQ(300u, spans[0].DurationSamples);
	EXPECT_EQ(72u, spans[0].Note);
}

TEST(MidiNote, IgnoresEventsOutsidePlayableWindow)
{
	const std::vector<MidiEvent> events{
		MidiEvent::MakeNoteOn(100u, 0, 60, 70),
		MidiEvent::MakeNoteOff(1200u, 0, 60),
		MidiEvent::MakeNoteOn(1400u, 0, 65, 90),
		MidiEvent::MakeNoteOff(1500u, 0, 65)
	};

	const auto spans = Extract(events, 1000u);

	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(100u, spans[0].StartSample);
	EXPECT_EQ(900u, spans[0].DurationSamples);
	EXPECT_EQ(60u, spans[0].Note);
}

TEST(MidiNote, IgnoresNoteOffWithoutActiveNoteOn)
{
	const std::vector<MidiEvent> events{
		MidiEvent::MakeNoteOff(50u, 0, 60),
		MidiEvent::MakeNoteOn(100u, 0, 62, 90),
		MidiEvent::MakeNoteOff(200u, 0, 62)
	};

	const auto spans = Extract(events, 500u);

	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(62u, spans[0].Note);
	EXPECT_EQ(100u, spans[0].DurationSamples);
}

TEST(MidiNote, MatchesChannelAndPitchIndependently)
{
	const std::vector<MidiEvent> events{
		MidiEvent::MakeNoteOn(10u, 0, 60, 50),
		MidiEvent::MakeNoteOn(20u, 1, 60, 60),
		MidiEvent::MakeNoteOn(30u, 0, 61, 70),
		MidiEvent::MakeNoteOff(110u, 0, 60),
		MidiEvent::MakeNoteOff(220u, 1, 60),
		MidiEvent::MakeNoteOff(330u, 0, 61)
	};

	const auto spans = Extract(events, 500u);

	ASSERT_EQ(3u, spans.size());
	EXPECT_EQ(0u, spans[0].Channel);
	EXPECT_EQ(60u, spans[0].Note);
	EXPECT_EQ(100u, spans[0].DurationSamples);
	EXPECT_EQ(1u, spans[1].Channel);
	EXPECT_EQ(60u, spans[1].Note);
	EXPECT_EQ(200u, spans[1].DurationSamples);
	EXPECT_EQ(0u, spans[2].Channel);
	EXPECT_EQ(61u, spans[2].Note);
	EXPECT_EQ(300u, spans[2].DurationSamples);
}

TEST(MidiNote, OverlappingSameNoteStartsClosePreviousSpan)
{
	const std::vector<MidiEvent> events{
		MidiEvent::MakeNoteOn(100u, 0, 60, 40),
		MidiEvent::MakeNoteOn(180u, 0, 60, 90),
		MidiEvent::MakeNoteOff(260u, 0, 60)
	};

	const auto spans = Extract(events, 500u);

	ASSERT_EQ(2u, spans.size());
	EXPECT_EQ(100u, spans[0].StartSample);
	EXPECT_EQ(80u, spans[0].DurationSamples);
	EXPECT_EQ(40u, spans[0].Velocity);
	EXPECT_EQ(180u, spans[1].StartSample);
	EXPECT_EQ(80u, spans[1].DurationSamples);
	EXPECT_EQ(90u, spans[1].Velocity);
}

TEST(MidiNote, EmptyInputProducesNoSpans)
{
	const std::vector<MidiEvent> events;
	const auto spans = Extract(events, 1000u);
	EXPECT_TRUE(spans.empty());
}

TEST(MidiNote, DenseContentRemainsStable)
{
	std::vector<MidiEvent> events;
	events.reserve(MidiLoop::Capacity());

	for (std::uint32_t i = 0; i < MidiLoop::Capacity() / 2u; ++i)
	{
		const auto start = i * 2u;
		const auto note = static_cast<std::uint8_t>(i % 128u);
		events.push_back(MidiEvent::MakeNoteOn(start, 0, note, 100));
		events.push_back(MidiEvent::MakeNoteOff(start + 1u, 0, note));
	}

	const auto spans = Extract(events, static_cast<std::uint32_t>(MidiLoop::Capacity() + 10u));

	ASSERT_EQ(MidiLoop::Capacity() / 2u, spans.size());
	EXPECT_EQ(0u, spans.front().StartSample);
	EXPECT_EQ(1u, spans.front().DurationSamples);
	EXPECT_EQ((MidiLoop::Capacity() - 2u), spans.back().StartSample);
	EXPECT_EQ(1u, spans.back().DurationSamples);
}