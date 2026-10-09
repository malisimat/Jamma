#include "gtest/gtest.h"
#include "graphics/MidiModel.h"
#include "graphics/QuantisationModel.h"

#include <cmath>

TEST(GraphicsMidiModel, SharedArcMeshHasChamferedCrossSectionAndMatchingUvs)
{
	constexpr unsigned int segments = 16u;
	const auto vertices = graphics::MidiModel::BuildBaseVerts(segments);
	const auto uvs = graphics::MidiModel::BuildBaseUvs(segments);
	const auto triangleCount = segments * 8u * 2u + 8u * 2u;
	EXPECT_EQ(vertices.size(), triangleCount * 9u);
	EXPECT_EQ(uvs.size(), triangleCount * 6u);

	bool foundChamfer = false;
	for (auto i = 0u; i + 2u < vertices.size(); i += 3u)
	{
		const auto y = std::abs(vertices[i + 1u]);
		const auto z = std::abs(vertices[i + 2u]);
		if (y > 0.39f && y < 0.5f && z > 0.85f)
			foundChamfer = true;
	}
	EXPECT_TRUE(foundChamfer);
	EXPECT_TRUE(graphics::MidiModel::BuildBaseVerts(0u).empty());
	EXPECT_TRUE(graphics::MidiModel::BuildBaseUvs(0u).empty());
}

TEST(GraphicsQuantisationModel, SubdivisionChangesDoNotMoveBeatGrid)
{
	engine::QuantisationLoopTakeVisual quarter{};
	quarter.LoopLengthSamps = 403u;
	quarter.GrainSamps = 100u;
	quarter.LoopGrains = 5u;
	quarter.Fraction = midi::MidiQuantisationFraction::Quarter;
	quarter.UseAbsoluteLocalGrid = true;
	quarter.GridIntervalSamps = 100u;
	quarter.GridBaseDivisions = 4u;
	quarter.TransportStartSamps = 17u;

	auto eighth = quarter;
	eighth.Fraction = midi::MidiQuantisationFraction::Eighth;

	const auto quarterCounts = engine::QuantisationModel::ResolveVisualCounts(quarter);
	const auto eighthCounts = engine::QuantisationModel::ResolveVisualCounts(eighth);
	EXPECT_EQ(17u, quarterCounts.GrainFrameCount);
	EXPECT_EQ(quarterCounts.GrainFrameCount, eighthCounts.GrainFrameCount);
	EXPECT_EQ(65u, quarterCounts.FractionDivisionCount);
	EXPECT_EQ(129u, eighthCounts.FractionDivisionCount);

	for (std::uint32_t beat = 0u; beat < quarterCounts.GrainFrameCount; ++beat)
	{
		const auto quarterOffset = engine::QuantisationModel::VisualBoundaryOffsetSamps(quarter, beat, 1u);
		const auto eighthOffset = engine::QuantisationModel::VisualBoundaryOffsetSamps(eighth, beat, 1u);
		EXPECT_EQ(quarterOffset, eighthOffset);
	}
}

TEST(GraphicsQuantisationModel, FractionDivisionCountRoundsFromTheActualGrid)
{
	engine::QuantisationLoopTakeVisual visual{};
	visual.LoopLengthSamps = 101u;
	visual.GrainSamps = 25u;
	visual.Fraction = midi::MidiQuantisationFraction::Quarter;
	visual.GridIntervalSamps = 100u;
	visual.GridBaseDivisions = 4u;

	const auto counts = engine::QuantisationModel::ResolveVisualCounts(visual);
	EXPECT_EQ(5u, counts.GrainFrameCount);
	EXPECT_EQ(17u, counts.FractionDivisionCount);
}

TEST(GraphicsMidiModel, NoteEndCapsFaceOutward)
{
	constexpr unsigned int segments = 16u;
	const auto vertices = graphics::MidiModel::BuildBaseVerts(segments);
	const auto capOffset = segments * 8u * 2u * 9u;
	for (auto edge = 0u; edge < 8u; ++edge)
	{
		for (auto end = 0u; end < 2u; ++end)
		{
			const auto i = capOffset + (edge * 2u + end) * 9u;
			const auto normalX = vertices[i + 4u] * vertices[i + 8u] -
				vertices[i + 5u] * vertices[i + 7u];
			EXPECT_GT(end == 0u ? -normalX : normalX, 0.0f);
		}
	}
}

TEST(GraphicsMidiModel, CapFreeRingRangeCoversEveryChamferFace)
{
	constexpr unsigned int segments = 16u;
	const auto vertices = graphics::MidiModel::BuildBaseVerts(segments);
	const auto uvs = graphics::MidiModel::BuildBaseUvs(segments);
	constexpr auto ringVertexCount = segments * 16u * 3u;
	ASSERT_EQ(vertices.size() / 3u, uvs.size() / 2u);
	ASSERT_EQ(ringVertexCount + 16u * 3u, vertices.size() / 3u);
	for (auto i = 0u; i < vertices.size() / 3u; ++i)
	{
		if (i < ringVertexCount)
			EXPECT_LT(uvs[i * 2u + 1u], 1.5f);
		else
			EXPECT_GT(uvs[i * 2u + 1u], 1.5f);
	}
}

TEST(GraphicsMidiModel, VelocityPreviewPreservesPublishedNotesAndClearsOnReplacement)
{
	for (const auto drawRing : { false, true })
	{
		graphics::MidiModelParams params;
		params.DrawSelectionRing = drawRing;
		graphics::MidiModel model(params);
		const midi::MidiNote original{ 10u, 20u, 0u, 60u, 80u, 0u };
		model.UpdateModel({ original }, 100u);
		for (const auto velocity : { 1, 32, 64, 90, 127 })
		{
			model.SetEditorHeld(0, velocity);
			EXPECT_EQ(drawRing ? 1 : 0, model.EditorHeldInstance());
			EXPECT_EQ(velocity, model.EditorPreviewVelocity());
			EXPECT_TRUE(model.EditorNoteMatches(0u, original, 100u));
			EXPECT_EQ(drawRing ? 2u : 1u, model.TotalInstanceCount());
		}
		model.ClearEditorHeld();
		EXPECT_EQ(-1, model.EditorHeldInstance());
		EXPECT_EQ(-1, model.EditorPreviewVelocity());
		model.SetEditorHeld(0, 32);
		model.UpdateModel({ original }, 100u);
		EXPECT_EQ(-1, model.EditorHeldInstance());
		EXPECT_EQ(-1, model.EditorPreviewVelocity());
	}
}
