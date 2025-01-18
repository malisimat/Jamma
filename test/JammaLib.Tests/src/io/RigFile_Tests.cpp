
#include "gtest/gtest.h"
#include <regex>
#include "resources/ResourceLib.h"
#include "io/Json.h"
#include "io/RigFile.h"

using io::Json;
using io::RigFile;

const std::string TriggerPairString = "{\"activatedown\":%ADOWN%,\"activateup\":11,\"ditchdown\":%DDOWN%,\"ditchup\":12}";

TEST(RigFile, ParsesAudioSettings) {
	auto str = "{\"name\":\"Soundblaster\",\"bufsize\":12,\"latency\":212,\"numchannelsin\":6,\"numchannelsout\":8}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto audio = io::UserConfig::AudioSettings::FromJson(json);

	ASSERT_TRUE(audio.has_value());
	ASSERT_EQ(0, audio.value().Name.compare("Soundblaster"));
	ASSERT_EQ(12, audio.value().BufSize);
	ASSERT_EQ(212, audio.value().LatencyIn);
	ASSERT_EQ(212, audio.value().LatencyOut);
	ASSERT_EQ(6, audio.value().NumChannelsIn);
	ASSERT_EQ(8, audio.value().NumChannelsOut);
}

TEST(RigFile, ParsesTriggerPair) {
	auto str = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "1"), std::regex("%DDOWN%"), "2");
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto pair = RigFile::TriggerPair::FromJson(json);

	ASSERT_TRUE(pair.has_value());
	ASSERT_EQ(1, pair.value().ActivateDown);
	ASSERT_EQ(11, pair.value().ActivateUp);
	ASSERT_EQ(2, pair.value().DitchDown);
	ASSERT_EQ(12, pair.value().DitchUp);
}

TEST(RigFile, ParsesTrigger) {
	auto pair1 = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "1"), std::regex("%DDOWN%"), "2");
	auto pair2 = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "3"), std::regex("%DDOWN%"), "4");
	auto pair3 = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "5"), std::regex("%DDOWN%"), "6");

	auto str = "{\"name\":\"trig\",\"stationtype\":31,\"pairs\":[" + pair1 + "," + pair2 + "," + pair3 + "]}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = RigFile::Trigger::FromJson(json);

	ASSERT_TRUE(trig.has_value());
	ASSERT_EQ(0, trig.value().Name.compare("trig"));
	ASSERT_EQ(3, trig.value().TriggerPairs.size());
	ASSERT_EQ(31, trig.value().StationType);

	ASSERT_EQ(1, trig.value().TriggerPairs[0].ActivateDown);
	ASSERT_EQ(11, trig.value().TriggerPairs[0].ActivateUp);
	ASSERT_EQ(2, trig.value().TriggerPairs[0].DitchDown);
	ASSERT_EQ(12, trig.value().TriggerPairs[0].DitchUp);

	ASSERT_EQ(3, trig.value().TriggerPairs[1].ActivateDown);
	ASSERT_EQ(4, trig.value().TriggerPairs[1].DitchDown);

	ASSERT_EQ(5, trig.value().TriggerPairs[2].ActivateDown);
	ASSERT_EQ(6, trig.value().TriggerPairs[2].DitchDown);
}

TEST(RigFile, ParsesFile) {
	std::string audio = "{\"name\":\"HDMI\",\"bufsize\":255,\"latency\":414,\"numchannelsin\":0,\"numchannelsout\":10}";
	
	auto pair1 = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "1"), std::regex("%DDOWN%"), "2");
	auto pair2 = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "3"), std::regex("%DDOWN%"), "4");
	auto trig1 = "{\"name\":\"trig1\",\"stationtype\":31,\"pairs\":[" + pair1 + "," + pair2 + "]}";

	auto pair3 = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "5"), std::regex("%DDOWN%"), "6");
	auto trig2 = "{\"name\":\"trig2\",\"stationtype\":32,\"pairs\":[" + pair3 + "]}";
		
	auto str = "{\"name\":\"rig\",\"user\":{\"audio\":" + audio + "},\"triggers\":[" + trig1 + "," + trig2 + "]}";
	auto testStream = std::stringstream(str);
	auto rig = RigFile::FromStream(std::move(testStream));

	ASSERT_TRUE(rig.has_value());
	ASSERT_EQ(RigFile::VERSION_V, rig.value().Version);
	ASSERT_EQ(0, rig.value().Name.compare("rig"));
	
	ASSERT_EQ(0, rig.value().User.Audio.Name.compare("HDMI"));
	ASSERT_EQ(255, rig.value().User.Audio.BufSize);
	ASSERT_EQ(414, rig.value().User.Audio.LatencyIn);
	ASSERT_EQ(414, rig.value().User.Audio.LatencyOut);
	ASSERT_EQ(0, rig.value().User.Audio.NumChannelsIn);
	ASSERT_EQ(10, rig.value().User.Audio.NumChannelsOut);

	ASSERT_EQ(2, rig.value().Triggers.size());

	ASSERT_EQ(0, rig.value().Triggers[0].Name.compare("trig1"));
	ASSERT_EQ(31, rig.value().Triggers[0].StationType);
	ASSERT_EQ(2, rig.value().Triggers[0].TriggerPairs.size());

	ASSERT_EQ(1, rig.value().Triggers[0].TriggerPairs[0].ActivateDown);
	ASSERT_EQ(11, rig.value().Triggers[0].TriggerPairs[0].ActivateUp);
	ASSERT_EQ(2, rig.value().Triggers[0].TriggerPairs[0].DitchDown);
	ASSERT_EQ(12, rig.value().Triggers[0].TriggerPairs[0].DitchUp);

	ASSERT_EQ(3, rig.value().Triggers[0].TriggerPairs[1].ActivateDown);
	ASSERT_EQ(4, rig.value().Triggers[0].TriggerPairs[1].DitchDown);

	ASSERT_EQ(0, rig.value().Triggers[1].Name.compare("trig2"));
	ASSERT_EQ(32, rig.value().Triggers[1].StationType);
	ASSERT_EQ(1, rig.value().Triggers[1].TriggerPairs.size());

	ASSERT_EQ(5, rig.value().Triggers[1].TriggerPairs[0].ActivateDown);
	ASSERT_EQ(6, rig.value().Triggers[1].TriggerPairs[0].DitchDown);
}