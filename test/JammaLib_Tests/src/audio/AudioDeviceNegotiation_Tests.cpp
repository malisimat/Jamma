#include <gtest/gtest.h>
#include "audio/AudioDevice.h"

TEST(AudioDeviceNegotiation, GeneratedPairsReachStereoAndMono)
{
	const auto many = audio::AudioDevice::GeneratedChannelPairs(8u, 8u);
	ASSERT_FALSE(many.empty());
	EXPECT_EQ(many.front(), (std::pair<unsigned int, unsigned int>{8u, 8u}));
	EXPECT_NE(std::find(many.begin(), many.end(),
		std::pair<unsigned int, unsigned int>{2u, 2u}), many.end());
	const auto wide = audio::AudioDevice::GeneratedChannelPairs(32u, 32u);
	EXPECT_NE(std::find(wide.begin(), wide.end(),
		std::pair<unsigned int, unsigned int>{25u, 32u}), wide.end());
	EXPECT_NE(std::find(wide.begin(), wide.end(),
		std::pair<unsigned int, unsigned int>{2u, 2u}), wide.end());
	const auto mono = audio::AudioDevice::GeneratedChannelPairs(1u, 2u);
	EXPECT_EQ(mono.front(), (std::pair<unsigned int, unsigned int>{1u, 2u}));
	EXPECT_EQ(audio::AudioDevice::GeneratedChannelPairs(0u, 2u).front(),
		(std::pair<unsigned int, unsigned int>{0u, 2u}));
	EXPECT_TRUE(audio::AudioDevice::GeneratedChannelPairs(2u, 0u).empty());
}

TEST(AudioDeviceNegotiation, RatesAreOrderedByDistanceThenLowerRate)
{
	const auto rates = audio::AudioDevice::OrderedRates({96000u, 48000u, 44100u, 48000u}, 46050u);
	EXPECT_EQ(rates, (std::vector<unsigned int>{44100u, 48000u, 96000u}));
}

TEST(AudioDeviceNegotiation, GeneratedPrefersDefaultOutputWhenDefaultsDiffer)
{
	audio::AsioInventory inventory;
	inventory.DefaultInputId = 1u;
	inventory.DefaultOutputId = 2u;
	inventory.Devices = {
		{ 0u, "other", true, "", 2u, 2u, {44100u}, 44100u },
		{ 1u, "input", true, "", 8u, 2u, {44100u}, 44100u, true, false },
		{ 2u, "output", true, "", 2u, 8u, {44100u}, 44100u, false, true }
	};
	io::UserConfig::AudioSettings settings{};
	settings.Name = "other";
	settings.NumChannelsIn = 2u;
	settings.NumChannelsOut = 2u;
	EXPECT_EQ(audio::AudioDevice::OrderedCandidateIds(inventory, settings, true),
		(std::vector<unsigned int>{2u, 1u, 0u}));
	EXPECT_EQ(audio::AudioDevice::OrderedCandidateIds(inventory, settings, false),
		(std::vector<unsigned int>{0u, 2u, 1u}));
}

TEST(AudioDeviceNegotiation, GeneratedPrefersDuplexOverOutputOnlyDefault)
{
	audio::AsioInventory inventory;
	inventory.DefaultOutputId = 0u;
	inventory.DefaultInputId = 1u;
	inventory.Devices = {
		{ 0u, "output only", true, "", 0u, 8u, {48000u}, 48000u },
		{ 1u, "duplex", true, "", 2u, 2u, {48000u}, 48000u }
	};
	io::UserConfig::AudioSettings settings{};
	EXPECT_EQ(audio::AudioDevice::OrderedCandidateIds(inventory, settings, true),
		(std::vector<unsigned int>{1u, 0u}));
}

TEST(AudioDeviceNegotiation, GeneratedRanksStereoOutputAheadOfMonoOutput)
{
	audio::AsioInventory inventory;
	inventory.DefaultOutputId = 0u;
	inventory.Devices = {
		{ 0u, "mono output", true, "", 8u, 1u, {48000u}, 48000u },
		{ 1u, "stereo duplex", true, "", 2u, 2u, {48000u}, 48000u }
	};
	EXPECT_EQ(audio::AudioDevice::OrderedCandidateIds(inventory, {}, true),
		(std::vector<unsigned int>{1u, 0u}));
}

TEST(AudioDeviceNegotiation, BoundedPlanAlwaysReachesStereoAtEverySupportedRate)
{
	const std::vector<unsigned int> rates{
		44100u, 48000u, 88200u, 96000u, 176400u, 192000u,
		22050u, 32000u, 64000u, 128000u, 256000u, 384000u };
	const auto stereo = audio::AudioDevice::GeneratedAttemptPlan(128u, 128u, rates, true);
	ASSERT_LE(stereo.size(), 2048u);
	EXPECT_EQ(stereo.front().InputChannels, 128u);
	EXPECT_EQ(stereo.front().OutputChannels, 128u);
	for (const auto rate : rates)
		EXPECT_NE(std::find_if(stereo.begin(), stereo.end(), [rate](const audio::AsioConfiguration& c)
			{ return c.InputChannels == 2u && c.OutputChannels == 2u && c.SampleRate == rate; }),
			stereo.end());
	EXPECT_TRUE(std::all_of(stereo.begin(), stereo.end(), [](const audio::AsioConfiguration& c)
		{ return c.InputChannels >= 2u && c.OutputChannels >= 2u; }));
	const auto degraded = audio::AudioDevice::GeneratedAttemptPlan(128u, 128u, rates, false);
	EXPECT_TRUE(std::all_of(degraded.begin(), degraded.end(), [](const audio::AsioConfiguration& c)
		{ return c.InputChannels < 2u || c.OutputChannels < 2u; }));
}
