#include <vector>

#include "gtest/gtest.h"

#include "engine/MidiEvent.h"
#include "engine/MidiQuantisation.h"

using engine::MidiEvent;
using engine::MidiQuantisationDivisor;
using engine::MidiQuantisationFraction;
using engine::MidiQuantisationFractionLabel;
using engine::MidiQuantisationSettings;
using engine::MidiQuantisationStepSamps;
using engine::QuantiseEvents;
using engine::QuantiseSampleOffset;

namespace
{
	std::vector<MidiEvent> QuantiseVec(const std::vector<MidiEvent>& src,
		std::uint32_t loopLength,
		std::uint32_t step)
	{
		std::vector<MidiEvent> dst(src.size());
		QuantiseEvents(src.data(), src.size(), loopLength, step, dst.data());
		return dst;
	}
}

TEST(MidiQuantisation, DivisorMatchesFractionName) {
	EXPECT_EQ(1u, MidiQuantisationDivisor(MidiQuantisationFraction::Whole));
	EXPECT_EQ(2u, MidiQuantisationDivisor(MidiQuantisationFraction::Half));
	EXPECT_EQ(4u, MidiQuantisationDivisor(MidiQuantisationFraction::Quarter));
	EXPECT_EQ(8u, MidiQuantisationDivisor(MidiQuantisationFraction::Eighth));
	EXPECT_EQ(16u, MidiQuantisationDivisor(MidiQuantisationFraction::Sixteenth));
	EXPECT_EQ(32u, MidiQuantisationDivisor(MidiQuantisationFraction::ThirtySecond));
}

TEST(MidiQuantisation, FractionLabelsMatchFractions) {
	EXPECT_STREQ("1", MidiQuantisationFractionLabel(MidiQuantisationFraction::Whole));
	EXPECT_STREQ("1/2", MidiQuantisationFractionLabel(MidiQuantisationFraction::Half));
	EXPECT_STREQ("1/4", MidiQuantisationFractionLabel(MidiQuantisationFraction::Quarter));
	EXPECT_STREQ("1/8", MidiQuantisationFractionLabel(MidiQuantisationFraction::Eighth));
	EXPECT_STREQ("1/16", MidiQuantisationFractionLabel(MidiQuantisationFraction::Sixteenth));
	EXPECT_STREQ("1/32", MidiQuantisationFractionLabel(MidiQuantisationFraction::ThirtySecond));
}

TEST(MidiQuantisation, StepSampsReturnsZeroWhenDisabledOrNoGrain) {
	MidiQuantisationSettings s;
	EXPECT_EQ(0u, MidiQuantisationStepSamps(s));

	s.Enabled = true;
	EXPECT_EQ(0u, MidiQuantisationStepSamps(s));

	s.GrainSamps = 24000u;
	EXPECT_EQ(24000u, MidiQuantisationStepSamps(s));

	s.Fraction = MidiQuantisationFraction::Quarter;
	EXPECT_EQ(6000u, MidiQuantisationStepSamps(s));
}

TEST(MidiQuantisation, QuantiseOffsetSnapsToNearestMultiple) {
	EXPECT_EQ(0u, QuantiseSampleOffset(0u, 100u, 1000u));
	EXPECT_EQ(0u, QuantiseSampleOffset(49u, 100u, 1000u));
	EXPECT_EQ(100u, QuantiseSampleOffset(50u, 100u, 1000u));
	EXPECT_EQ(100u, QuantiseSampleOffset(149u, 100u, 1000u));
	EXPECT_EQ(200u, QuantiseSampleOffset(150u, 100u, 1000u));
}

TEST(MidiQuantisation, QuantiseOffsetWrapsAtLoopEnd) {
	// Loop length 1000, step 100 — sample 970 should wrap to 0 (nearest snap = 1000).
	EXPECT_EQ(0u, QuantiseSampleOffset(970u, 100u, 1000u));
	EXPECT_EQ(900u, QuantiseSampleOffset(949u, 100u, 1000u));
}

TEST(MidiQuantisation, ZeroStepIsPassthrough) {
	EXPECT_EQ(123u, QuantiseSampleOffset(123u, 0u, 1000u));
}

TEST(MidiQuantisation, QuantiseEventsPreservesNoteDuration) {
	std::vector<MidiEvent> events = {
		MidiEvent::MakeNoteOn(160u, 0u, 60u, 100u),   // nearest 100-multiple is 200, delta +40
		MidiEvent::MakeNoteOff(560u, 0u, 60u),        // shifted by +40 -> 600, duration preserved (400)
	};

	auto result = QuantiseVec(events, 1000u, 100u);
	ASSERT_EQ(2u, result.size());
	EXPECT_EQ(200u, result[0].sampleOffset);
	EXPECT_EQ(600u, result[1].sampleOffset);
	EXPECT_EQ(400u, result[1].sampleOffset - result[0].sampleOffset);
}

TEST(MidiQuantisation, QuantiseEventsClampsNoteOffAtLoopBoundary) {
	// Note-on near end of loop. After snap forward, the matching note-off would
	// land past loopLength — clamp to loopLength - 1 so playback still fires the
	// note-off inside the window.
	std::vector<MidiEvent> events = {
		MidiEvent::MakeNoteOn(960u, 0u, 60u, 100u),  // nearest 100-multiple = 1000 -> wrap to 0, delta = -960
		MidiEvent::MakeNoteOff(990u, 0u, 60u),       // 990 + (-960) = 30 -> stays inside loop
	};

	auto result = QuantiseVec(events, 1000u, 100u);
	EXPECT_EQ(0u, result[0].sampleOffset);
	EXPECT_EQ(30u, result[1].sampleOffset);

	// Now a long note shifted forward so its tail crosses the boundary.
	std::vector<MidiEvent> longNote = {
		MidiEvent::MakeNoteOn(550u, 0u, 60u, 100u),  // snap-to 600, delta +50
		MidiEvent::MakeNoteOff(990u, 0u, 60u),       // 990 + 50 = 1040 -> clamp to 999
	};
	auto longResult = QuantiseVec(longNote, 1000u, 100u);
	EXPECT_EQ(600u, longResult[0].sampleOffset);
	EXPECT_EQ(999u, longResult[1].sampleOffset);
}

TEST(MidiQuantisation, QuantiseEventsPassesThroughWhenStepZero) {
	std::vector<MidiEvent> events = {
		MidiEvent::MakeNoteOn(123u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(789u, 0u, 60u),
	};
	auto result = QuantiseVec(events, 1000u, 0u);
	EXPECT_EQ(123u, result[0].sampleOffset);
	EXPECT_EQ(789u, result[1].sampleOffset);
}

TEST(MidiQuantisation, QuantiseEventsHandlesMultipleNotesAndChannels) {
	std::vector<MidiEvent> events = {
		MidiEvent::MakeNoteOn(140u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOn(160u, 1u, 60u, 100u),  // different channel, independent slot
		MidiEvent::MakeNoteOff(340u, 0u, 60u),
		MidiEvent::MakeNoteOff(360u, 1u, 60u),
	};
	auto result = QuantiseVec(events, 1000u, 100u);
	EXPECT_EQ(100u, result[0].sampleOffset); // 140 -> 100 (delta -40)
	EXPECT_EQ(200u, result[1].sampleOffset); // 160 -> 200 (delta +40)
	EXPECT_EQ(300u, result[2].sampleOffset); // 340 - 40
	EXPECT_EQ(400u, result[3].sampleOffset); // 360 + 40
}

TEST(MidiQuantisation, QuantiseEventsLeavesUnpairedNoteOffUntouched) {
	// NoteOff with no preceding NoteOn keeps its original timestamp.
	std::vector<MidiEvent> events = {
		MidiEvent::MakeNoteOff(340u, 0u, 60u),
	};
	auto result = QuantiseVec(events, 1000u, 100u);
	EXPECT_EQ(340u, result[0].sampleOffset);
}
