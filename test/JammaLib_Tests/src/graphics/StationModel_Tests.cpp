
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
	EXPECT_EQ(numTris, NumSides * 2u * 12u);
}

TEST(StationModel_Geometry, BottomBevel_HasExpectedTriCount)
{
	constexpr unsigned int NumSides = 16u;
	auto [verts, uvs] = StationModel::BuildBottomBevel(NumSides, 100.0f, 10.0f, 8.0f, 14.0f);

	const auto numTris = verts.size() / 9u;
	EXPECT_EQ(numTris, NumSides * 2u);
}

TEST(StationModel_Geometry, DeckBottom_HasExpectedTriCount)
{
	constexpr unsigned int NumSides = 16u;
	auto [verts, uvs] = StationModel::BuildDeckBottom(NumSides, 100.0f, 8.0f, 14.0f);

	const auto numTris = verts.size() / 9u;
	EXPECT_EQ(numTris, NumSides);
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
	// Top + upper bevel + side + lower bevel + bottom cap.
	EXPECT_GE(numTris, 200u);
}

TEST(StationModel_Geometry, BuildAllGeometry_IgnoresRibCount)
{
	auto [verts0, uvs0] = StationModel::BuildAllGeometry(32u, 190.0f, 0u);
	auto [verts8, uvs8] = StationModel::BuildAllGeometry(32u, 190.0f, 8u);

	EXPECT_EQ(verts0.size(), verts8.size());
	EXPECT_EQ(uvs0.size(), uvs8.size());
}

TEST(StationModel_Geometry, BuildAllGeometry_VertCountDivisibleBy3)
{
	auto [verts, uvs] = StationModel::BuildAllGeometry(32u, 190.0f, 8u);
	// Each triangle is 3 verts * 3 floats = 9 floats; total must be multiple of 9.
	EXPECT_EQ(verts.size() % 9u, 0u);
}

// -------------------------------------------------------------------------
// State ring geometry
// -------------------------------------------------------------------------

TEST(StationModel_Rings, LathedProfilesCloseAndMatchUvCount)
{
	const graphics::RingProfilePoint topProfile[] = {
		{ 9.3f, 0.0f }, { 15.8f, -3.5f }, { 14.9f, -13.5f }, { 11.2f, -16.0f }
	};
	const graphics::RingProfilePoint bottomProfile[] = {
		{ 9.3f, 0.0f }, { 16.6f, 3.2f }, { 15.3f, 12.4f }, { 10.2f, 19.0f }
	};
	auto [topVerts, topUvs] = StationModel::BuildLathedProfileGeometry(64u, topProfile, -2.0f, false, 4.0f);
	auto [bottomVerts, bottomUvs] = StationModel::BuildLathedProfileGeometry(64u, bottomProfile, -468.0f, true, 4.0f);

	EXPECT_EQ(topVerts.size() / 3u, topUvs.size() / 2u);
	EXPECT_EQ(bottomVerts.size() / 3u, bottomUvs.size() / 2u);
	EXPECT_EQ(topVerts.size() / 9u, 64u * (std::size(topProfile) - 1u) * 2u);
	EXPECT_EQ(bottomVerts.size() / 9u, 64u * (std::size(bottomProfile) - 1u) * 2u);
	EXPECT_NE(topVerts, bottomVerts);

	for (std::size_t i = 0u; i + 2u < topVerts.size(); i += 3u)
	{
		const auto radius = std::sqrt(topVerts[i] * topVerts[i] + topVerts[i + 2u] * topVerts[i + 2u]);
		EXPECT_GT(radius, 0.0f);
	}
}

TEST(StationModel_Rings, OccluderPrismHasTwoBarsAndScalesWithRadius)
{
	auto [verts, uvs] = StationModel::BuildOccluderPrismGeometry(45.0f, 85.0f, 5.0f);
	auto [smallerVerts, smallerUvs] = StationModel::BuildOccluderPrismGeometry(9.0f, 17.0f, 5.0f);

	EXPECT_EQ(verts.size() / 9u, 24u);
	EXPECT_EQ(verts.size() / 3u, uvs.size() / 2u);
	EXPECT_EQ(smallerVerts.size(), verts.size());
	EXPECT_EQ(smallerUvs.size(), uvs.size());
	EXPECT_NE(smallerVerts, verts);
}
