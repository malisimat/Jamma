#include "gtest/gtest.h"
#include "engine/MidiEvent.h"
#include "engine/MidiQueue.h"

using engine::MidiEvent;
using engine::MidiQueue;

namespace
{
	constexpr std::size_t kCap = 8; // small power-of-two for boundary tests

	MidiEvent NoteOn(std::uint32_t off, std::uint8_t note, std::uint8_t vel = 100)
	{
		return MidiEvent::MakeNoteOn(off, 0, note, vel);
	}
}

TEST(MidiQueue, NewQueueIsEmpty) {
	MidiQueue<kCap> q;
	ASSERT_TRUE(q.Empty());
	ASSERT_EQ(0u, q.Size());
	ASSERT_EQ(0u, q.DroppedCount());

	MidiEvent ev{};
	ASSERT_FALSE(q.Pop(ev));
}

TEST(MidiQueue, PushPopRoundTripPreservesEvent) {
	MidiQueue<kCap> q;
	const auto in = NoteOn(1234u, 60, 90);
	ASSERT_TRUE(q.Push(in));
	ASSERT_FALSE(q.Empty());
	ASSERT_EQ(1u, q.Size());

	MidiEvent out{};
	ASSERT_TRUE(q.Pop(out));
	ASSERT_EQ(in.sampleOffset, out.sampleOffset);
	ASSERT_EQ(in.status, out.status);
	ASSERT_EQ(in.data1, out.data1);
	ASSERT_EQ(in.data2, out.data2);
	ASSERT_TRUE(q.Empty());
}

TEST(MidiQueue, PreservesPushOrder) {
	MidiQueue<kCap> q;
	for (std::uint32_t i = 0; i < kCap - 1; ++i)
		ASSERT_TRUE(q.Push(NoteOn(i, static_cast<std::uint8_t>(i))));

	for (std::uint32_t i = 0; i < kCap - 1; ++i)
	{
		MidiEvent out{};
		ASSERT_TRUE(q.Pop(out));
		ASSERT_EQ(i, out.sampleOffset);
		ASSERT_EQ(i, out.data1);
	}
	ASSERT_TRUE(q.Empty());
}

TEST(MidiQueue, WrapsAroundInternalIndices) {
	MidiQueue<kCap> q;
	// Fill, drain, fill again — exercises tail and head crossing the buffer end.
	for (std::uint32_t pass = 0; pass < 3; ++pass)
	{
		for (std::uint32_t i = 0; i < kCap - 1; ++i)
			ASSERT_TRUE(q.Push(NoteOn(pass * 100 + i, static_cast<std::uint8_t>(i))));

		for (std::uint32_t i = 0; i < kCap - 1; ++i)
		{
			MidiEvent out{};
			ASSERT_TRUE(q.Pop(out));
			ASSERT_EQ(pass * 100 + i, out.sampleOffset);
		}
	}
	ASSERT_TRUE(q.Empty());
	ASSERT_EQ(0u, q.DroppedCount());
}

TEST(MidiQueue, OverflowDropsNewestAndKeepsExistingBufferedEvents) {
	MidiQueue<kCap> q;
	// Push kCap events; the SPSC ring holds kCap-1, so the last push overflows.
	for (std::uint32_t i = 0; i < kCap; ++i)
	{
		const bool ok = q.Push(NoteOn(i, static_cast<std::uint8_t>(i)));
		if (i < kCap - 1)
			ASSERT_TRUE(ok);
		else
			ASSERT_FALSE(ok); // overflow signalled
	}

	ASSERT_EQ(1u, q.DroppedCount());

	// Existing buffered events are preserved; overflow drops the newest push.
	std::uint32_t expected = 0;
	MidiEvent out{};
	while (q.Pop(out))
	{
		ASSERT_EQ(expected, out.sampleOffset);
		++expected;
	}
	ASSERT_EQ(kCap - 1, expected); // we popped events 0..kCap-2 inclusive
}

TEST(MidiQueue, ClearResetsState) {
	MidiQueue<kCap> q;
	for (std::uint32_t i = 0; i < kCap; ++i)
		(void)q.Push(NoteOn(i, static_cast<std::uint8_t>(i)));
	ASSERT_GT(q.DroppedCount(), 0u);
	ASSERT_FALSE(q.Empty());

	q.Clear();
	ASSERT_TRUE(q.Empty());
	ASSERT_EQ(0u, q.Size());
	ASSERT_EQ(0u, q.DroppedCount());
}

TEST(MidiEvent, NoteOnAndNoteOffClassification) {
	const auto on = MidiEvent::MakeNoteOn(0u, 3, 60, 100);
	ASSERT_TRUE(on.IsNoteOn());
	ASSERT_FALSE(on.IsNoteOff());
	ASSERT_EQ(3u, on.Channel());

	const auto off = MidiEvent::MakeNoteOff(0u, 3, 60);
	ASSERT_FALSE(off.IsNoteOn());
	ASSERT_TRUE(off.IsNoteOff());

	// NoteOn with velocity 0 is the running-status form of NoteOff.
	const auto runningOff = MidiEvent::MakeNoteOn(0u, 3, 60, 0);
	ASSERT_FALSE(runningOff.IsNoteOn());
	ASSERT_TRUE(runningOff.IsNoteOff());
}
