#include "gtest/gtest.h"
#include "utils/MathUtils.h"

// Regression test for a signed/unsigned modulo bug: v % len with an unsigned
// len converts a negative v to unsigned FIRST (usual arithmetic conversions),
// so the classic ((v%len)+len)%len idiom only produced a correct result when
// len happened to be a power of 2. Verified against plain mathematical
// modulo for representative non-power-of-2 lengths (see build-notes.md).
TEST(MathUtils, ModNegMatchesMathematicalModuloForNegativeValues)
{
	const unsigned int lengths[] = { 5u, 17u, 100u, 1000000u };

	for (auto len : lengths)
	{
		for (int v = -3; v <= 3; v++)
		{
			auto expected = ((v % static_cast<int>(len)) + static_cast<int>(len)) % static_cast<int>(len);
			EXPECT_EQ(utils::ModNeg(v, len), static_cast<unsigned int>(expected))
				<< "v=" << v << " len=" << len;
		}
	}
}

TEST(MathUtils, ModNegHandlesNonNegativeValuesAndWrap)
{
	EXPECT_EQ(utils::ModNeg(0, 17u), 0u);
	EXPECT_EQ(utils::ModNeg(16, 17u), 16u);
	EXPECT_EQ(utils::ModNeg(17, 17u), 0u);
	EXPECT_EQ(utils::ModNeg(34, 17u), 0u);
	EXPECT_EQ(utils::ModNeg(-1, 17u), 16u);
	EXPECT_EQ(utils::ModNeg(-17, 17u), 0u);
	EXPECT_EQ(utils::ModNeg(-18, 17u), 16u);
}
