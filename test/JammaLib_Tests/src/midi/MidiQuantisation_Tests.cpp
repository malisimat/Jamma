#include <vector>

#include "gtest/gtest.h"

#include "midi/MidiEvent.h"
#include "midi/MidiQuantisation.h"

using engine::MidiEvent;
using engine::MidiQuantisationDivisor;
using engine::MidiQuantisationFraction;
using engine::ClampMidiQuantisationFractionIndex;
using engine::MidiQuantisationFractionLabel;
using engine::MidiQuantisationSettings;
using engine::MidiQuantisationStepSamps;
using engine::MidiQuantisationGesture;
using engine::MidiQuantisationGrainCandidates;
using engine::ApplyMidiQuantisationGesture;
using engine::ApplyMidiQuantisationGuiPayload;
using engine::BuildQuantisedPlaybackEvents;
using engine::QuantiseEvents;
using engine::QuantiseSampleOffset;
using engine::ResolveMidiQuantisationDragFraction;
using engine::ResolveMidiQuantisationGestureGrain;

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

	std::vector<MidiEvent> BuildPlaybackVec(const std::vector<MidiEvent>& src,
		std::uint32_t loopLength,
		std::uint32_t step)
	{
		std::vector<MidiEvent> dst(src.size());
		BuildQuantisedPlaybackEvents(src.data(), src.size(), loopLength, step, dst.data());
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

TEST(MidiQuantisation, ClampFractionIndexBoundsPackedValues) {
	EXPECT_EQ(MidiQuantisationFraction::Whole, ClampMidiQuantisationFractionIndex(-1));
	EXPECT_EQ(MidiQuantisationFraction::Whole, ClampMidiQuantisationFractionIndex(0));
	EXPECT_EQ(MidiQuantisationFraction::Quarter, ClampMidiQuantisationFractionIndex(2));
	EXPECT_EQ(MidiQuantisationFraction::ThirtySecond, ClampMidiQuantisationFractionIndex(99));
}

TEST(MidiQuantisation, SettingsPackRoundTripsAndUnpackClampsFraction) {
	MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = MidiQuantisationFraction::Sixteenth;
	settings.GrainSamps = 48000u;

	const auto unpacked = MidiQuantisationSettings::Unpack(settings.Pack());
	EXPECT_EQ(settings, unpacked);

	const auto badFraction = 1ull | (99ull << 8u) | (3200ull << 16u);
	const auto clamped = MidiQuantisationSettings::Unpack(badFraction);
	EXPECT_TRUE(clamped.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::ThirtySecond, clamped.Fraction);
	EXPECT_EQ(3200u, clamped.GrainSamps);
}

TEST(MidiQuantisation, DragFractionResolutionRoundsAndClamps) {
	EXPECT_EQ(MidiQuantisationFraction::Whole,
		ResolveMidiQuantisationDragFraction(MidiQuantisationFraction::Whole, 10));
	EXPECT_EQ(MidiQuantisationFraction::Quarter,
		ResolveMidiQuantisationDragFraction(MidiQuantisationFraction::Whole, -64));
	EXPECT_EQ(MidiQuantisationFraction::Whole,
		ResolveMidiQuantisationDragFraction(MidiQuantisationFraction::Quarter, 200));
	EXPECT_EQ(MidiQuantisationFraction::ThirtySecond,
		ResolveMidiQuantisationDragFraction(MidiQuantisationFraction::ThirtySecond, -200));
}

TEST(MidiQuantisation, ApplyGestureTogglesOrDragsWithResolvedGrain) {
	MidiQuantisationSettings settings;
	settings.Enabled = false;
	settings.Fraction = MidiQuantisationFraction::Eighth;
	settings.GrainSamps = 0u;

	auto toggled = ApplyMidiQuantisationGesture(settings,
		MidiQuantisationGesture::Toggle,
		settings.Fraction,
		1600u);
	EXPECT_TRUE(toggled.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Eighth, toggled.Fraction);
	EXPECT_EQ(1600u, toggled.GrainSamps);

	auto dragged = ApplyMidiQuantisationGesture(toggled,
		MidiQuantisationGesture::DragFraction,
		MidiQuantisationFraction::Quarter,
		3200u);
	EXPECT_TRUE(dragged.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, dragged.Fraction);
	EXPECT_EQ(1600u, dragged.GrainSamps);

	auto disabled = ApplyMidiQuantisationGesture(dragged,
		MidiQuantisationGesture::Toggle,
		dragged.Fraction,
		3200u);
	EXPECT_FALSE(disabled.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Quarter, disabled.Fraction);
	EXPECT_EQ(1600u, disabled.GrainSamps);
}

TEST(MidiQuantisation, GestureGrainFallsBackByCurrentLoopPriority) {
	MidiQuantisationGrainCandidates candidates;
	candidates.FirstPlayableMidiLoopSamps = 960u;
	candidates.FirstAudioLoopSamps = 1920u;
	candidates.MidiVisualLoopSamps = 3840u;
	candidates.RecordedSamps = 7680u;
	EXPECT_EQ(960u, ResolveMidiQuantisationGestureGrain(candidates));

	candidates.FirstPlayableMidiLoopSamps = 0u;
	EXPECT_EQ(1920u, ResolveMidiQuantisationGestureGrain(candidates));

	candidates.FirstAudioLoopSamps = 0u;
	EXPECT_EQ(3840u, ResolveMidiQuantisationGestureGrain(candidates));

	candidates.MidiVisualLoopSamps = 0u;
	EXPECT_EQ(7680u, ResolveMidiQuantisationGestureGrain(candidates));
}

TEST(MidiQuantisation, GuiPayloadPreservesCurrentGrainWhenAbsentOrZero) {
	MidiQuantisationSettings current;
	current.Enabled = true;
	current.Fraction = MidiQuantisationFraction::Quarter;
	current.GrainSamps = 2400u;

	const int absentGrain[] = { 0, static_cast<int>(MidiQuantisationFraction::Eighth) };
	auto updated = ApplyMidiQuantisationGuiPayload(current, absentGrain, 2u);
	EXPECT_FALSE(updated.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Eighth, updated.Fraction);
	EXPECT_EQ(2400u, updated.GrainSamps);

	const int zeroGrain[] = { 1, static_cast<int>(MidiQuantisationFraction::Half), 0 };
	updated = ApplyMidiQuantisationGuiPayload(updated, zeroGrain, 3u);
	EXPECT_TRUE(updated.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::Half, updated.Fraction);
	EXPECT_EQ(2400u, updated.GrainSamps);

	const int explicitGrain[] = { 1, 99, 4800 };
	updated = ApplyMidiQuantisationGuiPayload(updated, explicitGrain, 3u);
	EXPECT_TRUE(updated.Enabled);
	EXPECT_EQ(MidiQuantisationFraction::ThirtySecond, updated.Fraction);
	EXPECT_EQ(4800u, updated.GrainSamps);
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

TEST(MidiQuantisation, PlaybackEventsAreSortedAfterCrossingQuantisedNotes) {
	std::vector<MidiEvent> events = {
		MidiEvent::MakeNoteOn(140u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(240u, 0u, 60u),
		MidiEvent::MakeNoteOn(160u, 0u, 62u, 100u),
		MidiEvent::MakeNoteOff(260u, 0u, 62u),
	};

	auto result = BuildPlaybackVec(events, 1000u, 100u);
	ASSERT_EQ(4u, result.size());
	EXPECT_EQ(100u, result[0].sampleOffset);
	EXPECT_EQ(60u, result[0].data1);
	EXPECT_TRUE(result[0].IsNoteOn());
	EXPECT_EQ(200u, result[1].sampleOffset);
	EXPECT_EQ(60u, result[1].data1);
	EXPECT_TRUE(result[1].IsNoteOff());
	EXPECT_EQ(200u, result[2].sampleOffset);
	EXPECT_EQ(62u, result[2].data1);
	EXPECT_TRUE(result[2].IsNoteOn());
	EXPECT_EQ(300u, result[3].sampleOffset);
}

TEST(MidiQuantisation, PlaybackEventsWrapLateNotesToCanonicalStartOrder) {
	std::vector<MidiEvent> events = {
		MidiEvent::MakeNoteOn(970u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(990u, 0u, 60u),
		MidiEvent::MakeNoteOn(40u, 0u, 62u, 100u),
		MidiEvent::MakeNoteOff(140u, 0u, 62u),
	};

	auto result = BuildPlaybackVec(events, 1000u, 100u);
	ASSERT_EQ(4u, result.size());
	EXPECT_EQ(0u, result[0].sampleOffset);
	EXPECT_EQ(60u, result[0].data1);
	EXPECT_TRUE(result[0].IsNoteOn());
	EXPECT_EQ(0u, result[1].sampleOffset);
	EXPECT_EQ(62u, result[1].data1);
	EXPECT_TRUE(result[1].IsNoteOn());
	EXPECT_EQ(20u, result[2].sampleOffset);
	EXPECT_EQ(60u, result[2].data1);
	EXPECT_TRUE(result[2].IsNoteOff());
	EXPECT_EQ(100u, result[3].sampleOffset);
	EXPECT_EQ(62u, result[3].data1);
	EXPECT_TRUE(result[3].IsNoteOff());
}

TEST(MidiQuantisation, PlaybackEventsOrderSameSampleNoteOffBeforeNoteOn) {
	std::vector<MidiEvent> events = {
		MidiEvent::MakeNoteOn(120u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(220u, 0u, 60u),
		MidiEvent::MakeNoteOn(200u, 0u, 60u, 100u),
		MidiEvent::MakeNoteOff(300u, 0u, 60u),
	};

	auto result = BuildPlaybackVec(events, 1000u, 100u);
	ASSERT_EQ(4u, result.size());
	EXPECT_EQ(100u, result[0].sampleOffset);
	EXPECT_TRUE(result[0].IsNoteOn());
	EXPECT_EQ(200u, result[1].sampleOffset);
	EXPECT_TRUE(result[1].IsNoteOff());
	EXPECT_EQ(200u, result[2].sampleOffset);
	EXPECT_TRUE(result[2].IsNoteOn());
	EXPECT_EQ(300u, result[3].sampleOffset);
}
