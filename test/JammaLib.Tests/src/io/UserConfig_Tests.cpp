
#include "gtest/gtest.h"
#include <regex>
#include "resources/ResourceLib.h"
#include "io/Json.h"
#include "io/UserConfig.h"

using io::Json;
using io::UserConfig;

TEST(UserConfig, ParsesAudioSettings) {
	auto str = "{\"name\":\"Soundblaster\",\"bufsize\":12,\"inlatency\":212,\"outlatency\":212,\"numchannelsin\":6,\"numchannelsout\":8}";
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
	auto str = "{\"fadeSamps\":13,\"seedGrainMinMs\":450,\"seedGrainTargetMaxMs\":2800,\"seedBpmMin\":90,\"seedQuantisation\":\"multiple\"}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto loop = UserConfig::LoopSettings::FromJson(json);

	ASSERT_TRUE(loop.has_value());
	ASSERT_EQ(13, loop.value().FadeSamps);
	ASSERT_EQ(450u, loop.value().SeedGrainMinMs);
	ASSERT_EQ(2800u, loop.value().SeedGrainTargetMaxMs);
	ASSERT_EQ(90u, loop.value().SeedBpmMin);
	ASSERT_FALSE(loop.value().SeedUsesPowers);
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
	std::string audio = "{\"name\":\"HDMI\",\"bufsize\":255,\"inlatency\":414,\"outlatency\":414,\"numchannelsin\":0,\"numchannelsout\":10}";
	std::string loop = "{\"fadeSamps\":54,\"seedGrainMinMs\":400,\"seedGrainTargetMaxMs\":3000,\"seedBpmMin\":80,\"seedQuantisation\":\"power\"}";
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
	ASSERT_EQ(400u, cfg.value().Loop.SeedGrainMinMs);
	ASSERT_EQ(3000u, cfg.value().Loop.SeedGrainTargetMaxMs);
	ASSERT_EQ(80u, cfg.value().Loop.SeedBpmMin);
	ASSERT_TRUE(cfg.value().Loop.SeedUsesPowers);

	ASSERT_EQ(21, cfg.value().Trigger.PreDelay);
	ASSERT_EQ(18, cfg.value().Trigger.DebounceSamps);
}

TEST(UserConfig, DeducesDefaultLoopTimingFromLongLoop) {
	UserConfig cfg;

	auto timing = cfg.DeduceLoopTiming(48000ul * 8ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	ASSERT_EQ(96000u, timing->GrainSamps);
	ASSERT_EQ(4u, timing->LoopGrains);
	ASSERT_EQ(4u, timing->BeatsPerGrain);
	ASSERT_FLOAT_EQ(120.0f, timing->Bpm);
	ASSERT_EQ(16u, timing->Bpi);
}

TEST(UserConfig, DeducesDefaultLoopTimingBelowThreeSecondsWhenPossible) {
	UserConfig cfg;

	auto timing = cfg.DeduceLoopTiming(48000ul * 6ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	ASSERT_EQ(72000u, timing->GrainSamps);
	ASSERT_EQ(4u, timing->LoopGrains);
	ASSERT_EQ(2u, timing->BeatsPerGrain);
	ASSERT_FLOAT_EQ(80.0f, timing->Bpm);
	ASSERT_EQ(8u, timing->Bpi);
}

TEST(UserConfig, LoopTimingHonoursConfiguredTargetMaxGrain) {
	UserConfig cfg;
	cfg.Loop.SeedGrainTargetMaxMs = 5000u;

	auto timing = cfg.DeduceLoopTiming(48000ul * 6ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	ASSERT_EQ(144000u, timing->GrainSamps);
	ASSERT_EQ(2u, timing->LoopGrains);
	ASSERT_EQ(4u, timing->BeatsPerGrain);
	ASSERT_FLOAT_EQ(80.0f, timing->Bpm);
	ASSERT_EQ(8u, timing->Bpi);
}
