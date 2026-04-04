
#include "gtest/gtest.h"
#include "audio/Hanning.h"

using audio::Hanning;

// Verify that fadeIn[i] + fadeOut[i] == 1.0 for all positions in the crossfade
// window. This ensures overlapping identical content produces no gain bump.
TEST(Hanning, CoeffsSumToOne)
{
	const unsigned int sizes[] = { 1, 2, 10, 20, 100 };
	for (auto size : sizes)
	{
		Hanning h(size);
		for (auto i = 0u; i < size; i++)
		{
			auto [fadeIn, fadeOut] = h[i];
			EXPECT_NEAR(fadeIn + fadeOut, 1.0f, 1e-6f)
				<< "size=" << size << " i=" << i;
		}
	}
}

// Constant-value input must produce the same constant value at every crossfade
// position (unity-gain invariant).
TEST(Hanning, ConstantInput_NoGainBump)
{
	const unsigned int sizes[] = { 1, 2, 20 };
	const float val = 0.5f;
	for (auto size : sizes)
	{
		Hanning h(size);
		for (auto i = 0u; i < size; i++)
		{
			float mixed = h.Mix(val, val, i);
			EXPECT_NEAR(mixed, val, 1e-6f)
				<< "size=" << size << " i=" << i;
		}
	}
}

// Out-of-range index: operator[] returns {1,0} and Mix returns the first arg.
TEST(Hanning, OutOfRange_ReturnsFadeInDefault)
{
	Hanning h(10);

	float result = h.Mix(0.7f, 0.3f, 10);
	EXPECT_FLOAT_EQ(result, 0.7f);

	auto [fadeIn, fadeOut] = h[10];
	EXPECT_FLOAT_EQ(fadeIn, 1.0f);
	EXPECT_FLOAT_EQ(fadeOut, 0.0f);
}

// At the first sample the fadeOut coefficient should dominate (loop end plays
// through), and at the last sample fadeIn should dominate (loop start fades in).
TEST(Hanning, FadeDirectionality)
{
	Hanning h(20);

	auto [fadeIn0, fadeOut0] = h[0];
	auto [fadeInEnd, fadeOutEnd] = h[19];

	EXPECT_LT(fadeIn0, 0.1f);
	EXPECT_GT(fadeOut0, 0.9f);
	EXPECT_GT(fadeInEnd, 0.9f);
	EXPECT_LT(fadeOutEnd, 0.1f);
}

// Block-boundary case: crossfade that straddles a block boundary should still
// produce unity gain for constant input across all positions.
TEST(Hanning, BlockBoundary_UnityGain)
{
	const unsigned int fadeSamps = 20u;
	Hanning h(fadeSamps);

	// Simulate two back-to-back calls that together span the full window.
	// First half: samples 0 .. fadeSamps/2 - 1
	// Second half: samples fadeSamps/2 .. fadeSamps - 1
	const float val = 0.75f;
	const unsigned int half = fadeSamps / 2;

	for (auto i = 0u; i < half; i++)
		EXPECT_NEAR(h.Mix(val, val, i), val, 1e-6f) << "first half, i=" << i;

	for (auto i = half; i < fadeSamps; i++)
		EXPECT_NEAR(h.Mix(val, val, i), val, 1e-6f) << "second half, i=" << i;
}
