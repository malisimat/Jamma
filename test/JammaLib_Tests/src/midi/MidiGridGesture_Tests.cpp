#include "gtest/gtest.h"

#include "midi/MidiGridGesture.h"
#include "midi/MidiPitchViewGesture.h"
#include "graphics/LoopGridProjection.h"
#include "glm/gtc/matrix_transform.hpp"

using midi::MidiGridGesture;
using midi::MidiLoop;

TEST(MidiGridGesture, ChannelControlStaysOutsideProjectedGridDuringOrbit)
{
	const auto projection = glm::perspective(glm::radians(42.0f), 1.0f, 1.0f, 1000.0f);
	std::optional<utils::Position2d> previous;
	bool moved = false;
	for (const auto eye : {glm::vec3(0,300,0.01f), glm::vec3(120,260,80),
		glm::vec3(-100,250,-100)})
	{
		const auto vp = projection * glm::lookAt(eye, glm::vec3(0), glm::vec3(0,0,-1));
		const auto position = graphics::LoopGridProjection::SideControlPosition(
			vp, glm::mat4(1), 50.0f, 800, 800, {88u,84u});
		ASSERT_TRUE(position);
		int left = 800, right = 0;
		for (const auto x : {-50.0f, 50.0f})
			for (const auto z : {-39.0f, 39.0f})
			{
				const auto corner = graphics::LoopGridProjection::Project(vp, glm::mat4(1), {x,0,z}, 800,800);
				ASSERT_TRUE(corner);
				left = std::min(left, corner->X); right = std::max(right, corner->X);
			}
		EXPECT_TRUE(position->X >= right + 18 || position->X + 88 <= left - 18);
		EXPECT_GE(position->Y, 18); EXPECT_LE(position->Y + 84, 782);
		if (previous && (position->X != previous->X || position->Y != previous->Y)) moved = true;
		previous = position;
	}
	EXPECT_TRUE(moved);
}

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

TEST(MidiGridGesture, CreationUsesDefaultVelocityWithAndWithoutGrid)
{
	for (const bool quantised : {false, true})
	{
		const auto source = MidiGridGestureFixture::EmptyLoop(quantised);
		MidiGridGesture gesture;
		ASSERT_TRUE(gesture.Begin(source, {15u, 60u}, 2u));
		EXPECT_EQ(100u, gesture.Working().Events[0].data2);
		ASSERT_TRUE(gesture.Update({35u, 60u}));
		for (std::size_t i = 0; i < gesture.Working().EventCount; ++i)
			if (gesture.Working().Events[i].IsNoteOn())
				EXPECT_EQ(100u, gesture.Working().Events[i].data2);
	}
}

TEST(MidiGridGesture, CreationUsesEditedVelocityThroughoutDrag)
{
	for (const bool quantised : {false, true})
	{
		auto source = MidiGridGestureFixture::EmptyLoop(quantised);
		ASSERT_TRUE(midi::MidiEditOperations::Create(source, 10u, 10u, 2u, 60u, 75u));
		MidiGridGesture velocity;
		ASSERT_TRUE(velocity.BeginVelocity(source, {15u, 60u}));
		ASSERT_TRUE(velocity.UpdateRelative(-20));
		ASSERT_EQ(70, velocity.ProposedVelocity());
		MidiGridGesture creation;
		ASSERT_TRUE(creation.Begin(velocity.Working(), {35u, 62u}, 2u, 0.0,
			static_cast<std::uint8_t>(velocity.ProposedVelocity())));
		ASSERT_TRUE(creation.Update({65u, 62u}));
		for (std::size_t i = 0; i < creation.Working().EventCount; ++i)
			if (creation.Working().Events[i].IsNoteOn())
				EXPECT_EQ(70u, creation.Working().Events[i].data2);
	}
}

TEST(MidiGridGesture, CreationRejectsInvalidVelocity)
{
	const auto source = MidiGridGestureFixture::EmptyLoop();
	MidiGridGesture gesture;
	EXPECT_FALSE(gesture.Begin(source, {15u, 60u}, 0u, 0.0, 0u));
	EXPECT_FALSE(gesture.Begin(source, {15u, 60u}, 0u, 0.0, 128u));
}

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
	ASSERT_EQ(1u, click.Preview().size());
	EXPECT_EQ(20u, click.Preview()[0].Start);
	EXPECT_EQ(40u, click.Preview()[0].End);
	ASSERT_TRUE(click.Update({ 30u, 64u, 0.30 }));
	EXPECT_FALSE(click.Dirty());
	ASSERT_EQ(1u, click.Preview().size());
	EXPECT_EQ(20u, click.Preview()[0].Start);
	EXPECT_EQ(40u, click.Preview()[0].End);
	click.Cancel();
	EXPECT_TRUE(click.Preview().empty());
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

TEST(MidiGridGesture, OverlappingChannelsUseLastRenderedSpan)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	source.Events[0] = midi::MidiEvent::MakeNoteOn(10u, 0u, 60u, 80u);
	source.Events[1] = midi::MidiEvent::MakeNoteOn(10u, 1u, 60u, 90u);
	source.Events[2] = midi::MidiEvent::MakeNoteOff(20u, 0u, 60u);
	source.Events[3] = midi::MidiEvent::MakeNoteOff(20u, 1u, 60u);
	source.EventCount = 4u;
	MidiGridGesture gesture;

	ASSERT_TRUE(gesture.Begin(source, {15u, 60u, 0.15}, 0u));
	EXPECT_FALSE(gesture.Rejected());
	ASSERT_EQ(2u, gesture.Working().EventCount);
	EXPECT_EQ(0u, gesture.Working().Events[0].Channel());
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

TEST(MidiGridGesture, EveryVisibleSampleRemovesWholeOffGridNoteAndPreservesOtherEvents)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	source.Events[0] = midi::MidiEvent::MakeNoteOn(12u, 2u, 60u, 81u);
	source.Events[1] = {17u, 0xb2u, 7u, 99u, 0u};
	source.Events[2] = midi::MidiEvent::MakeNoteOff(38u, 2u, 60u);
	source.Events[3] = midi::MidiEvent::MakeNoteOn(40u, 2u, 60u, 83u);
	source.Events[4] = midi::MidiEvent::MakeNoteOff(50u, 2u, 60u);
	source.EventCount = 5u;
	const auto targets = midi::MidiGridTargets::Build(source);
	ASSERT_EQ(2u, targets.Notes.size());
	EXPECT_EQ(10u, targets.Notes[0].Start);
	EXPECT_EQ(36u, targets.Notes[0].End);
	for (auto sample = 10u; sample < 36u; ++sample)
	{
		SCOPED_TRACE(sample);
		MidiGridGesture gesture;
		ASSERT_TRUE(gesture.Begin(source, {sample, 60u, sample / 100.0}, 2u));
		ASSERT_FALSE(gesture.Filling());
		ASSERT_EQ(3u, gesture.Working().EventCount);
		EXPECT_EQ(0xb2u, gesture.Working().Events[0].status);
		EXPECT_EQ(40u, gesture.Working().Events[1].sampleOffset);
		EXPECT_EQ(83u, gesture.Working().Events[1].data2);
		EXPECT_EQ(10u, gesture.Preview()[0].Start);
		EXPECT_EQ(36u, gesture.Preview()[0].End);
	}
	MidiGridGesture adjacent;
	ASSERT_TRUE(adjacent.Begin(source, {36u, 60u, 0.36}, 2u));
	EXPECT_TRUE(adjacent.Filling());
	EXPECT_EQ(36u, adjacent.Preview()[0].Start);
	EXPECT_EQ(40u, adjacent.Preview()[0].End);
}

TEST(MidiGridGesture, PaintModesRemainFixedAndNarrowInterpolatedNotesAreVisited)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::CreateExact(source, 10u, 11u, 0u, 60u));
	ASSERT_TRUE(midi::MidiEditOperations::CreateExact(source, 30u, 31u, 0u, 60u));
	MidiGridGesture remove;
	ASSERT_TRUE(remove.Begin(source, {10u, 60u, 0.10}, 0u));
	ASSERT_TRUE(remove.Update({35u, 60u, 0.35}));
	EXPECT_FALSE(remove.Filling());
	EXPECT_EQ(0u, remove.Working().EventCount);
	EXPECT_EQ(2u, remove.Preview().size());
	ASSERT_TRUE(remove.Update({10u, 60u, 0.10}));
	EXPECT_EQ(2u, remove.Preview().size());
	MidiGridGesture add;
	ASSERT_TRUE(add.Begin(source, {5u, 60u, 0.05}, 0u));
	const auto count = add.Working().EventCount;
	ASSERT_TRUE(add.Update({10u, 60u, 0.10}));
	EXPECT_EQ(count, add.Working().EventCount);
	ASSERT_TRUE(add.Update({35u, 60u, 0.35}));
	EXPECT_TRUE(add.Filling());
	EXPECT_FALSE(add.Rejected());
	const auto spans = midi::MidiGridTargets::Build(add.Working()).Notes;
	EXPECT_EQ(6u, spans.size());
	for (const auto& span : spans) EXPECT_FALSE(span.Ambiguous);
}

TEST(MidiGridGesture, PartialCellFillsMultipleUncoveredSpansAndKeepsNotes)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::CreateExact(source, 12u, 14u, 2u, 60u));
	ASSERT_TRUE(midi::MidiEditOperations::CreateExact(source, 16u, 18u, 2u, 60u));
	MidiGridGesture add;
	ASSERT_TRUE(add.Begin(source, {15u, 60u, 0.15}, 2u));
	ASSERT_EQ(3u, add.Preview().size());
	EXPECT_EQ(10u, add.Preview()[0].Start); EXPECT_EQ(12u, add.Preview()[0].End);
	EXPECT_EQ(14u, add.Preview()[1].Start); EXPECT_EQ(16u, add.Preview()[1].End);
	EXPECT_EQ(18u, add.Preview()[2].Start); EXPECT_EQ(20u, add.Preview()[2].End);
	EXPECT_EQ(5u, midi::MidiGridTargets::Build(add.Working()).Notes.size());
	ASSERT_TRUE(add.Update({13u, 60u, 0.13}));
	ASSERT_EQ(4u, add.Preview().size());
	EXPECT_EQ(12u, add.Preview().back().Start);
	EXPECT_EQ(14u, add.Preview().back().End);
	ASSERT_TRUE(add.Update({13u, 60u, 0.13}));
	EXPECT_EQ(4u, add.Preview().size());
	EXPECT_EQ(5u, midi::MidiGridTargets::Build(add.Working()).Notes.size());
}

TEST(MidiGridGesture, AddPaintHoldsSkippedExistingNotesWithoutChangingThem)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::CreateExact(source, 10u, 11u, 2u, 60u));
	ASSERT_TRUE(midi::MidiEditOperations::CreateExact(source, 30u, 31u, 2u, 60u));
	MidiGridGesture add;
	ASSERT_TRUE(add.Begin(source, {5u, 60u, 0.05}, 2u));
	ASSERT_TRUE(add.Update({35u, 60u, 0.35}));
	const auto heldCount = add.Preview().size();
	for (const auto start : {10u, 30u})
	{
		EXPECT_EQ(1u, std::count_if(add.Preview().begin(), add.Preview().end(),
			[start](const auto& span) { return span.Start == start && span.End == start + 1u; }));
	}
	ASSERT_TRUE(add.Update({5u, 60u, 0.05}));
	EXPECT_EQ(heldCount, add.Preview().size());
	const auto notes = midi::MidiGridTargets::Build(add.Working()).Notes;
	for (const auto start : {10u, 30u})
		EXPECT_EQ(1u, std::count_if(notes.begin(), notes.end(),
			[start](const auto& note) { return note.Start == start && note.End == start + 1u; }));
}

TEST(MidiGridGesture, AmbiguousSourceRemovalRollsBackWholeGesture)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	source.Events[0] = midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 90u);
	source.Events[1] = midi::MidiEvent::MakeNoteOff(10u, 0u, 60u);
	source.Events[2] = midi::MidiEvent::MakeNoteOn(20u, 0u, 60u, 90u);
	source.Events[3] = midi::MidiEvent::MakeNoteOn(30u, 0u, 60u, 91u);
	source.Events[4] = midi::MidiEvent::MakeNoteOff(40u, 0u, 60u);
	source.Events[5] = midi::MidiEvent::MakeNoteOff(50u, 0u, 60u);
	source.EventCount = 6u;
	MidiGridGesture remove;
	ASSERT_TRUE(remove.Begin(source, {5u, 60u, 0.05}, 0u));
	EXPECT_FALSE(remove.Update({35u, 60u, 0.35}));
	EXPECT_TRUE(remove.Rejected()); EXPECT_FALSE(remove.Dirty());
	EXPECT_TRUE(remove.Preview().empty());
	EXPECT_EQ(source.EventCount, remove.Working().EventCount);
	MidiGridGesture add;
	ASSERT_TRUE(add.Begin(source, {65u, 60u, 0.65}, 0u));
	EXPECT_TRUE(add.Dirty()); // Unrelated ambiguity does not lock empty cells.
}

TEST(MidiGridGesture, EnumeratedLocalAndRemoteCellsCanAllBeAddedAndRemoved)
{
	// Exhaust small physical lengths, rational steps, wrapped phase/origin, and
	// nonzero transport starts. Every geometry cell is tested through the gesture.
	for (const bool remote : {false, true})
	for (const auto length : {1u, 2u, 3u, 7u, 11u, 23u, 100u})
	for (const auto interval : {4u, 10u, 13u})
	for (const auto fraction : {midi::MidiQuantisationFraction::Whole, midi::MidiQuantisationFraction::Quarter})
	for (const auto phase : {-1, 0, 1})
	for (const auto transport : {0u, 17u})
	{
		auto source = MidiGridGestureFixture::EmptyLoop();
		source.LoopLengthSamps = length; source.Quantisation.GrainSamps = interval;
		source.Quantisation.Fraction = fraction; source.Quantisation.PhaseOffsetSamps = phase;
		source.QuantisationTransportStartSamps = transport;
		if (remote)
		{
			source.Quantisation.RemoteIntervalSamps = interval;
			source.Quantisation.RemoteBpi = 3u;
			source.Quantisation.RemoteOriginSamps = -7;
		}
		const auto grid = midi::LoopGridGeometry::Resolve(length, source.Quantisation, transport);
		ASSERT_TRUE(grid);
		for (std::size_t cell = 0; cell + 1 < grid->Boundaries.size(); ++cell)
		{
			SCOPED_TRACE(::testing::Message() << remote << ":" << length << ":" << interval << ":" << phase << ":" << transport << ":" << cell);
			const auto canonical = grid->SeamHeadEnd && cell == 0u ? grid->Boundaries.size() - 2u : cell;
			const auto start = grid->Boundaries[canonical], end = grid->Boundaries[canonical + 1];
			MidiGridGesture add;
			ASSERT_TRUE(add.Begin(source, {start, 60u, start / static_cast<double>(length)}, 3u));
			const auto targets = midi::MidiGridTargets::Build(add.Working());
			ASSERT_EQ(1u, targets.Notes.size());
			EXPECT_EQ(start, targets.Notes[0].Start); EXPECT_EQ(end, targets.Notes[0].End);
			EXPECT_FALSE(targets.Notes[0].Ambiguous);
			// Toggle quantisation: editor spans remain exact sample-domain material.
			auto disabled = add.Working(); disabled.Quantisation.Enabled = false;
			const auto rawTargets = midi::MidiGridTargets::Build(disabled);
			ASSERT_EQ(1u, rawTargets.Notes.size());
			EXPECT_EQ(start, rawTargets.Notes[0].Start); EXPECT_EQ(end, rawTargets.Notes[0].End);
			MidiGridGesture remove;
			ASSERT_TRUE(remove.Begin(add.Working(), {end - 1u, 60u, (end - 1u) / static_cast<double>(length)}, 3u));
			EXPECT_FALSE(remove.Filling()); EXPECT_EQ(0u, remove.Working().EventCount);
		}
	}
}

TEST(MidiGridGesture, TargetSpansMatchRenderedPlaybackAndCellEdges)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	source.Quantisation.PhaseOffsetSamps = 1;
	source.Events[0] = midi::MidiEvent::MakeNoteOn(12u, 0u, 60u, 90u);
	source.Events[1] = midi::MidiEvent::MakeNoteOff(28u, 0u, 60u);
	source.EventCount = 2u;
	std::array<midi::MidiEvent, 2> playback;
	midi::MidiQuantisation::BuildQuantisedPlaybackEvents(source.Events.data(), 2u, 100u,
		source.Quantisation, 0u, playback.data());
	const auto rendered = midi::MidiNote::ExtractSpans(playback.data(), 2u, 100u);
	const auto targets = midi::MidiGridTargets::Build(source);
	ASSERT_EQ(rendered.size(), targets.Notes.size());
	const auto grid = midi::LoopGridGeometry::Resolve(100u, source.Quantisation, 0u);
	for (auto sample = 0u; sample < 100u; ++sample)
	{
		const auto target = targets.Resolve(sample, 60u, &*grid);
		const bool on = rendered[0].StartSample <= sample && sample < rendered[0].StartSample + rendered[0].DurationSamples;
		ASSERT_EQ(on, target.NoteIndex.has_value());
		if (on)
		{
			EXPECT_EQ(rendered[0].StartSample, target.Start);
			EXPECT_EQ(rendered[0].StartSample + rendered[0].DurationSamples, target.End);
		}
		else
		{
			EXPECT_EQ(grid->Boundaries[grid->CellAt(sample)], target.Start);
			EXPECT_EQ(grid->Boundaries[grid->CellAt(sample) + 1u], target.End);
		}
	}
}

TEST(MidiGridGesture, PaintVisitsEverySkippedPitchRowAndRollsBackOnCapacity)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	MidiGridGesture add;
	ASSERT_TRUE(add.Begin(source, {15u, 60u, 0.15}, 0u));
	ASSERT_TRUE(add.Update({15u, 64u, 0.15}));
	ASSERT_EQ(5u, add.Preview().size());
	for (std::size_t i = 0; i < 5; ++i) EXPECT_EQ(60u + i, add.Preview()[i].Pitch);
	source.EventCount = MidiLoop::DefaultCapacity - 2u;
	for (std::size_t i = 0; i < source.EventCount; ++i) source.Events[i] = {0u, 0xb0u, 7u, 99u, 0u};
	MidiGridGesture full;
	ASSERT_TRUE(full.Begin(source, {15u, 60u, 0.15}, 0u));
	EXPECT_FALSE(full.Update({25u, 60u, 0.25}));
	EXPECT_TRUE(full.Rejected()); EXPECT_FALSE(full.Dirty());
	EXPECT_TRUE(full.Preview().empty()); EXPECT_EQ(source.EventCount, full.Working().EventCount);
}

TEST(MidiGridGesture, WholeGridPaintAndRemoveIncludesPhysicalSeamFragments)
{
	for (const auto length : {11u, 100u})
	for (const auto phase : {0, 1})
	for (const auto pitch : {0u, 127u})
	{
		auto source = MidiGridGestureFixture::EmptyLoop();
		source.LoopLengthSamps = length;
		source.Quantisation.GrainSamps = length == 11u ? 4u : 10u;
		source.Quantisation.PhaseOffsetSamps = phase;
		const auto grid = midi::LoopGridGeometry::Resolve(length, source.Quantisation, 0u);
		ASSERT_TRUE(grid);
		MidiGridGesture add;
		const auto note = static_cast<std::uint8_t>(pitch);
		ASSERT_TRUE(add.Begin(source, {0u, note, 0.0}, 0u));
		for (std::size_t cell = 1; cell + 1 < grid->Boundaries.size(); ++cell)
			ASSERT_TRUE(add.Update({grid->Boundaries[cell], note, grid->Boundaries[cell] / static_cast<double>(length)}));
		const auto painted = midi::MidiGridTargets::Build(add.Working());
		EXPECT_EQ(grid->Boundaries.size() - 1u - (grid->SeamHeadEnd ? 1u : 0u), painted.Notes.size());
		for (auto sample = 0u; sample < length; ++sample)
			EXPECT_TRUE(painted.Resolve(sample, note, &*grid).NoteIndex);
		MidiGridGesture remove;
		ASSERT_TRUE(remove.Begin(add.Working(), {length - 1u, note, (length - 1u) / static_cast<double>(length)}, 0u));
		// Visit backwards, then cross the seam back to the last cell.
		for (std::size_t cell = grid->Boundaries.size() - 1u; cell-- > 0u;)
			ASSERT_TRUE(remove.Update({grid->Boundaries[cell], note, grid->Boundaries[cell] / static_cast<double>(length)}));
		ASSERT_TRUE(remove.Update({length - 1u, note, (length - 1u) / static_cast<double>(length)}));
		EXPECT_EQ(0u, remove.Working().EventCount);
		EXPECT_EQ(painted.Notes.size(), remove.Preview().size());
	}
}

TEST(MidiGridGesture, QuantisationReorderingStillRemovesTheCorrectSourceIdentity)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	source.Quantisation.PhaseOffsetSamps = 20;
	source.Events[0] = midi::MidiEvent::MakeNoteOn(10u, 0u, 60u, 70u);
	source.Events[1] = midi::MidiEvent::MakeNoteOff(20u, 0u, 60u);
	source.Events[2] = midi::MidiEvent::MakeNoteOn(90u, 0u, 60u, 90u);
	source.Events[3] = midi::MidiEvent::MakeNoteOff(95u, 0u, 60u);
	source.EventCount = 4u;
	const auto targets = midi::MidiGridTargets::Build(source);
	ASSERT_EQ(2u, targets.Notes.size());
	EXPECT_EQ(2u, targets.Notes[0].On);
	EXPECT_EQ(10u, targets.Notes[0].Start);
	MidiGridGesture remove;
	ASSERT_TRUE(remove.Begin(source, {12u, 60u, 0.12}, 0u));
	ASSERT_EQ(2u, remove.Working().EventCount);
	EXPECT_EQ(10u, remove.Working().Events[0].sampleOffset);
	EXPECT_EQ(70u, remove.Working().Events[0].data2);
	ASSERT_TRUE(remove.Update({35u, 60u, 0.35}));
	EXPECT_EQ(0u, remove.Working().EventCount);
}

TEST(MidiGridGesture, VelocityCapturesQuantisedIdentityAndChangesOnlyOneField)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 12u, 15u, 3u, 60u, 80u));
	source.Events[0].flags = 0x40;
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.BeginVelocity(source, {11u, 60u}));
	ASSERT_TRUE(gesture.CapturedNoteIndex());
	EXPECT_EQ(10u, gesture.CapturedTarget().Start);
	EXPECT_TRUE(gesture.Preview().empty());
	ASSERT_TRUE(gesture.UpdateRelative(2.0)); EXPECT_FALSE(gesture.Dirty());
	ASSERT_TRUE(gesture.UpdateRelative(6.0)); EXPECT_EQ(82, gesture.ProposedVelocity());
	EXPECT_TRUE(gesture.Dirty());
	for (std::size_t i = 0; i < source.EventCount; ++i)
	{
		const auto& a = source.Events[i]; const auto& b = gesture.Working().Events[i];
		EXPECT_EQ(a.sampleOffset, b.sampleOffset); EXPECT_EQ(a.status, b.status);
		EXPECT_EQ(a.data1, b.data1); EXPECT_EQ(a.flags, b.flags);
		EXPECT_EQ(i == 0 ? 82 : a.data2, b.data2);
	}
	ASSERT_TRUE(gesture.UpdateRelative(-8.0)); EXPECT_FALSE(gesture.Dirty());
	ASSERT_TRUE(gesture.UpdateRelative(1000)); EXPECT_EQ(127, gesture.ProposedVelocity());
	ASSERT_TRUE(gesture.UpdateRelative(-2000)); EXPECT_EQ(1, gesture.ProposedVelocity());
}

TEST(MidiGridGesture, VelocityWorksWithoutGridAndRejectsAmbiguousNotes)
{
	auto source = MidiGridGestureFixture::EmptyLoop(false);
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 10u, 30u, 1u, 60u));
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.BeginVelocity(source, {20u, 60u}));
	EXPECT_FALSE(gesture.BeginSnappedMove(source, {20u, 60u}));
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 15u, 15u, 1u, 60u));
	EXPECT_FALSE(gesture.BeginVelocity(source, {20u, 60u})); EXPECT_TRUE(gesture.Rejected());
	EXPECT_EQ(4u, source.EventCount);
}

TEST(MidiGridGesture, SnappedMoveRetainsGrabOffsetRawDurationAndMetadata)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 12u, 25u, 3u, 60u, 41u));
	source.Events[0].flags = 0x40; source.Events[1].flags = 0x20; source.Events[1].data2 = 7;
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.BeginSnappedMove(source, {31u, 60u}));
	ASSERT_TRUE(gesture.Update({39u, 60u})); EXPECT_FALSE(gesture.Dirty());
	ASSERT_TRUE(gesture.Update({71u, 64u})); EXPECT_TRUE(gesture.Dirty());
	ASSERT_EQ(1u, gesture.Preview().size()); EXPECT_EQ(50u, gesture.Preview()[0].Start);
	const auto& moved = gesture.Working();
	EXPECT_EQ(25u, moved.Events[1].sampleOffset - moved.Events[0].sampleOffset);
	EXPECT_EQ(64u, moved.Events[0].data1); EXPECT_EQ(64u, moved.Events[1].data1);
	EXPECT_EQ(41u, moved.Events[0].data2); EXPECT_EQ(7u, moved.Events[1].data2);
	EXPECT_EQ(0x40u, moved.Events[0].flags); EXPECT_EQ(0x20u, moved.Events[1].flags);
	EXPECT_EQ(3u, moved.Events[0].Channel());
	ASSERT_TRUE(gesture.Update({31u, 60u})); EXPECT_FALSE(gesture.Dirty());
}

TEST(MidiGridGesture, SnappedOverlapReplacesAndInvalidDestinationKeepsPreview)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 10u, 20u, 1u, 60u));
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 50u, 20u, 1u, 60u));
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.BeginSnappedMove(source, {15u, 60u}));
	ASSERT_TRUE(gesture.Update({55u, 60u})); EXPECT_TRUE(gesture.Dirty());
	EXPECT_EQ(2u, gesture.Working().EventCount);
	EXPECT_EQ(50u, gesture.Working().Events[0].sampleOffset);
	ASSERT_TRUE(gesture.Update({35u, 62u}));
	EXPECT_EQ(4u, gesture.Working().EventCount); // Only the released destination matters.
	auto seam = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::Create(seam, 80u, 20u, 1u, 60u));
	ASSERT_TRUE(gesture.BeginSnappedMove(seam, {85u, 60u}));
	EXPECT_FALSE(gesture.Update({15u, 60u})); EXPECT_EQ(1u, gesture.Working().EventCount);
	EXPECT_FALSE(gesture.Rejected()); ASSERT_EQ(1u, gesture.Preview().size());
	EXPECT_EQ(10u, gesture.Preview()[0].Start);
	ASSERT_TRUE(gesture.Update({85u, 61u})); EXPECT_TRUE(gesture.Dirty());
}

TEST(MidiGridGesture, SnappedMoveReplacesAllOverlappingChannelsAndKeepsOtherNotes)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 10u, 20u, 3u, 60u, 41u));
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 40u, 20u, 0u, 64u));
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 60u, 20u, 1u, 64u));
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 50u, 20u, 2u, 65u));
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 70u, 10u, 2u, 64u));
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.BeginSnappedMove(source, {15u, 60u}));
	ASSERT_TRUE(gesture.Update({55u, 64u}));
	const auto targets = midi::MidiGridTargets::Build(gesture.Working());
	ASSERT_EQ(3u, targets.Notes.size());
	const auto moved = std::find_if(targets.Notes.begin(), targets.Notes.end(), [](const auto& n) {
		return n.Start == 50u && n.Pitch == 64u;
	});
	ASSERT_NE(targets.Notes.end(), moved);
	EXPECT_EQ(70u, moved->End);
	EXPECT_EQ(3u, gesture.Working().Events[moved->On].Channel());
	EXPECT_EQ(41u, gesture.Working().Events[moved->On].data2);
	EXPECT_EQ(10u, source.EventCount); // Detached preview has not edited the source.
}

TEST(MidiGridGesture, SnappedRationalRemoteAndPhaseGridReprojectsExactly)
{
	for (int phase : {0, 3, -7})
	{
		auto source = MidiGridGestureFixture::EmptyLoop();
		source.Quantisation.RemoteIntervalSamps = 101u; source.Quantisation.RemoteBpi = 7u;
		source.Quantisation.RemoteOriginSamps = -23;
		source.Quantisation.PhaseOffsetSamps = phase; source.QuantisationTransportStartSamps = 13;
		ASSERT_TRUE(midi::MidiEditOperations::Create(source, 20u, 5u, 2u, 60u));
		const auto targets = midi::MidiGridTargets::Build(source);
		ASSERT_EQ(1u, targets.Notes.size());
		const auto grid = midi::LoopGridGeometry::Resolve(100u, source.Quantisation, 13u);
		ASSERT_TRUE(grid);
		int accepted = 0;
		for (auto start : grid->Boundaries)
		{
			if (start + 5u >= 100u) continue;
			auto candidate = source;
			if (!midi::MidiEditOperations::MoveSnapped(candidate, 0, 1, start, start + 5u, 62)) continue;
			++accepted;
			const auto moved = midi::MidiGridTargets::Build(candidate);
			ASSERT_EQ(1u, moved.Notes.size()); EXPECT_EQ(start, moved.Notes[0].Start); EXPECT_EQ(start + 5u, moved.Notes[0].End);
			EXPECT_EQ(5u, candidate.Events[1].sampleOffset - candidate.Events[0].sampleOffset);
		}
		EXPECT_GT(accepted, 0);
	}
}

TEST(MidiPitchViewGesture, ProjectedPanAndZoomKeepAnchorPitch)
{
	midi::MidiPitchViewGesture view;
	view.Begin(40, 24, 0.5); view.Update(4, 0.5);
	EXPECT_EQ(40, view.Bottom()); EXPECT_EQ(24, view.Rows());
	view.Update(16, 0.75); EXPECT_EQ(26, view.Rows()); EXPECT_EQ(33, view.Bottom());
	EXPECT_NEAR(52.0, view.Bottom() + 0.75 * view.Rows(), 0.5);
	view.Update(0, 0.5); EXPECT_EQ(24, view.Rows()); EXPECT_EQ(40, view.Bottom());
	view.Update(10000, 10000); EXPECT_EQ(128, view.Rows()); EXPECT_EQ(0, view.Bottom());
	view.Begin(40, 24, 0.5); view.Update(-10000, -10000);
	EXPECT_EQ(12, view.Rows()); EXPECT_EQ(116, view.Bottom());
}

TEST(MidiGridGesture, ExactTimingSnappedMovePreservesFlagsAndPitchLimits)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::CreateExact(source, 11u, 16u, 4u, 120u));
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.BeginSnappedMove(source, {12u, 120u}));
	ASSERT_TRUE(gesture.Update({42u, 127u}));
	EXPECT_EQ(40u, gesture.Working().Events[0].sampleOffset);
	EXPECT_EQ(45u, gesture.Working().Events[1].sampleOffset);
	EXPECT_TRUE(gesture.Working().Events[0].HasExactTiming());
	EXPECT_TRUE(gesture.Working().Events[1].HasExactTiming());
	EXPECT_EQ(127u, gesture.Working().Events[0].data1);
	auto changed = source; ++changed.Revision;
	EXPECT_FALSE(gesture.MatchesPublished(changed));
	gesture.Cancel(); EXPECT_EQ(MidiGridGesture::Kind::None, gesture.Mode());
	EXPECT_FALSE(gesture.CapturedNoteIndex()); EXPECT_TRUE(gesture.Preview().empty());
}

TEST(MidiGridGesture, VelocityOverlapUsesLastDisplayedChannelAndKeepsControlEvents)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 12u, 15u, 1u, 60u, 20u));
	ASSERT_TRUE(midi::MidiEditOperations::Create(source, 13u, 15u, 2u, 60u, 70u));
	source.Events[source.EventCount++] = midi::MidiEvent{90u, 0xB1, 7u, 99u, 0x80u};
	MidiGridGesture gesture;
	ASSERT_TRUE(gesture.BeginVelocity(source, {11u, 60u}));
	ASSERT_TRUE(gesture.UpdateRelative(4));
	EXPECT_EQ(20u, gesture.Working().Events[0].data2);
	EXPECT_EQ(71u, gesture.Working().Events[1].data2);
	EXPECT_EQ(0xB1u, gesture.Working().Events[4].status);
	EXPECT_EQ(99u, gesture.Working().Events[4].data2);
	EXPECT_EQ(0x80u, gesture.Working().Events[4].flags);
}

TEST(MidiGridGesture, SplitEdgeCellsCreateOneNoteAndShareRemovalAndVelocity)
{
	for (const bool remote : {false, true})
	for (const auto clicked : {0u, 99u})
	{
		auto source = MidiGridGestureFixture::EmptyLoop();
		source.Quantisation.PhaseOffsetSamps = 1;
		if (remote)
		{
			source.Quantisation.RemoteIntervalSamps = 100u;
			source.Quantisation.RemoteBpi = 10u;
		}
		MidiGridGesture add;
		ASSERT_TRUE(add.Begin(source, {clicked, 60u, clicked / 100.0}, 2u));
		ASSERT_EQ(1u, add.Working().EventCount);
		EXPECT_EQ(91u, add.Working().Events[0].sampleOffset);
		ASSERT_EQ(1u, add.Preview().size());
		EXPECT_EQ(91u, add.Preview()[0].Start); EXPECT_EQ(101u, add.Preview()[0].End);
		ASSERT_TRUE(add.Update({clicked == 0u ? 99u : 0u, 60u, clicked == 0u ? 0.99 : 0.0}));
		EXPECT_EQ(1u, add.Working().EventCount); EXPECT_EQ(1u, add.Preview().size());
		for (const auto edge : {0u, 99u})
		{
			MidiGridGesture velocity;
			ASSERT_TRUE(velocity.BeginVelocity(add.Working(), {edge, 60u}));
			EXPECT_EQ(0u, *velocity.CapturedNoteIndex());
			ASSERT_TRUE(velocity.UpdateRelative(4.0));
			EXPECT_EQ(101u, velocity.Working().Events[0].data2);
			MidiGridGesture remove;
			ASSERT_TRUE(remove.Begin(add.Working(), {edge, 60u}, 2u));
			EXPECT_EQ(0u, remove.Working().EventCount);
			ASSERT_EQ(1u, remove.Preview().size());
			EXPECT_EQ(101u, remove.Preview()[0].End);
		}
	}
}

TEST(MidiGridGesture, ExistingFirstCellNoteHasTheSameIdentityAtBothEdges)
{
	auto source = MidiGridGestureFixture::EmptyLoop();
	source.Quantisation.PhaseOffsetSamps = 1;
	ASSERT_TRUE(midi::MidiEditOperations::CreateExact(source, 0u, 1u, 2u, 60u, 72u));
	const auto grid = midi::LoopGridGeometry::Resolve(100u, source.Quantisation, 0u);
	const auto targets = midi::MidiGridTargets::Build(source);
	const auto first = targets.Resolve(0u, 60u, &*grid), last = targets.Resolve(99u, 60u, &*grid);
	ASSERT_TRUE(first.NoteIndex); EXPECT_EQ(first.NoteIndex, last.NoteIndex);
	MidiGridGesture remove;
	ASSERT_TRUE(remove.Begin(source, {99u, 60u}, 2u));
	EXPECT_EQ(0u, remove.Working().EventCount);
}
