
#include "gtest/gtest.h"
#include <regex>
#include "resources/ResourceLib.h"
#include "io/Json.h"
#include "io/UserConfig.h"

using io::Json;
using io::UserConfig;

TEST(UserConfig, ParsesAudioSettings) {
	auto str = "{\"name\":\"Soundblaster\",\"bufsize\":12,\"latency\":212,\"numchannelsin\":6,\"numchannelsout\":8}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto audio = UserConfig::AudioSettings::FromJson(json);

	ASSERT_TRUE(audio.has_value());
	ASSERT_EQ(0, audio.value().Name.compare("Soundblaster"));
	ASSERT_EQ(12, audio.value().BufSize);
	ASSERT_EQ(212, audio.value().LatencyIn);
	ASSERT_EQ(212, audio.value().LatencyOut);
	ASSERT_EQ(6, audio.value().NumChannelsIn);
	ASSERT_EQ(8, audio.value().NumChannelsOut);
}

TEST(UserConfig, ParsesLoopSettings) {
	auto str = "{\"fadeSamps\":13}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto loop = UserConfig::LoopSettings::FromJson(json);

	ASSERT_TRUE(loop.has_value());
	ASSERT_EQ(13, loop.value().FadeSamps);
}

TEST(UserConfig, ParsesTriggerSettings) {
	auto str = "{\"preDelay\":42,\"debounceSamps\":59}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = UserConfig::TriggerSettings::FromJson(json);

	ASSERT_TRUE(trig.has_value());
	ASSERT_EQ(42, trig.value().PreDelay);
	ASSERT_EQ(59, trig.value().DebounceSamps);
}

TEST(UserConfig, ParsesFile) {
	std::string audio = "{\"name\":\"HDMI\",\"bufsize\":255,\"latency\":414,\"numchannelsin\":0,\"numchannelsout\":10}";
	std::string loop = "{\"fadeSamps\":54}";
	std::string trigger = "{\"preDelay\":21,\"debounceSamps\":18}";
	
	auto str = "{\"name\":\"user\",\"audio\":" + audio + ",\"loop\":" + loop + ",\"trigger\":" + trigger + "}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto cfg = UserConfig::FromJson(json);

	ASSERT_TRUE(cfg.has_value());

	ASSERT_EQ(0, cfg.value().Audio.Name.compare("HDMI"));
	ASSERT_EQ(255, cfg.value().Audio.BufSize);
	ASSERT_EQ(414, cfg.value().Audio.LatencyIn);
	ASSERT_EQ(414, cfg.value().Audio.LatencyOut);
	ASSERT_EQ(0, cfg.value().Audio.NumChannelsIn);
	ASSERT_EQ(10, cfg.value().Audio.NumChannelsOut);

	ASSERT_EQ(54, cfg.value().Loop.FadeSamps);

	ASSERT_EQ(21, cfg.value().Trigger.PreDelay);
	ASSERT_EQ(18, cfg.value().Trigger.DebounceSamps);
}