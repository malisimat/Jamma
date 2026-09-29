#include "gtest/gtest.h"
#include "graphics/MidiModel.h"

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
