#include "gtest/gtest.h"
#include "resources/ResourceLib.h"

using resources::ResourceLib;

TEST(ResourceLibTextureArgsTest, AcceptsNoArgsAsNormalTexture)
{
	bool isNinePatch = true;
	unsigned int borderX = 99;
	unsigned int borderY = 77;

	const auto parsed = ResourceLib::ParseTextureArgs({}, isNinePatch, borderX, borderY);

	ASSERT_TRUE(parsed);
	EXPECT_FALSE(isNinePatch);
	EXPECT_EQ(0u, borderX);
	EXPECT_EQ(0u, borderY);
}

TEST(ResourceLibTextureArgsTest, AcceptsStrictNinePatchArgs)
{
	bool isNinePatch = false;
	unsigned int borderX = 0;
	unsigned int borderY = 0;

	const auto parsed = ResourceLib::ParseTextureArgs({ "ninepatch", "12", "34" }, isNinePatch, borderX, borderY);

	ASSERT_TRUE(parsed);
	EXPECT_TRUE(isNinePatch);
	EXPECT_EQ(12u, borderX);
	EXPECT_EQ(34u, borderY);
}

TEST(ResourceLibTextureArgsTest, RejectsMalformedArgs)
{
	bool isNinePatch = false;
	unsigned int borderX = 0;
	unsigned int borderY = 0;

	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "ninepatch", "12" }, isNinePatch, borderX, borderY));
	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "ninepatch", "12", "x" }, isNinePatch, borderX, borderY));
	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "ninepatch", "-1", "2" }, isNinePatch, borderX, borderY));
	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "ninepatch", "1", "2", "3" }, isNinePatch, borderX, borderY));
	EXPECT_FALSE(ResourceLib::ParseTextureArgs({ "stretch", "1", "2" }, isNinePatch, borderX, borderY));
}
