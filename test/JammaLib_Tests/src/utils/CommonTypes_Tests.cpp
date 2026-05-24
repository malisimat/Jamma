
#include "gtest/gtest.h"
#include "./utils/CommonTypes.h"

using utils::Position2d;
using utils::Size2d;

TEST(Position2d, CanIncrement) {
	Position2d posA = { 2, 7 };
	Position2d posB = { -8, 3 };
	posA+= posB;

	ASSERT_EQ(-6, posA.X);
	ASSERT_EQ(10, posA.Y);
}

TEST(Position2d, CanAdd) {
	Position2d posA = { 2, 7 };
	Position2d posB = { -8, 3 };
	auto res = posA + posB;

	ASSERT_EQ(-6, res.X);
	ASSERT_EQ(10, res.Y);
}

TEST(Position2d, CanSubtract) {
	Position2d posA = { 2, 13 };
	Position2d posB = { 8, 3 };
	auto res = posA - posB;

	ASSERT_EQ(-6, res.X);
	ASSERT_EQ(10, res.Y);
}

TEST(Position2d, CanMultiply) {
	Position2d posA = { 2, 13 };
	Position2d posB = { 8, 3 };
	auto res = posA * posB;

	ASSERT_EQ(16, res.X);
	ASSERT_EQ(39, res.Y);
}

TEST(Position2d, ClampsMin) {
	Position2d pos = { 2, 1 };
	Position2d posMin = { 4, 5 };
	Position2d posMax = { 100, 100 };
	auto res = Position2d::Clamp(pos, posMin, posMax);

	ASSERT_EQ(4, res.X);
	ASSERT_EQ(5, res.Y);
}

TEST(Position2d, ClampsMax) {
	Position2d pos = { 22, 13 };
	Position2d posMin = { 1, 1 };
	Position2d posMax = { 4, 5 };
	auto res = Position2d::Clamp(pos, posMin, posMax);

	ASSERT_EQ(4, res.X);
	ASSERT_EQ(5, res.Y);
}

TEST(Position2d, ClampsMinMax) {
	Position2d pos = { 2, 13 };
	Position2d posMin = { 4, 5 };
	Position2d posMax = { 100, 10 };
	auto res = Position2d::Clamp(pos, posMin, posMax);

	ASSERT_EQ(4, res.X);
	ASSERT_EQ(10, res.Y);
}

////////////////////////////////
// Size2d Tests
////////////////////////////////
TEST(Size2d, CanIncrement) {
	Size2d sizeA = { 2, 13 };
	Size2d sizeB = { 8, 3 };
	sizeA += sizeB;

	ASSERT_EQ(10, sizeA.Width);
	ASSERT_EQ(16, sizeA.Height);
}

TEST(Size2d, CanAdd) {
	Size2d sizeA = { 2, 13 };
	Size2d sizeB = { 8, 3 };
	auto res = sizeA + sizeB;

	ASSERT_EQ(10, res.Width);
	ASSERT_EQ(16, res.Height);
}

TEST(Size2d, CanSubtract) {
	Size2d sizeA = { 2, 13 };
	Size2d sizeB = { 8, 3 };
	auto res = sizeA - sizeB;

	ASSERT_EQ(0, res.Width);
	ASSERT_EQ(10, res.Height);
}

TEST(Size2d, CanMultiply) {
	Size2d sizeA = { 2, 13 };
	Size2d sizeB = { 8, 3 };
	auto res = sizeA * sizeB;

	ASSERT_EQ(16, res.Width);
	ASSERT_EQ(39, res.Height);
}

TEST(Size2d, CanMultiplyInPlace) {
	Size2d sizeA = { 2, 13 };
	Size2d sizeB = { 8, 3 };
	sizeA *= sizeB;

	ASSERT_EQ(16, sizeA.Width);
	ASSERT_EQ(39, sizeA.Height);
}

TEST(Size2d, CanCompare) {
	Size2d sizeA = { 2, 13 };
	Size2d sizeB = { 8, 3 };
	Size2d sizeC = { 8, 3 };
	ASSERT_FALSE(sizeA == sizeB);
	ASSERT_FALSE(sizeA == sizeC);
	ASSERT_TRUE(sizeB == sizeC);
}

TEST(Size2d, ClampsMin) {
	Size2d size = { 2, 1 };
	Size2d sizeMin = { 4, 5 };
	Size2d sizeMax = { 100, 100 };
	auto res = Size2d::Clamp(size, sizeMin, sizeMax);

	ASSERT_EQ(4, res.Width);
	ASSERT_EQ(5, res.Height);
}

TEST(Size2d, ClampsMax) {
	Size2d size = { 22, 13 };
	Size2d sizeMin = { 1, 1 };
	Size2d sizeMax = { 4, 5 };
	auto res = Size2d::Clamp(size, sizeMin, sizeMax);

	ASSERT_EQ(4, res.Width);
	ASSERT_EQ(5, res.Height);
}

TEST(Size2d, ClampsMinMax) {
	Size2d size = { 2, 13 };
	Size2d sizeMin = { 4, 5 };
	Size2d sizeMax = { 100, 10 };
	auto res = Size2d::Clamp(size, sizeMin, sizeMax);

	ASSERT_EQ(4, res.Width);
	ASSERT_EQ(10, res.Height);
}

TEST(Size2d, HitTestInside) {
	Size2d rect = { 10, 20 };
	Position2d pos = { 5, 10 };
	ASSERT_TRUE(Size2d::RectTest(rect, pos));
}

TEST(Size2d, HitTestOutsideX) {
	Size2d rect = { 10, 20 };
	Position2d pos = { 15, 10 };
	ASSERT_FALSE(Size2d::RectTest(rect, pos));
}

TEST(Size2d, HitTestOutsideY) {
	Size2d rect = { 10, 20 };
	Position2d pos = { 5, -5 };
	ASSERT_FALSE(Size2d::RectTest(rect, pos));
}
