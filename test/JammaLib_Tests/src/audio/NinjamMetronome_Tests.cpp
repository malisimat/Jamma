#include "gtest/gtest.h"
#include "audio/NinjamMetronome.h"

#include <algorithm>
#include <cmath>
#include <vector>

TEST(NinjamMetronome, GeneratedTablesAreFiniteAndNonEmpty)
{
	audio::NinjamMetronome metronome;
	metronome.Configure(48000u);

	ASSERT_FALSE(metronome.NormalTable().empty());
	ASSERT_FALSE(metronome.AccentTable().empty());
	EXPECT_TRUE(std::all_of(metronome.NormalTable().begin(), metronome.NormalTable().end(),
		[](float sample) { return std::isfinite(sample); }));
	EXPECT_TRUE(std::all_of(metronome.AccentTable().begin(), metronome.AccentTable().end(),
		[](float sample) { return std::isfinite(sample); }));
}

TEST(NinjamMetronome, MixesKnownOnsetToEveryOutputChannelOnly)
{
	audio::NinjamMetronome metronome;
	metronome.Configure(48000u);
	std::vector<float> output(16u, 0.0f);
	const auto exportBuffer = output;

	ninjam::NinjamMetronomeTimingResult timing;
	timing.Valid = true;
	timing.Onsets[0] = { 2u, false };
	timing.OnsetCount = 1u;
	metronome.Mix(output.data(), 2u, 8u, timing);

	EXPECT_EQ(output[0], 0.0f);
	EXPECT_EQ(output[1], 0.0f);
	EXPECT_EQ(output[4], output[5]);
	EXPECT_TRUE(std::any_of(output.begin() + 4, output.end(),
		[](float sample) { return sample != 0.0f; }));
	EXPECT_EQ(exportBuffer, std::vector<float>(16u, 0.0f));
}

TEST(NinjamMetronome, LevelScalesBothClickVoicesAndTheirTails)
{
	audio::NinjamMetronome full, quiet;
	full.Configure(48000u); quiet.Configure(48000u);
	for (const bool accent : {false, true})
	{
		full.Reset(); quiet.Reset();
		ninjam::NinjamMetronomeTimingResult timing;
		timing.Valid = true; timing.Onsets[0] = {0u, accent}; timing.OnsetCount = 1u;
		for (int block = 0; block < 2; ++block)
		{
			std::array<float, 64> a{}, b{};
			full.Mix(a.data(), 1u, 64u, timing);
			quiet.Mix(b.data(), 1u, 64u, timing, 0.25f);
			for (size_t index = 0; index < a.size(); ++index)
				EXPECT_FLOAT_EQ(a[index] * 0.25f, b[index]);
			timing.OnsetCount = 0u;
		}
	}
}
