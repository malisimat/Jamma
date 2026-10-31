#include "gtest/gtest.h"
#include "io/Json.h"
#include "io/RigFile.h"
#include <sstream>
#include <limits>

using io::Json;
using io::RigFile;

class RigFileTest
{
public:
	static std::string Trigger(const std::string& activate = "[]", const std::string& ditch = "[]",
		const std::string& target = "null", const std::string& capture = R"({"audio":[],"midi":[]})")
	{
		return R"({"id":"trigger-guid","name":"trig","target":)" + target +
			R"(,"controls":{"activate":)" + activate + R"(,"ditch":)" + ditch + R"(},"capture":)" + capture + "}";
	}
	static std::optional<RigFile> Read(const std::string& triggers,
		const std::string& midi = R"([{"id":"TriggerPad","name":"TriggerPad","enabled":true},{"id":"Keys A","name":"Keys A","enabled":true},{"id":"Keys B","name":"Keys B","enabled":true}])",
		const std::string& audio = R"({"name":"HDMI","samplerate":44100,"bufsize":255,"inlatency":414,"outlatency":414,"numbuffers":2,"numchannelsin":0,"numchannelsout":10})")
	{
		const auto json = R"({"version":"2.0","name":"rig","devices":{"audio":)" + audio +
			R"(,"midi":)" + midi + R"(,"serial":[{"id":"pedal-a","port":"COM3","baudrate":115200,"enabled":true}]},"settings":{"loop":{"fadeSamps":800,"seedGrainMinMs":400,"seedGrainTargetMaxMs":3000,"seedBpmMin":80,"seedQuantisation":"power"},"trigger":{"debounceMs":280},"midi":{"channelOverrideTriggers":false,"channelOverrideLive":true}},"triggers":[)" + triggers + "]}";
		return RigFile::FromStream(std::stringstream(json));
	}
	static std::string Keyboard(unsigned int press, unsigned int release)
	{
		return R"({"source":"keyboard","device":"keyboard","press":{"code":)" + std::to_string(press) +
			R"(,"state":1},"release":{"code":)" + std::to_string(release) + R"(,"state":0}})";
	}
	static std::string Midi(const std::string& kind, unsigned int id, const std::string& channel)
	{
		return R"({"source":"midi","device":"TriggerPad","button":{"kind":")" + kind + R"(","id":)" + std::to_string(id) + R"(,"channel":)" + channel + "}}";
	}
};

TEST(RigFile, StrictParserRejectsMalformedUnsupportedAndDuplicateFields)
{
	for (const auto& json : {std::string{}, std::string("{}"),
		RigFile::DefaultJson + "garbage", RigFile::DefaultJson + "{}"})
	{
		RigFile::Diagnostics diagnostics;
		EXPECT_FALSE(RigFile::FromStream(std::stringstream(json), &diagnostics));
		EXPECT_FALSE(diagnostics.empty());
	}
	for (const auto& version : {"1.0", "2.1", "3.0"})
	{
		auto json = RigFile::DefaultJson;
		const auto offset = json.find("2.0");
		ASSERT_NE(std::string::npos, offset);
		json.replace(offset, 3u, version);
		EXPECT_FALSE(RigFile::FromStream(std::stringstream(json)));
	}
	auto duplicate = RigFile::DefaultJson;
	duplicate.insert(duplicate.find('{') + 1u, R"("version":"2.0",)");
	EXPECT_FALSE(RigFile::FromStream(std::stringstream(duplicate)));
}

TEST(RigFile, CompiledCapacityCountsAnyChannelExpansionAndRetainsPairedEdges)
{
	std::string bindings;
	for (unsigned int note = 0u; note < 4u; ++note)
	{
		if (!bindings.empty()) bindings += ",";
		bindings += RigFileTest::Midi("note", note, "null");
	}
	const auto full = RigFileTest::Read(RigFileTest::Trigger("[" + bindings + "]"));
	ASSERT_TRUE(full);
	ASSERT_EQ(4u, full->Triggers[0].ControlBindings.Activate.size());
	for (const auto& binding : full->Triggers[0].ControlBindings.Activate)
	{
		EXPECT_FALSE(binding.Press.Channel);
		EXPECT_EQ(binding.Press.Code, binding.Release.Code);
		EXPECT_EQ(0u, binding.Release.State);
	}
	bindings += "," + RigFileTest::Midi("note", 4u, "1");
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger("[" + bindings + "]")));
}

TEST(RigFile, DitchFirstOverlapRemovesWholeLaterBindingAndRoundTripsPriority)
{
	auto trigger = RigFileTest::Trigger("[" + RigFileTest::Keyboard(1u, 11u) + "]",
		"[" + RigFileTest::Keyboard(2u, 11u) + "]");
	const auto controls = trigger.find(R"("controls":)");
	const auto capture = trigger.find(R"(,"capture":)");
	ASSERT_NE(std::string::npos, controls); ASSERT_NE(std::string::npos, capture);
	trigger.replace(controls, capture - controls,
		R"("controls":{"ditch":[)" + RigFileTest::Keyboard(2u, 11u) +
		R"(],"activate":[)" + RigFileTest::Keyboard(1u, 11u) + "]}");
	const auto rig = RigFileTest::Read(trigger);
	ASSERT_TRUE(rig);
	EXPECT_EQ(RigFile::Control::Ditch, rig->Triggers[0].ControlBindings.First);
	EXPECT_TRUE(rig->Triggers[0].ControlBindings.Activate.empty());
	ASSERT_EQ(1u, rig->Triggers[0].ControlBindings.Ditch.size());
	EXPECT_EQ(11u, rig->Triggers[0].ControlBindings.Ditch[0].Release.Code);
	std::stringstream serialized;
	ASSERT_TRUE(RigFile::ToJsonStream(*rig, serialized));
	const auto reloaded = RigFile::FromStream(std::move(serialized));
	ASSERT_TRUE(reloaded); EXPECT_EQ(*rig, *reloaded);
}

TEST(RigFile, RegistryRejectsHardwareNameReferencesAndCrossSourceIds)
{
	const auto devices = R"([{"id":"keys","name":"Hardware keys 0","enabled":true}])";
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger("[]", "[]", "null",
		R"({"audio":[],"midi":["Hardware keys 0"]})"), devices));
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger("[]", "[]", "null",
		R"({"audio":[],"midi":["pedal-a"]})"), devices));
	EXPECT_TRUE(RigFileTest::Read(RigFileTest::Trigger("[]", "[]", "null",
		R"({"audio":[],"midi":["keys"]})"), devices));
}

TEST(RigFile, ParsesAudioSettings) {
	const auto rig = RigFileTest::Read(RigFileTest::Trigger(), "[]",
		R"({"name":"Soundblaster","samplerate":48000,"bufsize":12,"inlatency":212,"outlatency":212,"numbuffers":2,"numchannelsin":6,"numchannelsout":8})");
	ASSERT_TRUE(rig);
	EXPECT_EQ("Soundblaster", rig->User.Audio.Name);
	EXPECT_EQ(12u, rig->User.Audio.BufSize);
	EXPECT_EQ(212u, rig->User.Audio.LatencyIn);
	EXPECT_EQ(212u, rig->User.Audio.LatencyOut);
	EXPECT_EQ(6u, rig->User.Audio.NumChannelsIn);
	EXPECT_EQ(8u, rig->User.Audio.NumChannelsOut);
}

TEST(RigFile, PreservesInputOnlyAudioAndExplicitTriggerTimingUnits)
{
	const auto rig = RigFileTest::Read(RigFileTest::Trigger(), "[]",
		R"({"name":"Input only","samplerate":48000,"bufsize":128,"inlatency":0,"outlatency":0,"numbuffers":2,"numchannelsin":2,"numchannelsout":0})");
	ASSERT_TRUE(rig);
	EXPECT_EQ(2u, rig->User.Audio.NumChannelsIn); EXPECT_EQ(0u, rig->User.Audio.NumChannelsOut);
	EXPECT_EQ(0u, rig->User.Trigger.PreDelaySamps); EXPECT_EQ(280u, rig->User.Trigger.DebounceMs);
	std::stringstream serialized; ASSERT_TRUE(RigFile::ToJsonStream(*rig, serialized));
	const auto reloaded = RigFile::FromStream(std::move(serialized));
	ASSERT_TRUE(reloaded); EXPECT_EQ(*rig, *reloaded);
}

TEST(RigFile, ParsesIndependentPressReleaseBindings) {
	const auto rig = RigFileTest::Read(RigFileTest::Trigger("[" + RigFileTest::Keyboard(1u, 11u) + "]", "[" + RigFileTest::Keyboard(2u, 12u) + "]"));
	ASSERT_TRUE(rig);
	const auto& controls = rig->Triggers[0].ControlBindings;
	ASSERT_EQ(1u, controls.Activate.size()); ASSERT_EQ(1u, controls.Ditch.size());
	EXPECT_EQ(1u, controls.Activate[0].Press.Code); EXPECT_EQ(11u, controls.Activate[0].Release.Code);
	EXPECT_EQ(2u, controls.Ditch[0].Press.Code); EXPECT_EQ(12u, controls.Ditch[0].Release.Code);
}

TEST(RigFile, ParsesSerialBindingSource) {
	const auto rig = RigFileTest::Read(RigFileTest::Trigger(
		R"([{"source":"serial","device":"pedal-a","button":0}])", R"([{"source":"serial","device":"pedal-a","button":1}])"));
	ASSERT_TRUE(rig);
	const auto& activate = rig->Triggers[0].ControlBindings.Activate[0];
	EXPECT_EQ(RigFile::BindingSource::Serial, activate.Source);
	EXPECT_EQ("pedal-a", activate.Device);
	EXPECT_EQ(0u, activate.Press.Code); EXPECT_EQ(0u, activate.Release.Code);
	EXPECT_EQ(1u, activate.Press.State); EXPECT_EQ(0u, activate.Release.State);
	EXPECT_EQ(1u, rig->Triggers[0].ControlBindings.Ditch[0].Press.Code);
}

TEST(RigFile, RejectsSerialBindingEmptyDevice) {
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger(R"([{"source":"serial","device":"","button":0}])")));
}

TEST(RigFile, ParsesTrigger) {
	const auto rig = RigFileTest::Read(RigFileTest::Trigger(
		"[" + RigFileTest::Keyboard(1u, 11u) + "," + RigFileTest::Keyboard(3u, 13u) + "," + RigFileTest::Keyboard(5u, 15u) + "]",
		"[" + RigFileTest::Keyboard(2u, 12u) + "," + RigFileTest::Keyboard(4u, 14u) + "," + RigFileTest::Keyboard(6u, 16u) + "]"));
	ASSERT_TRUE(rig);
	const auto& trigger = rig->Triggers[0];
	EXPECT_EQ("trig", trigger.Name);
	ASSERT_EQ(3u, trigger.ControlBindings.Activate.size()); ASSERT_EQ(3u, trigger.ControlBindings.Ditch.size());
	EXPECT_EQ(1u, trigger.ControlBindings.Activate[0].Press.Code); EXPECT_EQ(11u, trigger.ControlBindings.Activate[0].Release.Code);
	EXPECT_EQ(2u, trigger.ControlBindings.Ditch[0].Press.Code); EXPECT_EQ(12u, trigger.ControlBindings.Ditch[0].Release.Code);
	EXPECT_EQ(3u, trigger.ControlBindings.Activate[1].Press.Code); EXPECT_EQ(4u, trigger.ControlBindings.Ditch[1].Press.Code);
	EXPECT_EQ(5u, trigger.ControlBindings.Activate[2].Press.Code); EXPECT_EQ(6u, trigger.ControlBindings.Ditch[2].Press.Code);
}

TEST(RigFile, ParsesStableTriggerIdAndRejectsMissingId) {
	const auto identified = RigFileTest::Read(RigFileTest::Trigger());
	ASSERT_TRUE(identified); EXPECT_EQ("trigger-guid", identified->Triggers[0].Id);
	auto missing = RigFileTest::Trigger(); missing.erase(missing.find(R"("id":"trigger-guid",)"), std::string(R"("id":"trigger-guid",)").size());
	EXPECT_FALSE(RigFileTest::Read(missing));
	auto invalid = RigFileTest::Trigger(); invalid.replace(invalid.find(R"("trigger-guid")"), std::string(R"("trigger-guid")").size(), "12");
	EXPECT_FALSE(RigFileTest::Read(invalid));
}

TEST(RigFile, ParsesMidiInputDevicesAndRemovesDuplicates) {
	const auto rig = RigFileTest::Read(RigFileTest::Trigger("[]", "[]", "null", R"({"audio":[],"midi":["Keys A","Keys B","Keys A"]})"));
	ASSERT_TRUE(rig);
	EXPECT_EQ((std::vector<std::string>{"Keys A", "Keys B"}), rig->Triggers[0].CaptureInputs.Midi);
}

TEST(RigFile, ParsesMidiTriggerBinding) {
	const auto rig = RigFileTest::Read(RigFileTest::Trigger("[" + RigFileTest::Midi("note", 60u, "10") + "]", "[" + RigFileTest::Midi("cc", 64u, "1") + "]"));
	ASSERT_TRUE(rig);
	const auto& controls = rig->Triggers[0].ControlBindings;
	ASSERT_EQ(1u, controls.Activate.size()); ASSERT_EQ(1u, controls.Ditch.size());
	EXPECT_EQ("TriggerPad", controls.Activate[0].Device);
	EXPECT_EQ(RigFile::NOTE, controls.Activate[0].Press.Kind); EXPECT_EQ(9u, controls.Activate[0].Press.Channel);
	EXPECT_EQ(60u, controls.Activate[0].Press.Code); EXPECT_EQ(1u, controls.Activate[0].Press.State);
	EXPECT_EQ(RigFile::CC, controls.Ditch[0].Press.Kind); EXPECT_EQ(0u, controls.Ditch[0].Press.Channel);
	EXPECT_EQ(64u, controls.Ditch[0].Press.Code); EXPECT_EQ(1u, controls.Ditch[0].Press.State);
}

TEST(RigFile, ParsesNoteAndInverseNoteOffMidiBindingKinds) {
	const auto rig = RigFileTest::Read(RigFileTest::Trigger("[" + RigFileTest::Midi("note", 60u, "1") + "]", "[" + RigFileTest::Midi("noteoff", 61u, "1") + "]"));
	ASSERT_TRUE(rig);
	const auto& controls = rig->Triggers[0].ControlBindings;
	EXPECT_EQ(RigFile::NOTE, controls.Activate[0].Press.Kind); EXPECT_EQ(1u, controls.Activate[0].Press.State);
	EXPECT_EQ(RigFile::NOTE, controls.Ditch[0].Press.Kind); EXPECT_EQ(0u, controls.Ditch[0].Press.State);
	EXPECT_EQ(1u, controls.Ditch[0].Release.State);
}

TEST(RigFile, RejectsMidiTriggerBindingWithInvalidChannel) {
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger("[" + RigFileTest::Midi("note", 60u, "0") + "]")));
}

TEST(RigFile, RejectsMidiTriggerBindingWithChannelAboveSixteen) {
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger("[" + RigFileTest::Midi("note", 60u, "17") + "]")));
}

TEST(RigFile, RejectsMidiTriggerBindingWithOutOfRangeId) {
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger("[" + RigFileTest::Midi("note", 128u, "1") + "]")));
}

TEST(RigFile, ParsesFile) {
	auto first = RigFileTest::Trigger("[" + RigFileTest::Keyboard(1u, 11u) + "," + RigFileTest::Keyboard(3u, 13u) + "]", "[" + RigFileTest::Keyboard(2u, 12u) + "," + RigFileTest::Keyboard(4u, 14u) + "]");
	auto second = RigFileTest::Trigger("[" + RigFileTest::Keyboard(5u, 15u) + "]", "[" + RigFileTest::Keyboard(6u, 16u) + "]");
	second.replace(second.find("trigger-guid"), std::string("trigger-guid").size(), "second-id");
	second.replace(second.find(R"("name":"trig")"), std::string(R"("name":"trig")").size(), R"("name":"second")");
	const auto rig = RigFileTest::Read(first + "," + second);
	ASSERT_TRUE(rig); EXPECT_EQ(RigFile::CurrentVersion, rig->Version); EXPECT_EQ("rig", rig->Name);
	EXPECT_EQ("HDMI", rig->User.Audio.Name); EXPECT_EQ(255u, rig->User.Audio.BufSize);
	EXPECT_EQ(414u, rig->User.Audio.LatencyIn); EXPECT_EQ(414u, rig->User.Audio.LatencyOut);
	EXPECT_EQ(0u, rig->User.Audio.NumChannelsIn); EXPECT_EQ(10u, rig->User.Audio.NumChannelsOut);
	ASSERT_EQ(2u, rig->Triggers.size());
	ASSERT_EQ(2u, rig->Triggers[0].ControlBindings.Activate.size()); ASSERT_EQ(1u, rig->Triggers[1].ControlBindings.Activate.size());
	EXPECT_EQ(3u, rig->Triggers[0].ControlBindings.Activate[1].Press.Code);
	EXPECT_EQ(5u, rig->Triggers[1].ControlBindings.Activate[0].Press.Code); EXPECT_EQ(6u, rig->Triggers[1].ControlBindings.Ditch[0].Press.Code);
}

TEST(RigFile, ParsesFileWithMidiTriggerBinding) {
	const auto rig = RigFileTest::Read(RigFileTest::Trigger("[" + RigFileTest::Midi("note", 48u, "1") + "]", "[" + RigFileTest::Midi("note", 49u, "1") + "]", "null", R"({"audio":[],"midi":["TriggerPad","Keys A"]})"));
	ASSERT_TRUE(rig); ASSERT_EQ(1u, rig->Triggers.size());
	EXPECT_EQ((std::vector<std::string>{"TriggerPad", "Keys A"}), rig->Triggers[0].CaptureInputs.Midi);
	EXPECT_EQ("TriggerPad", rig->Triggers[0].ControlBindings.Activate[0].Device);
	EXPECT_EQ(48u, rig->Triggers[0].ControlBindings.Activate[0].Press.Code);
	EXPECT_EQ(49u, rig->Triggers[0].ControlBindings.Ditch[0].Press.Code);
}

TEST(RigFile, ParsesExplicitNullAndNamedTargetsAndRejectsAbsentOrEmptyTarget) {
	const auto unbound = RigFileTest::Read(RigFileTest::Trigger());
	ASSERT_TRUE(unbound); EXPECT_FALSE(unbound->Triggers[0].StationTarget);
	const auto named = RigFileTest::Read(RigFileTest::Trigger("[]", "[]", R"("Drums")"));
	ASSERT_TRUE(named); EXPECT_EQ("Drums", named->Triggers[0].StationTarget);
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger("[]", "[]", R"("")")));
	auto absent = RigFileTest::Trigger(); absent.erase(absent.find(R"("target":null,)"), std::string(R"("target":null,)").size());
	EXPECT_FALSE(RigFileTest::Read(absent));
}

TEST(RigFile, ParsesOnlyExplicitMidiDeviceSelections) {
	const auto none = RigFileTest::Read(RigFileTest::Trigger()); ASSERT_TRUE(none); EXPECT_TRUE(none->Triggers[0].CaptureInputs.Midi.empty());
	const auto selected = RigFileTest::Read(RigFileTest::Trigger("[]", "[]", "null", R"({"audio":[],"midi":["Keys A","Keys A"]})"));
	ASSERT_TRUE(selected); EXPECT_EQ((std::vector<std::string>{"Keys A"}), selected->Triggers[0].CaptureInputs.Midi);
	EXPECT_FALSE(RigFileTest::Read(RigFileTest::Trigger("[]", "[]", "null", R"({"audio":[],"midi":["*"]})")));
	EXPECT_FALSE(RigFileTest::Read(R"({"name":"legacy","midiinputmode":"any"})"));
}

TEST(RigFile, JsonSerializerRoundTripsEveryKnownSectionAndRejectsUnknownFields) {
	auto json = RigFile::DefaultJson; json.insert(json.size() - 1u, R"(,"unknown":42)");
	EXPECT_FALSE(RigFile::FromStream(std::stringstream(json)));
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)); ASSERT_TRUE(rig);
	rig->Triggers[0].Id = "stable-trigger-id"; rig->Triggers[0].StationTarget = "Station \"A\"";
	rig->User.Midi.Devices = {{"Keys\\One", true, "keys"}, {"Pad", true, "pad"}};
	rig->Triggers[0].CaptureInputs.Midi = {"keys"};
	RigFile::Binding activate; activate.Source = RigFile::BindingSource::Midi; activate.Device = "pad";
	activate.Press = {60u, 1u, RigFile::NOTE, 2u}; activate.Release = {60u, 0u, RigFile::NOTE, 2u};
	RigFile::Binding ditch = activate; ditch.Press = {64u, 1u, RigFile::CC, std::nullopt}; ditch.Release = {64u, 0u, RigFile::CC, std::nullopt};
	rig->Triggers[0].ControlBindings = {{activate}, {ditch}, RigFile::Control::Ditch};
	rig->User.Serial.Devices.push_back({"pedal", "COM9", 57600u, true});
	std::stringstream out; ASSERT_TRUE(RigFile::ToJsonStream(*rig, out));
	const auto reparsed = RigFile::FromStream(std::move(out)); ASSERT_TRUE(reparsed);
	EXPECT_EQ(rig->Name, reparsed->Name); EXPECT_EQ(rig->User, reparsed->User); EXPECT_EQ(rig->Triggers, reparsed->Triggers);
}

TEST(RigFileRouting, ResolvesNamesSafelyWithoutPositionalTargetMigration) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.Triggers.resize(4, rig.Triggers[0]);
	for (size_t i = 0u; i < rig.Triggers.size(); ++i) { rig.Triggers[i].Id = "trigger-" + std::to_string(i); rig.Triggers[i].Name = "trigger-" + std::to_string(i); }
	rig.Triggers[0].StationTarget.reset(); rig.Triggers[1].StationTarget = "Bass";
	rig.Triggers[2].StationTarget = "Dup"; rig.Triggers[3].StationTarget.reset();
	std::vector<io::JamFile::Station> stations(3);
	stations[0].Name = "Drums"; stations[1].Name = "Dup"; stations[2].Name = "Dup";
	const auto result = io::RigFileRouting::Resolve(rig, stations, 2u, {});
	ASSERT_TRUE(result.IsValid); EXPECT_FALSE(result.RequiresSave);
	EXPECT_FALSE(result.CandidateRig.Triggers[0].StationTarget); EXPECT_FALSE(result.Triggers[0].StationIndex);
	EXPECT_EQ(io::RigFileRouting::Warning::None, result.Triggers[0].Reason);
	EXPECT_EQ(io::RigFileRouting::Warning::TargetMissing, result.Triggers[1].Reason);
	EXPECT_EQ(io::RigFileRouting::Warning::TargetAmbiguous, result.Triggers[2].Reason);
	EXPECT_FALSE(result.Triggers[3].StationIndex); EXPECT_FALSE(result.CandidateRig.Triggers[3].StationTarget);
}

TEST(RigFileRouting, RejectsMissingIdsWithoutChangingTargets) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.Triggers.resize(2, rig.Triggers[0]);
	rig.Triggers[0].Id.clear(); rig.Triggers[0].StationTarget.reset();
	rig.Triggers[1].Id = "existing-id"; rig.Triggers[1].Name = "second"; rig.Triggers[1].StationTarget = "Bass";
	const auto result = io::RigFileRouting::Resolve(rig, {{"Drums"}, {"Bass"}}, 2u, {});
	EXPECT_FALSE(result.IsValid); EXPECT_FALSE(result.RequiresSave);
	EXPECT_TRUE(result.CandidateRig.Triggers[0].Id.empty()); EXPECT_FALSE(result.CandidateRig.Triggers[0].StationTarget);
	EXPECT_EQ("existing-id", result.CandidateRig.Triggers[1].Id);
}

TEST(RigFileRouting, RejectsDuplicateNonEmptyTriggerIds) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.Triggers.resize(2, rig.Triggers[0]); rig.Triggers[1].Name = "second";
	for (auto& trigger : rig.Triggers) trigger.Id = "duplicate-id";
	EXPECT_FALSE(io::RigFileRouting::Resolve(rig, {}, 2u, {}).IsValid);
}

TEST(RigFileRouting, SupportsManyToOneAndReportsUnavailableSources) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.Triggers.push_back(rig.Triggers[0]); rig.Triggers[1].Id = "second-id"; rig.Triggers[1].Name = "second";
	for (auto& trigger : rig.Triggers) trigger.StationTarget = "One";
	rig.User.Midi.Devices = {{"Present hardware", true, "Present"}, {"Missing hardware", true, "Missing"}};
	rig.Triggers[0].CaptureInputs.Audio = {0u, 5u}; rig.Triggers[0].CaptureInputs.Midi = {"Present", "Missing"};
	const auto result = io::RigFileRouting::Resolve(rig, {{"One"}}, 2u, {"Present"});
	ASSERT_TRUE(result.IsValid); ASSERT_EQ(2u, result.Triggers.size());
	EXPECT_EQ(0u, result.Triggers[0].StationIndex); EXPECT_EQ(0u, result.Triggers[1].StationIndex);
	ASSERT_EQ(4u, result.Triggers[0].Sources.size());
	EXPECT_TRUE(result.Triggers[0].Sources[0].Available); EXPECT_FALSE(result.Triggers[0].Sources[1].Available);
	EXPECT_TRUE(result.Triggers[0].Sources[2].Available); EXPECT_FALSE(result.Triggers[0].Sources[3].Available);
}

TEST(RigFileRouting, RetainsExactHardwareNamesAndResolvesAvailabilityByRegistryId) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.User.Midi.Devices = {{"CASIO USB-MIDI 0", true, "casio"}};
	rig.Triggers[0].StationTarget = "Station"; rig.Triggers[0].CaptureInputs.Midi = {"casio"};
	const auto unavailable = io::RigFileRouting::Resolve(rig, {{"Station"}}, 2u, {"CASIO USB-MIDI"});
	ASSERT_TRUE(unavailable.IsValid); EXPECT_FALSE(unavailable.Triggers[0].Sources.back().Available);
	const auto available = io::RigFileRouting::Resolve(rig, {{"Station"}}, 2u, {"casio"});
	ASSERT_TRUE(available.IsValid); EXPECT_FALSE(available.RequiresSave);
	EXPECT_EQ("CASIO USB-MIDI 0", available.CandidateRig.User.Midi.Devices[0].Name);
	EXPECT_EQ("casio", available.CandidateRig.Triggers[0].CaptureInputs.Midi[0]);
	EXPECT_TRUE(available.Triggers[0].Sources.back().Available);
}

TEST(RigFileRouting, MutationHelpersArePureAndRejectDuplicateCaptureRoutes) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value();
	rig.Triggers.clear(); rig.User.Midi.Devices = {{"Keys hardware", true, "Keys"}};
	auto first = io::RigFileRouting::WithUnboundTrigger(rig);
	ASSERT_TRUE(rig.Triggers.empty()); ASSERT_EQ(1u, first.Triggers.size()); EXPECT_EQ("Trigger-1", first.Triggers[0].Name);
	EXPECT_FALSE(first.Triggers[0].Id.empty());
	first.Triggers.push_back(first.Triggers[0]); first.Triggers[1].Name = "Trigger-3"; first.Triggers[1].Id = "existing-id";
	const auto second = io::RigFileRouting::WithUnboundTrigger(first);
	ASSERT_EQ(3u, second.Triggers.size()); EXPECT_EQ("Trigger-2", second.Triggers.back().Name);
	EXPECT_FALSE(second.Triggers.back().Id.empty()); EXPECT_NE(first.Triggers[0].Id, second.Triggers.back().Id); EXPECT_NE("existing-id", second.Triggers.back().Id);
	const auto adc = io::RigFileRouting::WithAdcInput(second, 0u, 7u); ASSERT_TRUE(adc);
	EXPECT_EQ(second.Triggers[0].Id, adc->Triggers[0].Id); EXPECT_FALSE(io::RigFileRouting::WithAdcInput(*adc, 0u, 7u));
	const auto midi = io::RigFileRouting::WithMidiInput(*adc, 0u, "Keys"); ASSERT_TRUE(midi);
	EXPECT_EQ(adc->Triggers[0].Id, midi->Triggers[0].Id); EXPECT_FALSE(io::RigFileRouting::WithMidiInput(*midi, 0u, "Keys"));
	EXPECT_FALSE(io::RigFileRouting::WithMidiInput(*midi, 0u, "*"));
	const auto removed = io::RigFileRouting::WithoutMidiInput(*midi, 0u, "Keys"); ASSERT_TRUE(removed); EXPECT_TRUE(removed->Triggers[0].CaptureInputs.Midi.empty());
	const auto targeted = io::RigFileRouting::WithStationTarget(*removed, 0u, "Station"); ASSERT_TRUE(targeted);
	EXPECT_EQ(removed->Triggers[0].Id, targeted->Triggers[0].Id);
	const auto withoutFirst = io::RigFileRouting::WithoutTrigger(*targeted, 0u); ASSERT_TRUE(withoutFirst);
	ASSERT_EQ(2u, withoutFirst->Triggers.size()); EXPECT_EQ("existing-id", withoutFirst->Triggers[0].Id);
}

TEST(RigFileRouting, LifecycleAddIsUnboundAndDeleteRemovesTheWholeTriggerRouteSet) {
	auto rig = RigFile::FromStream(std::stringstream(RigFile::DefaultJson)).value(); rig.Triggers.clear();
	auto added = io::RigFileRouting::WithUnboundTrigger(rig);
	ASSERT_EQ(1u, added.Triggers.size()); EXPECT_FALSE(added.Triggers[0].Id.empty()); EXPECT_EQ("Trigger-1", added.Triggers[0].Name);
	EXPECT_FALSE(added.Triggers[0].StationTarget);
	EXPECT_TRUE(added.Triggers[0].CaptureInputs.Audio.empty()); EXPECT_TRUE(added.Triggers[0].CaptureInputs.Midi.empty());
	EXPECT_TRUE(added.Triggers[0].ControlBindings.Activate.empty()); EXPECT_TRUE(added.Triggers[0].ControlBindings.Ditch.empty());
	added.User.Midi.Devices = {{"Keys hardware", true, "Keys"}};
	added.Triggers[0].StationTarget = "Station A"; added.Triggers[0].CaptureInputs.Audio = {1u, 2u}; added.Triggers[0].CaptureInputs.Midi = {"Keys"};
	const auto deleted = io::RigFileRouting::WithoutTrigger(added, 0u); ASSERT_TRUE(deleted);
	EXPECT_TRUE(deleted->Triggers.empty()); EXPECT_EQ(1u, added.Triggers.size());
}

TEST(RigFile, TriggerPredelayDefaultsToZeroAndRoundTripsFractionalSignedMilliseconds)
{
	auto rig = RigFileTest::Read(RigFileTest::Trigger());
	ASSERT_TRUE(rig);
	ASSERT_EQ(1u, rig->Triggers.size());
	EXPECT_DOUBLE_EQ(0.0, rig->Triggers.front().PreDelayMs);
	for (const double milliseconds : {0.0, 86.0, -86.0, 0.123456789012345, -200.0, 200.0})
	{
		rig->Triggers.front().PreDelayMs = milliseconds;
		std::stringstream encoded;
		ASSERT_TRUE(RigFile::ToJsonStream(*rig, encoded));
		EXPECT_EQ(std::string::npos, encoded.str().find("preDelaySamps"));
		const auto restored = RigFile::FromStream(std::move(encoded));
		ASSERT_TRUE(restored);
		EXPECT_DOUBLE_EQ(milliseconds, restored->Triggers.front().PreDelayMs);
	}
}

TEST(RigFile, RejectsOutOfRangeAndWrongTypeTriggerPredelay)
{
	for (const auto& milliseconds : {"200.1", "-200.1", "true", "null", "\"86\""})
	{
		auto trigger = RigFileTest::Trigger();
		trigger.insert(1u, std::string("\"preDelayMs\":") + milliseconds + ",");
		EXPECT_FALSE(RigFileTest::Read(trigger)) << milliseconds;
	}
}

TEST(RigFile, MigratesLegacyZeroAndDiagnosesNonzeroOrConflictingGlobalPredelay)
{
	for (const auto value : {0u, 1u})
	{
		auto json = RigFile::DefaultJson;
		const auto triggerSettings = json.find("\"debounceMs\"");
		ASSERT_NE(std::string::npos, triggerSettings);
		json.insert(triggerSettings, "\"preDelaySamps\":" + std::to_string(value) + ",");
		RigFile::Diagnostics diagnostics;
		auto parsed = RigFile::FromStream(std::stringstream(json), &diagnostics);
		EXPECT_EQ(value == 0u, parsed.has_value());
		ASSERT_FALSE(diagnostics.empty());
		EXPECT_EQ("$.settings.trigger.preDelaySamps", diagnostics.front().Path);
		if (value == 0u)
		{
			const auto triggerId = json.find("\"id\"", json.find("\"triggers\""));
			ASSERT_NE(std::string::npos, triggerId);
			json.insert(triggerId, "\"preDelayMs\":0,");
			EXPECT_FALSE(RigFile::FromStream(std::stringstream(json)));
		}
	}
}

TEST(RigFile, MidiSeamWindowDefaultsValidatesAndRoundTrips)
{
	auto rig = RigFileTest::Read(RigFileTest::Trigger());
	ASSERT_TRUE(rig); EXPECT_EQ(200u,rig->User.Loop.MidiSeamMergeWindowMs);
	for (const auto ms : {0u,30u,200u,500u,(std::numeric_limits<unsigned int>::max)()})
	{
		rig->User.Loop.MidiSeamMergeWindowMs=ms;
		std::stringstream saved; ASSERT_TRUE(RigFile::ToJsonStream(*rig,saved));
		const auto restored=RigFile::FromStream(std::move(saved));
		ASSERT_TRUE(restored); EXPECT_EQ(ms,restored->User.Loop.MidiSeamMergeWindowMs);
	}
	for (const auto value : {"-1","4294967296","30.5","true","null","\"30\""})
	{
		auto json=RigFile::DefaultJson;
		const auto at=json.find("\"midiSeamMergeWindowMs\":200"); ASSERT_NE(std::string::npos,at);
		json.replace(at,std::string("\"midiSeamMergeWindowMs\":200").size(),std::string("\"midiSeamMergeWindowMs\":")+value);
		EXPECT_FALSE(RigFile::FromStream(std::stringstream(json))) << value;
	}
}
