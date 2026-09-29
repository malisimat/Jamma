#include "gtest/gtest.h"

#include "midi/MidiGridGesture.h"
#include "graphics/LoopGridProjection.h"
#include "glm/gtc/matrix_transform.hpp"

using midi::MidiGridGesture;
using midi::MidiLoop;

struct MidiGridGestureFixture
{
	static MidiLoop::EditState EmptyLoop(bool quantised = true)
	{
		MidiLoop::EditState state;
		state.LoopLengthSamps = 100u;
		state.Revision = 7u;
		state.Quantisation.Enabled = quantised;
		state.Quantisation.GrainSamps = 10u;
		state.Quantisation.Fraction = midi::MidiQuantisationFraction::Whole;
		return state;
	}
};

TEST(MidiGridGesture, PaintsSkippedCellsOnceAndCrossesSeam)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.Begin(source, { 95u, 60u, 0.95 }, 2u));
	ASSERT_TRUE(gesture.Update({ 5u, 60u, 0.05 }));
	ASSERT_EQ(2u, gesture.Preview().size());
	ASSERT_TRUE(gesture.Update({ 35u, 60u, 0.35 }));
	ASSERT_EQ(5u, gesture.Preview().size());
	ASSERT_TRUE(gesture.Update({ 15u, 60u, 0.15 }));
	EXPECT_EQ(5u, gesture.Preview().size());
	EXPECT_TRUE(gesture.Dirty());
	EXPECT_FALSE(gesture.Rejected());
}

TEST(MidiGridGesture, FilledStartErasesAndEmptyNeighborsStayEmpty)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	source.Events[0] = midi::MidiEvent::MakeNoteOn(10u, 2u, 60u, 81u);
	source.Events[1] = midi::MidiEvent::MakeNoteOff(20u, 2u, 60u);
	source.EventCount = 2u;
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.Begin(source, { 15u, 60u, 0.15 }, 2u));
	EXPECT_FALSE(gesture.Filling());
	ASSERT_TRUE(gesture.Update({ 35u, 60u, 0.35 }));
	EXPECT_EQ(1u, gesture.Preview().size());
	EXPECT_EQ(0u, gesture.Working().EventCount);
	EXPECT_EQ(2u, source.EventCount);
}

TEST(MidiGridGesture, FreeMoveUsesAnchorAndClampsPitchAndTime)
{
	auto source = MidiGridGestureFixture::EmptyLoop(false);
	source.Events[0] = midi::MidiEvent::MakeNoteOn(20u, 4u, 60u, 87u);
	source.Events[1] = midi::MidiEvent::MakeNoteOff(40u, 4u, 60u);
	source.EventCount = 2u;
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.Begin(source, { 30u, 60u, 0.30 }, 4u, 10.0));
	EXPECT_EQ(MidiGridGesture::Kind::Move, gesture.Mode());
	ASSERT_TRUE(gesture.Update({ 95u, 127u, 0.95 }));
	EXPECT_EQ(80u, gesture.Working().Events[0].sampleOffset);
	EXPECT_EQ(127u, gesture.Working().Events[0].data1);
	ASSERT_TRUE(gesture.Update({ 31u, 60u, 0.31 }));
	EXPECT_EQ(21u, gesture.Working().Events[0].sampleOffset);
	EXPECT_EQ(60u, gesture.Working().Events[0].data1);
}

TEST(MidiGridGesture, CancelDiscardsDetachedWork)
{
	auto source = MidiGridGestureFixture::EmptyLoop(false);
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.Begin(source, { 20u, 62u, 0.20 }, 0u));
	ASSERT_TRUE(gesture.Dirty());
	gesture.Cancel();
	EXPECT_FALSE(gesture.Dirty());
	EXPECT_EQ(MidiGridGesture::Kind::None, gesture.Mode());
	EXPECT_EQ(0u, source.EventCount);
}

TEST(MidiGridGesture, FreeEdgeTrimHasSixPixelTargetAndClickIsNoOp)
{
	auto source = MidiGridGestureFixture::EmptyLoop(false);
	source.Events[0] = midi::MidiEvent::MakeNoteOn(20u, 1u, 64u, 91u);
	source.Events[1] = midi::MidiEvent::MakeNoteOff(40u, 1u, 64u);
	source.EventCount = 2u;
	MidiGridGesture trim;
	ASSERT_TRUE(trim.Begin(source, { 19u, 64u, 0.19 }, 1u, 5.0));
	EXPECT_EQ(MidiGridGesture::Kind::TrimLeft, trim.Mode());
	ASSERT_TRUE(trim.Update({ 10u, 64u, 0.10 }));
	EXPECT_EQ(11u, trim.Working().Events[0].sampleOffset);
	MidiGridGesture click;
	ASSERT_TRUE(click.Begin(source, { 30u, 64u, 0.30 }, 1u, 10.0));
	EXPECT_EQ(MidiGridGesture::Kind::Move, click.Mode());
	ASSERT_TRUE(click.Update({ 30u, 64u, 0.30 }));
	EXPECT_FALSE(click.Dirty());
}

TEST(MidiGridGesture, FullCapacityFreeCreateRejectsWithoutSourceChange)
{
	auto source = MidiGridGestureFixture::EmptyLoop(false);
	source.EventCount = MidiLoop::DefaultCapacity;
	MidiGridGesture gesture;
	EXPECT_FALSE(gesture.Begin(source, { 10u, 60u, 0.10 }, 0u));
	EXPECT_FALSE(gesture.Dirty());
	EXPECT_EQ(MidiLoop::DefaultCapacity, source.EventCount);
}

TEST(MidiGridGesture, EmptyClickKeepsModestDefaultAndLeftDragCreatesEarlier)
{
	auto source = MidiGridGestureFixture::EmptyLoop(false);
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.Begin(source, { 20u, 63u, 0.20 }, 0u));
	const auto clickDuration = gesture.Preview().front().End - gesture.Preview().front().Start;
	ASSERT_GT(clickDuration, 1u);
	ASSERT_TRUE(gesture.Update({ 20u, 63u, 0.20 }));
	EXPECT_EQ(clickDuration, gesture.Preview().front().End - gesture.Preview().front().Start);
	ASSERT_TRUE(gesture.Update({ 10u, 63u, 0.10 }));
	EXPECT_EQ(10u, gesture.Preview().front().Start);
	EXPECT_EQ(21u, gesture.Preview().front().End);
}

TEST(MidiGridGesture, GridOrRevisionChangeInvalidatesDetachedGesture)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.Begin(source, { 10u, 60u, 0.10 }, 0u));
	EXPECT_TRUE(gesture.MatchesPublished(source));
	auto changed = source;
	changed.Quantisation.PhaseOffsetSamps = 1;
	EXPECT_FALSE(gesture.MatchesPublished(changed));
	changed = source;
	++changed.Revision;
	EXPECT_FALSE(gesture.MatchesPublished(changed));
	gesture.Cancel();
	EXPECT_FALSE(gesture.MatchesPublished(source));
}

TEST(MidiGridGesture, OverlappingChannelsRejectEraseAtomically)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	source.Events[0] = midi::MidiEvent::MakeNoteOn(10u, 0u, 60u, 80u);
	source.Events[1] = midi::MidiEvent::MakeNoteOn(10u, 1u, 60u, 90u);
	source.Events[2] = midi::MidiEvent::MakeNoteOff(20u, 0u, 60u);
	source.Events[3] = midi::MidiEvent::MakeNoteOff(20u, 1u, 60u);
	source.EventCount = 4u;
	MidiGridGesture gesture;
	EXPECT_FALSE(gesture.Begin(source, { 15u, 60u, 0.15 }, 0u));
	EXPECT_TRUE(gesture.Rejected());
	EXPECT_FALSE(gesture.Dirty());
	EXPECT_TRUE(gesture.Preview().empty());
	EXPECT_EQ(4u, gesture.Working().EventCount);
	EXPECT_EQ(4u, source.EventCount);
}

TEST(MidiGridGesture, PointerRayRoundTripsThroughLocalPlaneAfterResize)
{
	const auto view = glm::lookAt(glm::vec3(10.0f, 180.0f, 20.0f),
		glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
	const auto model = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 0.0f, -3.0f));
	for (const auto width : { 800, 1600 })
	{
		const auto vp = glm::perspective(glm::radians(50.0f),
			static_cast<float>(width) / 600.0f, 1.0f, 1000.0f) * view;
		const auto pixel = graphics::LoopGridProjection::Project(vp, model,
			{ 12.0f, 2.0f, -8.0f }, width, 600);
		ASSERT_TRUE(pixel);
		const auto local = graphics::LoopGridProjection::UnprojectToLocalPlane(vp,
			model, *pixel, width, 600, 2.0f);
		ASSERT_TRUE(local);
		EXPECT_NEAR(12.0f, local->x, 0.6f);
		EXPECT_NEAR(-8.0f, local->z, 0.6f);
	}
}

TEST(MidiGridGesture, CameraDistanceFitsWideAndNarrowEditorSurfaces)
{
	const auto wide = graphics::LoopGridProjection::CameraDistance(400.0f, 1.0f, 16.0f / 9.0f);
	const auto narrow = graphics::LoopGridProjection::CameraDistance(400.0f, 1.0f, 9.0f / 16.0f);
	EXPECT_GT(narrow, wide);
	const auto tanHalfFov = std::tan(glm::radians(21.0f));
	EXPECT_GT((wide - 40.0f) * tanHalfFov * 16.0f / 9.0f, 400.0f);
	EXPECT_GT((wide - 40.0f) * tanHalfFov, 400.0f * 0.78f);
	EXPECT_GT((narrow - 40.0f) * tanHalfFov * 9.0f / 16.0f, 400.0f);
}

TEST(MidiGridGesture, HigherPitchProjectsAboveLowerPitch)
{
	const auto view = glm::lookAt(glm::vec3(0.0f, 400.0f, 40.0f),
		glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
	const auto vp = glm::perspective(glm::radians(42.0f), 1.0f, 10.0f, 2000.0f) * view;
	const auto low = graphics::LoopGridProjection::Project(vp, glm::mat4(1.0f),
		{ 0.0f, 2.0f, 40.0f }, 800, 800);
	const auto high = graphics::LoopGridProjection::Project(vp, glm::mat4(1.0f),
		{ 0.0f, 2.0f, -40.0f }, 800, 800);
	ASSERT_TRUE(low);
	ASSERT_TRUE(high);
	EXPECT_GT(high->Y, low->Y);
}
