
#include <cmath>
#include "gtest/gtest.h"
#include "graphics/StationModel.h"

using graphics::StationModel;

// -------------------------------------------------------------------------
// Deck-top geometry
// -------------------------------------------------------------------------

TEST(StationModel_Geometry, DeckTop_HasExpectedTriCount)
{
	constexpr unsigned int NumSides = 32u;
	auto [verts, uvs] = StationModel::BuildDeckTop(NumSides, 100.0f);

	// Each side yields 1 triangle (fan from center).
	const auto numTris = verts.size() / 9u;  // 9 floats per triangle
	EXPECT_EQ(numTris, NumSides);
}

TEST(StationModel_Geometry, DeckTop_UVCountMatchesVertCount)
{
	auto [verts, uvs] = StationModel::BuildDeckTop(16u, 100.0f);
	// 3 floats per vert, 2 floats per uv, same vertex count
	EXPECT_EQ(verts.size() / 3u, uvs.size() / 2u);
}

TEST(StationModel_Geometry, DeckTop_AllVertsAtDeckY)
{
	constexpr float Radius = 80.0f;
	auto [verts, uvs] = StationModel::BuildDeckTop(16u, Radius);

	// Every Y component (index 1, 4, 7, ...) should be 0.
	for (size_t i = 1u; i < verts.size(); i += 3u)
		EXPECT_FLOAT_EQ(verts[i], 0.0f) << "Vert Y at index " << i;
}

TEST(StationModel_Geometry, DeckTop_RadiusBound)
{
	constexpr float Radius = 50.0f;
	auto [verts, uvs] = StationModel::BuildDeckTop(16u, Radius);

	for (size_t i = 0u; i + 2u < verts.size(); i += 3u)
	{
		const float x = verts[i];
		const float z = verts[i + 2u];
		const float r = std::sqrt(x * x + z * z);
		EXPECT_LE(r, Radius + 1e-3f) << "Vertex outside radius at index " << i;
	}
}

// -------------------------------------------------------------------------
// Bevel geometry
// -------------------------------------------------------------------------

TEST(StationModel_Geometry, Bevel_HasExpectedTriCount)
{
	constexpr unsigned int NumSides = 16u;
	auto [verts, uvs] = StationModel::BuildBevel(NumSides, 100.0f, 10.0f, 8.0f);

	// Each side yields 2 triangles (one quad).
	const auto numTris = verts.size() / 9u;
	EXPECT_EQ(numTris, NumSides * 2u);
}

TEST(StationModel_Geometry, Bevel_UVPartKindAllBevel)
{
	auto [verts, uvs] = StationModel::BuildBevel(8u, 100.0f, 10.0f, 8.0f);

	// UV.y (index 1, 3, 5, ...) should all equal 1.0 (UV_BEVEL).
	for (size_t i = 1u; i < uvs.size(); i += 2u)
		EXPECT_FLOAT_EQ(uvs[i], 1.0f) << "UV.y at index " << i;
}

// -------------------------------------------------------------------------
// Side geometry
// -------------------------------------------------------------------------

TEST(StationModel_Geometry, Side_HasExpectedTriCount)
{
	constexpr unsigned int NumSides = 16u;
	auto [verts, uvs] = StationModel::BuildSide(NumSides, 100.0f, 14.0f);

	const auto numTris = verts.size() / 9u;
	EXPECT_EQ(numTris, NumSides * 2u);
}

// -------------------------------------------------------------------------
// Rib geometry
// -------------------------------------------------------------------------

TEST(StationModel_Geometry, Ribs_NonEmpty)
{
	auto [verts, uvs] = StationModel::BuildRibs(32u, 100.0f, 8u,
		18.0f, 90.0f, 6.0f, 0.045f);

	EXPECT_FALSE(verts.empty());
	EXPECT_FALSE(uvs.empty());
}

TEST(StationModel_Geometry, Ribs_UVPartKindAllRib)
{
	auto [verts, uvs] = StationModel::BuildRibs(32u, 100.0f, 4u,
		18.0f, 90.0f, 6.0f, 0.045f);

	// UV.y (index 1, 3, 5, ...) should all equal 3.0 (UV_RIB).
	for (size_t i = 1u; i < uvs.size(); i += 2u)
		EXPECT_FLOAT_EQ(uvs[i], 3.0f) << "UV.y at index " << i;
}

// -------------------------------------------------------------------------
// Combined geometry
// -------------------------------------------------------------------------

TEST(StationModel_Geometry, BuildAllGeometry_NonEmpty)
{
	auto [verts, uvs] = StationModel::BuildAllGeometry(32u, 190.0f, 8u);

	EXPECT_FALSE(verts.empty());
	EXPECT_FALSE(uvs.empty());
}

TEST(StationModel_Geometry, BuildAllGeometry_UVCountMatchesVerts)
{
	auto [verts, uvs] = StationModel::BuildAllGeometry(32u, 190.0f, 8u);
	EXPECT_EQ(verts.size() / 3u, uvs.size() / 2u);
}

TEST(StationModel_Geometry, BuildAllGeometry_TriCountAboveFloor)
{
	auto [verts, uvs] = StationModel::BuildAllGeometry(32u, 190.0f, 8u);
	const auto numTris = verts.size() / 9u;
	// 32 (top) + 32*2 (bevel) + 32*2 (side) + 8*6 (ribs) = 32+64+64+48 = 208
	EXPECT_GE(numTris, 200u);
}

TEST(StationModel_Geometry, BuildAllGeometry_VertCountDivisibleBy3)
{
	auto [verts, uvs] = StationModel::BuildAllGeometry(32u, 190.0f, 8u);
	// Each triangle is 3 verts * 3 floats = 9 floats; total must be multiple of 9.
	EXPECT_EQ(verts.size() % 9u, 0u);
}
