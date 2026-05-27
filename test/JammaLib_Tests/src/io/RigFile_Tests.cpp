
#include "gtest/gtest.h"
#include <regex>
#include "resources/ResourceLib.h"
#include "io/Json.h"
#include "io/RigFile.h"

using io::Json;
using io::RigFile;

const std::string TriggerPairString = "{\"activatedown\":%ADOWN%,\"activateup\":11,\"ditchdown\":%DDOWN%,\"ditchup\":12}";
const std::string SerialTriggerPairString = "{\"source\":\"serial\",\"device\":\"pedal-a\",\"activatedown\":%ADOWN%,\"activateup\":%ADOWN%,\"ditchdown\":%DDOWN%,\"ditchup\":%DDOWN%}";

TEST(RigFile, ParsesAudioSettings) {
	auto str = "{\"name\":\"Soundblaster\",\"bufsize\":12,\"inlatency\":212,\"outlatency\":212,\"numchannelsin\":6,\"numchannelsout\":8}";
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

TEST(RigFile, ParsesSerialTriggerPairSource) {
	auto str = std::regex_replace(std::regex_replace(SerialTriggerPairString, std::regex("%ADOWN%"), "0"), std::regex("%DDOWN%"), "1");
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto pair = RigFile::TriggerPair::FromJson(json);

	ASSERT_TRUE(pair.has_value());
	ASSERT_EQ(RigFile::TriggerPair::SOURCE_SERIAL, pair.value().Source);
	ASSERT_EQ(0, pair.value().Device.compare("pedal-a"));
	ASSERT_EQ(0u, pair.value().ActivateDown);
	ASSERT_EQ(0u, pair.value().ActivateUp);
	ASSERT_EQ(1u, pair.value().DitchDown);
	ASSERT_EQ(1u, pair.value().DitchUp);
}

TEST(RigFile, ParsesSerialTriggerPairEmptyDeviceAsDefault) {
	auto str = "{\"source\":\"serial\",\"device\":\"\",\"activatedown\":0,\"activateup\":0,\"ditchdown\":1,\"ditchup\":1}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto pair = RigFile::TriggerPair::FromJson(json);

	ASSERT_TRUE(pair.has_value());
	ASSERT_EQ(RigFile::TriggerPair::SOURCE_SERIAL, pair.value().Source);
	ASSERT_EQ(0, pair.value().Device.compare("default"));
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

TEST(RigFile, ParsesMidiInputChannelsAsOneBasedRigValues) {
	auto pair = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "51"), std::regex("%DDOWN%"), "52");
	auto str = "{\"name\":\"Trig2\",\"stationtype\":0,\"pairs\":[" + pair + "],\"midiinput\":[1,16,1,0,17],\"midiinputdevices\":[\"Keys A\",\"Keys B\",\"Keys A\"]}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = RigFile::Trigger::FromJson(json);

	ASSERT_TRUE(trig.has_value());
	ASSERT_EQ(2u, trig.value().MidiInputChannels.size());
	EXPECT_EQ(0u, trig.value().MidiInputChannels[0]);
	EXPECT_EQ(15u, trig.value().MidiInputChannels[1]);
	ASSERT_EQ(2u, trig.value().MidiInputDevices.size());
	EXPECT_EQ(0, trig.value().MidiInputDevices[0].compare("Keys A"));
	EXPECT_EQ(0, trig.value().MidiInputDevices[1].compare("Keys B"));
}

TEST(RigFile, ParsesMidiTriggerBinding) {
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"TriggerPad\",\"activate\":{\"kind\":\"note\",\"channel\":10,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = RigFile::Trigger::FromJson(json);

	ASSERT_TRUE(trig.has_value());
	ASSERT_TRUE(trig.value().MidiTrigger.has_value());
	EXPECT_EQ(0, trig.value().Name.compare("TrigMidi"));
	EXPECT_EQ(0, trig.value().MidiTrigger->Device.compare("TriggerPad"));
	EXPECT_EQ(RigFile::MidiTriggerEvent::NOTE, trig.value().MidiTrigger->Activate.Kind);
	EXPECT_EQ(9u, trig.value().MidiTrigger->Activate.Channel);
	EXPECT_EQ(60u, trig.value().MidiTrigger->Activate.Id);
	EXPECT_EQ(1u, trig.value().MidiTrigger->Activate.State);
	EXPECT_EQ(RigFile::MidiTriggerEvent::CC, trig.value().MidiTrigger->Ditch.Kind);
	EXPECT_EQ(0u, trig.value().MidiTrigger->Ditch.Channel);
	EXPECT_EQ(64u, trig.value().MidiTrigger->Ditch.Id);
	EXPECT_EQ(1u, trig.value().MidiTrigger->Ditch.State);
}

TEST(RigFile, ParsesNoteOnAndNoteOffMidiTriggerBindingKinds) {
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"activate\":{\"kind\":\"note-on\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"noteoff\",\"channel\":1,\"id\":61}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = RigFile::Trigger::FromJson(json);

	ASSERT_TRUE(trig.has_value());
	ASSERT_TRUE(trig.value().MidiTrigger.has_value());
	EXPECT_EQ(RigFile::MidiTriggerEvent::NOTE, trig.value().MidiTrigger->Activate.Kind);
	EXPECT_EQ(1u, trig.value().MidiTrigger->Activate.State);
	EXPECT_EQ(RigFile::MidiTriggerEvent::NOTE, trig.value().MidiTrigger->Ditch.Kind);
	EXPECT_EQ(0u, trig.value().MidiTrigger->Ditch.State);
}

TEST(RigFile, RejectsMidiTriggerBindingWithInvalidChannel) {
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"activate\":{\"kind\":\"note\",\"channel\":0,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = RigFile::Trigger::FromJson(json);

	EXPECT_FALSE(trig.has_value());
}

TEST(RigFile, RejectsMidiTriggerBindingWithChannelAboveSixteen) {
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"activate\":{\"kind\":\"note\",\"channel\":17,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = RigFile::Trigger::FromJson(json);

	EXPECT_FALSE(trig.has_value());
}

TEST(RigFile, RejectsMidiTriggerBindingWithOutOfRangeId) {
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":128},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = RigFile::Trigger::FromJson(json);

	EXPECT_FALSE(trig.has_value());
}

TEST(RigFile, ParsesFile) {
	std::string audio = "{\"name\":\"HDMI\",\"bufsize\":255,\"inlatency\":414,\"outlatency\":414,\"numchannelsin\":0,\"numchannelsout\":10}";
	
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

TEST(RigFile, ParsesFileWithMidiTriggerBinding) {
	std::string audio = "{\"name\":\"HDMI\",\"bufsize\":255,\"inlatency\":414,\"outlatency\":414,\"numchannelsin\":0,\"numchannelsout\":10}";
	std::string midi = "{\"devices\":[{\"name\":\"TriggerPad\",\"enabled\":true},{\"name\":\"Keys A\",\"enabled\":true}]}";
	std::string midiTrigger = "{\"name\":\"trigMidi\",\"stationtype\":0,\"midiinput\":[1],\"midiinputdevices\":[\"TriggerPad\",\"Keys A\"],\"trigger\":{\"type\":\"midi\",\"device\":\"TriggerPad\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":48},\"ditch\":{\"kind\":\"note\",\"channel\":1,\"id\":49}}}";
	auto str = "{\"name\":\"rig\",\"user\":{\"audio\":" + audio + ",\"midi\":" + midi + "},\"triggers\":[" + midiTrigger + "]}";
	auto testStream = std::stringstream(str);
	auto rig = RigFile::FromStream(std::move(testStream));

	ASSERT_TRUE(rig.has_value());
	ASSERT_EQ(1u, rig.value().Triggers.size());
	ASSERT_TRUE(rig.value().Triggers[0].MidiTrigger.has_value());
	ASSERT_EQ(2u, rig.value().Triggers[0].MidiInputDevices.size());
	EXPECT_EQ(0, rig.value().Triggers[0].MidiInputDevices[0].compare("TriggerPad"));
	EXPECT_EQ(0, rig.value().Triggers[0].MidiInputDevices[1].compare("Keys A"));
	EXPECT_EQ(0, rig.value().Triggers[0].MidiTrigger->Device.compare("TriggerPad"));
	EXPECT_EQ(48u, rig.value().Triggers[0].MidiTrigger->Activate.Id);
	EXPECT_EQ(49u, rig.value().Triggers[0].MidiTrigger->Ditch.Id);
}