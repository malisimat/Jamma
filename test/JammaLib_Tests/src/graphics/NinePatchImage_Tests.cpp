#include <array>
#include <algorithm>
#include <vector>

#include "gtest/gtest.h"
#include "graphics/NinePatchImage.h"

using graphics::NinePatchImage;

namespace
{
	std::pair<float, float> CellXBounds(const std::array<GLfloat, 162>& positions, int cellIndex)
	{
		const auto start = cellIndex * 18;
		auto minX = positions[start + 0];
		auto maxX = positions[start + 0];
		for (auto i = 0; i < 6; ++i)
		{
			const auto x = positions[start + (i * 3)];
			minX = (std::min)(minX, x);
			maxX = (std::max)(maxX, x);
		}

		return { minX, maxX };
	}

	std::pair<float, float> CellYBounds(const std::array<GLfloat, 162>& positions, int cellIndex)
	{
		const auto start = cellIndex * 18;
		auto minY = positions[start + 1];
		auto maxY = positions[start + 1];
		for (auto i = 0; i < 6; ++i)
		{
			const auto y = positions[start + (i * 3) + 1];
			minY = (std::min)(minY, y);
			maxY = (std::max)(maxY, y);
		}

		return { minY, maxY };
	}
}

TEST(NinePatchImageTest, BuildPositions_CornersClamped)
{
	auto positions = NinePatchImage::BuildPositions(8, 8, { 64, 64 });
	auto [xMin, xMax] = CellXBounds(positions, 0);
	auto [yMin, yMax] = CellYBounds(positions, 0);

	EXPECT_FLOAT_EQ(0.0f, xMin);
	EXPECT_FLOAT_EQ(8.0f, xMax);
	EXPECT_FLOAT_EQ(0.0f, yMin);
	EXPECT_FLOAT_EQ(8.0f, yMax);
}

TEST(NinePatchImageTest, BuildPositions_CentreStretches)
{
	auto positions = NinePatchImage::BuildPositions(8, 8, { 200, 100 });
	auto [xMin, xMax] = CellXBounds(positions, 4);
	auto [yMin, yMax] = CellYBounds(positions, 4);

	EXPECT_FLOAT_EQ(8.0f, xMin);
	EXPECT_FLOAT_EQ(192.0f, xMax);
	EXPECT_FLOAT_EQ(8.0f, yMin);
	EXPECT_FLOAT_EQ(92.0f, yMax);
}

TEST(NinePatchImageTest, BuildPositions_ZeroBorderFillsWholeImage)
{
	auto positions = NinePatchImage::BuildPositions(0, 0, { 140, 100 });
	auto [xMin, xMax] = CellXBounds(positions, 4);
	auto [yMin, yMax] = CellYBounds(positions, 4);

	EXPECT_FLOAT_EQ(0.0f, xMin);
	EXPECT_FLOAT_EQ(140.0f, xMax);
	EXPECT_FLOAT_EQ(0.0f, yMin);
	EXPECT_FLOAT_EQ(100.0f, yMax);
}

TEST(NinePatchImageTest, BuildPositions_MinimumSize)
{
	const auto size = utils::Size2d{ 10, 10 };
	auto positions = NinePatchImage::BuildPositions(8, 8, size);

	for (auto i = 0u; i < positions.size(); i += 3)
	{
		EXPECT_GE(positions[i], 0.0f);
		EXPECT_LE(positions[i], static_cast<float>(size.Width));
		EXPECT_GE(positions[i + 1], 0.0f);
		EXPECT_LE(positions[i + 1], static_cast<float>(size.Height));
	}
}
