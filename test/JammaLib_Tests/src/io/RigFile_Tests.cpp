
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

TEST(RigFile, ParsesMidiInputDevicesAndRemovesDuplicates) {
	auto pair = std::regex_replace(std::regex_replace(TriggerPairString, std::regex("%ADOWN%"), "51"), std::regex("%DDOWN%"), "52");
	auto str = "{\"name\":\"Trig2\",\"stationtype\":0,\"pairs\":[" + pair + "],\"midiinputdevices\":[\"Keys A\",\"Keys B\",\"Keys A\"]}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = RigFile::Trigger::FromJson(json);

	ASSERT_TRUE(trig.has_value());
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
	std::string midiTrigger = "{\"name\":\"trigMidi\",\"stationtype\":0,\"midiinputdevices\":[\"TriggerPad\",\"Keys A\"],\"trigger\":{\"type\":\"midi\",\"device\":\"TriggerPad\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":48},\"ditch\":{\"kind\":\"note\",\"channel\":1,\"id\":49}}}";
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

TEST(RigFile, ParsesStationTargetAbsentEmptyAndName) {
	auto absent = RigFile::Trigger::FromJson(std::get<Json::JsonPart>(Json::FromStream(std::stringstream("{\"name\":\"a\"}")).value()));
	auto empty = RigFile::Trigger::FromJson(std::get<Json::JsonPart>(Json::FromStream(std::stringstream("{\"name\":\"b\",\"stationtarget\":\"\"}")).value()));
	auto named = RigFile::Trigger::FromJson(std::get<Json::JsonPart>(Json::FromStream(std::stringstream("{\"name\":\"c\",\"stationtarget\":\"Drums\"}")).value()));
	ASSERT_TRUE(absent.has_value()); ASSERT_FALSE(absent->StationTarget.has_value());
	ASSERT_TRUE(empty.has_value()); ASSERT_TRUE(empty->StationTarget.has_value()); EXPECT_TRUE(empty->StationTarget->empty());
	ASSERT_TRUE(named.has_value()); ASSERT_EQ("Drums", named->StationTarget.value());
}

TEST(RigFile, ParsesExplicitAndLegacyMidiInputModes) {
	auto parse = [](const char* json) { return RigFile::Trigger::FromJson(std::get<Json::JsonPart>(Json::FromStream(std::stringstream(json)).value())); };
	ASSERT_EQ(RigFile::Trigger::MidiInputMode::LegacyAny, parse("{\"name\":\"legacy\"}")->MidiInputs);
	ASSERT_EQ(RigFile::Trigger::MidiInputMode::None, parse("{\"name\":\"none\",\"midiinputmode\":\"none\"}")->MidiInputs);
	ASSERT_EQ(RigFile::Trigger::MidiInputMode::Any, parse("{\"name\":\"any\",\"midiinputmode\":\"any\"}")->MidiInputs);
	ASSERT_EQ(RigFile::Trigger::MidiInputMode::Selected, parse("{\"name\":\"selected\",\"midiinputmode\":\"selected\",\"midiinputdevices\":[\"Keys\",\"Keys\",\"\"]}")->MidiInputs);
	EXPECT_FALSE(parse("{\"name\":\"bad\",\"midiinputmode\":\"selected\",\"midiinputdevices\":[]}").has_value());
	EXPECT_FALSE(parse("{\"name\":\"bad\",\"midiinputmode\":\"any\",\"midiinputdevices\":[\"Keys\"]}").has_value());
}

TEST(RigFile, JsonSerializerRoundTripsEveryKnownSectionAndDropsUnknownFields) {
	auto json = std::string(RigFile::DefaultJson);
	json.insert(json.size() - 1u, ",\"unknown\":42");
	auto rig = RigFile::FromStream(std::stringstream(json));
	ASSERT_TRUE(rig.has_value());
	rig->Triggers[0].StationTarget = "Station \"A\"";
	rig->Triggers[0].MidiInputs = RigFile::Trigger::MidiInputMode::Selected;
	rig->Triggers[0].MidiInputDevices = { "Keys\\One" };
	RigFile::Trigger::MidiTriggerBinding binding{};
	binding.Device = "Pad";
	binding.Activate = { RigFile::NOTE, 2u, 60u, 1u, false };
	binding.Ditch = { RigFile::CC, 0u, 64u, 1u, true };
	rig->Triggers[0].MidiTrigger = binding;
	rig->User.Serial.Devices.push_back({ "pedal", "COM9", 57600u, true });
	std::stringstream out;
	ASSERT_TRUE(RigFile::ToJsonStream(rig.value(), out));
	EXPECT_EQ(std::string::npos, out.str().find("unknown")); // Parsed models intentionally do not preserve unknown JSON fields.
	auto reparsed = RigFile::FromStream(std::move(out));
	ASSERT_TRUE(reparsed.has_value());
	EXPECT_EQ(rig->Name, reparsed->Name);
	EXPECT_EQ(rig->User.Audio.SampleRate, reparsed->User.Audio.SampleRate);
	ASSERT_EQ(1u, reparsed->User.Serial.Devices.size());
	EXPECT_EQ("COM9", reparsed->User.Serial.Devices[0].Port);
	ASSERT_EQ(1u, reparsed->Triggers.size());
	EXPECT_EQ("Station \"A\"", reparsed->Triggers[0].StationTarget.value());
	EXPECT_EQ("Keys\\One", reparsed->Triggers[0].MidiInputDevices[0]);
	ASSERT_TRUE(reparsed->Triggers[0].MidiTrigger.has_value());
	EXPECT_EQ(60u, reparsed->Triggers[0].MidiTrigger->Activate.Id);
}

TEST(RigFileRouting, ResolvesNamesSafelyAndMigratesOnlyInRangeLegacyTargets) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.Triggers.resize(4, rig.Triggers[0]);
	rig.Triggers[0].Name = "legacy"; rig.Triggers[0].StationTarget.reset();
	rig.Triggers[1].Name = "named"; rig.Triggers[1].StationTarget = "Bass";
	rig.Triggers[2].Name = "ambiguous"; rig.Triggers[2].StationTarget = "Dup";
	rig.Triggers[3].Name = "out"; rig.Triggers[3].StationTarget.reset();
	std::vector<io::JamFile::Station> stations(3);
	stations[0].Name = "Drums"; stations[1].Name = "Dup"; stations[2].Name = "Dup";
	auto result = io::RigFileRouting::Resolve(rig, stations, 2u, {});
	ASSERT_TRUE(result.RequiresSave);
	EXPECT_EQ("Drums", result.CandidateRig.Triggers[0].StationTarget.value());
	EXPECT_EQ(0u, result.Triggers[0].StationIndex.value());
	EXPECT_EQ(io::RigFileRouting::Warning::TargetMissing, result.Triggers[1].Reason);
	EXPECT_EQ(io::RigFileRouting::Warning::TargetAmbiguous, result.Triggers[2].Reason);
	EXPECT_FALSE(result.Triggers[3].StationIndex.has_value());
	EXPECT_FALSE(result.CandidateRig.Triggers[3].StationTarget.has_value());
}

TEST(RigFileRouting, SupportsManyToOneAndReportsUnavailableSources) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.Triggers.push_back(rig.Triggers[0]);
	for (auto& trigger : rig.Triggers) trigger.StationTarget = "One";
	rig.Triggers[0].InputChannels = { 0u, 5u };
	rig.Triggers[0].MidiInputs = RigFile::Trigger::MidiInputMode::Selected;
	rig.Triggers[0].MidiInputDevices = { "Present", "Missing" };
	std::vector<io::JamFile::Station> stations(1); stations[0].Name = "One";
	auto result = io::RigFileRouting::Resolve(rig, stations, 2u, { "Present" });
	ASSERT_EQ(0u, result.Triggers[0].StationIndex.value()); ASSERT_EQ(0u, result.Triggers[1].StationIndex.value());
	ASSERT_EQ(4u, result.Triggers[0].Sources.size());
	EXPECT_TRUE(result.Triggers[0].Sources[0].Available); EXPECT_FALSE(result.Triggers[0].Sources[1].Available);
	EXPECT_TRUE(result.Triggers[0].Sources[2].Available); EXPECT_FALSE(result.Triggers[0].Sources[3].Available);
}

TEST(RigFileRouting, MutationHelpersArePureAndRejectDuplicateCaptureRoutes) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.Triggers.clear();
	auto first = io::RigFileRouting::WithUnboundTrigger(rig);
	ASSERT_TRUE(rig.Triggers.empty()); ASSERT_EQ("Trigger-1", first.Triggers[0].Name);
	first.Triggers.push_back(first.Triggers[0]); first.Triggers[1].Name = "Trigger-3";
	auto second = io::RigFileRouting::WithUnboundTrigger(first);
	EXPECT_EQ("Trigger-2", second.Triggers.back().Name);
	auto adc = io::RigFileRouting::WithAdcInput(second, 0u, 7u); ASSERT_TRUE(adc.has_value());
	EXPECT_FALSE(io::RigFileRouting::WithAdcInput(adc.value(), 0u, 7u).has_value());
	auto midi = io::RigFileRouting::WithMidiInput(adc.value(), 0u, "Keys"); ASSERT_TRUE(midi.has_value());
	EXPECT_FALSE(io::RigFileRouting::WithMidiInput(midi.value(), 0u, "Keys").has_value());
	auto removed = io::RigFileRouting::WithoutMidiInput(midi.value(), 0u, "Keys"); ASSERT_TRUE(removed.has_value());
	EXPECT_EQ(RigFile::Trigger::MidiInputMode::None, removed->Triggers[0].MidiInputs);
}
