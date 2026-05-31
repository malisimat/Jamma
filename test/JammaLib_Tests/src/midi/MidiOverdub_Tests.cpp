#include <array>
#include <vector>

#include "gtest/gtest.h"
#include "midi/MidiEvent.h"
#include "midi/MidiOverdub.h"

using engine::MidiEvent;
using engine::MidiOverdubRenderParams;
using engine::MidiPunchWindow;

namespace
{
	struct EventView
	{
		std::uint32_t Offset;
		std::uint8_t Status;
		std::uint8_t Note;
		std::uint8_t Velocity;
	};

	std::vector<EventView> BuildEvents(const std::vector<MidiEvent>& sourceEvents,
		std::uint32_t sourceLoopLength,
		std::uint32_t targetLoopLength,
		const std::vector<MidiPunchWindow>& punchWindows,
		std::size_t outCapacity = 128u)
	{
		std::vector<MidiEvent> output(outCapacity);
		MidiOverdubRenderParams params;
		params.SourceEvents = sourceEvents.data();
		params.SourceEventCount = sourceEvents.size();
		params.SourceLoopLengthSamps = sourceLoopLength;
		params.TargetLoopLengthSamps = targetLoopLength;
		params.PunchWindows = punchWindows.data();
		params.PunchWindowCount = punchWindows.size();

		const auto eventCount = engine::BuildMidiOverdubBaseEvents(params, output.data(), output.size());
		std::vector<EventView> built;
		built.reserve(eventCount);
		for (std::size_t i = 0u; i < eventCount; ++i)
		{
			built.push_back({
				output[i].sampleOffset,
				output[i].status,
				output[i].data1,
				output[i].data2
				});
		}

		return built;
	}
}

TEST(MidiOverdub, CopiesSourceOutsideSinglePunchWindow)
{
	const auto events = BuildEvents({
		MidiEvent::MakeNoteOn(2u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(8u, 0u, 60u),
		MidiEvent::MakeNoteOn(60u, 0u, 62u, 90u),
		MidiEvent::MakeNoteOff(80u, 0u, 62u),
		},
		100u,
		100u,
		{ MidiPunchWindow{20u, 40u} });

	ASSERT_EQ(4u, events.size());
	EXPECT_EQ(2u, events[0].Offset);
	EXPECT_TRUE((events[0].Status & MidiEvent::StatusMask) == MidiEvent::NoteOn);
	EXPECT_EQ(8u, events[1].Offset);
	EXPECT_TRUE((events[1].Status & MidiEvent::StatusMask) == MidiEvent::NoteOff);
	EXPECT_EQ(60u, events[2].Offset);
	EXPECT_EQ(80u, events[3].Offset);
}

TEST(MidiOverdub, CutsSourceNoteAtPunchIn)
{
	const auto events = BuildEvents({
		MidiEvent::MakeNoteOn(10u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(90u, 0u, 60u),
		},
		100u,
		100u,
		{ MidiPunchWindow{20u, 40u} });

	ASSERT_EQ(4u, events.size());
	EXPECT_EQ(10u, events[0].Offset);
	EXPECT_TRUE((events[0].Status & MidiEvent::StatusMask) == MidiEvent::NoteOn);
	EXPECT_EQ(20u, events[1].Offset);
	EXPECT_TRUE((events[1].Status & MidiEvent::StatusMask) == MidiEvent::NoteOff);
}

TEST(MidiOverdub, RestartsSourceNoteAtPunchOut)
{
	const auto events = BuildEvents({
		MidiEvent::MakeNoteOn(10u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(90u, 0u, 60u),
		},
		100u,
		100u,
		{ MidiPunchWindow{20u, 40u} });

	ASSERT_EQ(4u, events.size());
	EXPECT_EQ(40u, events[2].Offset);
	EXPECT_TRUE((events[2].Status & MidiEvent::StatusMask) == MidiEvent::NoteOn);
	EXPECT_EQ(90u, events[3].Offset);
	EXPECT_TRUE((events[3].Status & MidiEvent::StatusMask) == MidiEvent::NoteOff);
}

TEST(MidiOverdub, ErasesSourceNoteFullyInsidePunchWindow)
{
	const auto events = BuildEvents({
		MidiEvent::MakeNoteOn(25u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(30u, 0u, 60u),
		},
		100u,
		100u,
		{ MidiPunchWindow{20u, 40u} });

	EXPECT_TRUE(events.empty());
}

TEST(MidiOverdub, RepeatsSourceAcrossLongerTargetLoop)
{
	const auto events = BuildEvents({
		MidiEvent::MakeNoteOn(10u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(20u, 0u, 60u),
		},
		100u,
		250u,
		{});

	ASSERT_EQ(6u, events.size());
	EXPECT_EQ(10u, events[0].Offset);
	EXPECT_EQ(20u, events[1].Offset);
	EXPECT_EQ(110u, events[2].Offset);
	EXPECT_EQ(120u, events[3].Offset);
	EXPECT_EQ(210u, events[4].Offset);
	EXPECT_EQ(220u, events[5].Offset);
}

TEST(MidiOverdub, ClipsSourceToShorterTargetLoop)
{
	const auto events = BuildEvents({
		MidiEvent::MakeNoteOn(50u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(90u, 0u, 60u),
		},
		100u,
		60u,
		{});

	ASSERT_EQ(2u, events.size());
	EXPECT_EQ(50u, events[0].Offset);
	EXPECT_TRUE((events[0].Status & MidiEvent::StatusMask) == MidiEvent::NoteOn);
	EXPECT_EQ(60u, events[1].Offset);
	EXPECT_TRUE((events[1].Status & MidiEvent::StatusMask) == MidiEvent::NoteOff);
}

TEST(MidiOverdub, HandlesVelocityZeroNoteOnAsNoteOff)
{
	const auto events = BuildEvents({
		MidiEvent::MakeNoteOn(10u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOn(30u, 0u, 60u, 0u),
		},
		100u,
		100u,
		{});

	ASSERT_EQ(2u, events.size());
	EXPECT_EQ(10u, events[0].Offset);
	EXPECT_EQ(30u, events[1].Offset);
	EXPECT_TRUE((events[1].Status & MidiEvent::StatusMask) == MidiEvent::NoteOff);
}

TEST(MidiOverdub, CanonicalOrderingSendsNoteOffBeforeNoteOnAtBoundary)
{
	const auto events = BuildEvents({
		MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u),
		},
		100u,
		210u,
		{});

	ASSERT_GE(events.size(), 5u);
	EXPECT_EQ(100u, events[1].Offset);
	EXPECT_TRUE((events[1].Status & MidiEvent::StatusMask) == MidiEvent::NoteOff);
	EXPECT_EQ(100u, events[2].Offset);
	EXPECT_TRUE((events[2].Status & MidiEvent::StatusMask) == MidiEvent::NoteOn);
}

TEST(MidiOverdub, DropsDeterministicallyWhenOutputCapacityExceeded)
{
	std::vector<MidiEvent> source;
	for (std::uint8_t note = 0u; note < 8u; ++note)
	{
		source.push_back(MidiEvent::MakeNoteOn(static_cast<std::uint32_t>(note * 10u), 0u, static_cast<std::uint8_t>(60u + note), 100u));
		source.push_back(MidiEvent::MakeNoteOff(static_cast<std::uint32_t>(note * 10u + 5u), 0u, static_cast<std::uint8_t>(60u + note)));
	}

	const auto events = BuildEvents(source, 128u, 128u, {}, 4u);
	ASSERT_EQ(4u, events.size());
	EXPECT_EQ(0u, events[0].Offset);
	EXPECT_EQ(5u, events[1].Offset);
	EXPECT_EQ(10u, events[2].Offset);
	EXPECT_EQ(15u, events[3].Offset);
}
