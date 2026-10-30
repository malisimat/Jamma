
#include "gtest/gtest.h"
#include <algorithm>
#include <regex>
#include <limits>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "resources/ResourceLib.h"
#include "io/InitFile.h"
#include "io/StartupConfig.h"
#include "io/Json.h"
#include "io/UserConfig.h"

using io::Json;
using io::InitFile;
using io::UserConfig;

class StartupDefaultsTest : public testing::Test
{
protected:
	std::filesystem::path Directory;
	void SetUp() override
	{
		Directory = std::filesystem::temp_directory_path() / ("jamma-startup-defaults-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		ASSERT_TRUE(std::filesystem::create_directory(Directory));
	}
	void TearDown() override { std::filesystem::remove_all(Directory); }
	std::optional<InitFile> Read(const std::string& json)
	{
		const auto path = Directory / "defaults.json";
		{ std::ofstream file(path); file << json; }
		return InitFile::Read(path.wstring());
	}
};

TEST_F(StartupDefaultsTest, MissingOrEmptyPathsPreserveTheOtherSelectionAndPreferences)
{
	for (const auto* absent : { "jam", "rig" })
		for (const bool explicitEmpty : { false, true })
		{
			SCOPED_TRACE(std::string(absent) + (explicitEmpty ? " empty" : " missing"));
			const bool missingJam = std::string(absent) == "jam";
			const auto json = std::string("{\"consoleautostart\":false,\"win\":[22,24,1400,1000],") +
				"\"logging\":{\"event\":\"verbose\"}," +
				(missingJam ? "\"rig\":\"chosen.rig\",\"rigorigin\":\"user\"" :
					"\"jam\":\"chosen.jam\",\"jamorigin\":\"user\"") +
				(explicitEmpty ? std::string(",\"") + absent + "\":\"\"" : "") + "}";
			const auto defaults = Read(json);
			ASSERT_TRUE(defaults);
			EXPECT_EQ(missingJam ? L"" : L"chosen.jam", defaults->Jam);
			EXPECT_EQ(missingJam ? L"chosen.rig" : L"", defaults->Rig);
			EXPECT_EQ("user", missingJam ? defaults->RigOrigin : defaults->JamOrigin);
			EXPECT_FALSE(defaults->ConsoleAutoStart);
			EXPECT_EQ(22, defaults->WinPos.X);
			EXPECT_EQ("verbose", defaults->Logging.Event);
		}
}

TEST_F(StartupDefaultsTest, EmptyObjectIsValidButMalformedOrUnreadableDefaultsAreRejected)
{
	const auto empty = Read("{}");
	ASSERT_TRUE(empty);
	EXPECT_TRUE(empty->Jam.empty());
	EXPECT_TRUE(empty->Rig.empty());
	EXPECT_FALSE(Read("{} trailing"));
	EXPECT_FALSE(Read("[]"));
	EXPECT_FALSE(InitFile::Read((Directory / "missing.json").wstring()));
}

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
	auto str = "{\"preDelaySamps\":0,\"debounceMs\":59}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto trig = UserConfig::TriggerSettings::FromJson(json);

	ASSERT_TRUE(trig.has_value());
	ASSERT_EQ(0u, trig.value().PreDelaySamps);
	ASSERT_EQ(59, trig.value().DebounceMs);
}

TEST(UserConfig, ParsesMidiSettings) {
	auto str = "{\"name\":\"MPK mini\",\"enabled\":true}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto midi = UserConfig::MidiSettings::FromJson(json);

	ASSERT_TRUE(midi.has_value());
	ASSERT_EQ(0, midi.value().Name.compare("MPK mini"));
	ASSERT_TRUE(midi.value().Enabled);
}

TEST(UserConfig, ParsesMidiDeviceList) {
	auto str = "{\"devices\":[{\"name\":\"MPK mini\",\"enabled\":true},{\"name\":\"Launchpad X\",\"enabled\":false}],\"channelOverrideTriggers\":true,\"channelOverrideLive\":false}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto midi = UserConfig::MidiConfig::FromJson(json);

	ASSERT_TRUE(midi.has_value());
	ASSERT_EQ(2u, midi->Devices.size());
	EXPECT_EQ(0, midi->Devices[0].Name.compare("MPK mini"));
	EXPECT_TRUE(midi->Devices[0].Enabled);
	EXPECT_EQ(0, midi->Devices[1].Name.compare("Launchpad X"));
	EXPECT_FALSE(midi->Devices[1].Enabled);
	EXPECT_TRUE(midi->ChannelOverrideTriggers);
	EXPECT_FALSE(midi->ChannelOverrideLive);
}

TEST(UserConfig, MidiChannelOverrideDefaultsPreserveTriggerIsolation)
{
	auto str = "{\"devices\":[{\"name\":\"default\",\"enabled\":true}]}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto midi = UserConfig::MidiConfig::FromJson(json);

	ASSERT_TRUE(midi.has_value());
	EXPECT_FALSE(midi->ChannelOverrideTriggers);
	EXPECT_TRUE(midi->ChannelOverrideLive);
}

TEST(UserConfig, RejectsLegacySingleMidiDeviceShape) {
	auto str = "{\"name\":\"MPK mini\",\"enabled\":true}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto midi = UserConfig::MidiConfig::FromJson(json);

	EXPECT_FALSE(midi.has_value());
}

TEST(UserConfig, ParsesSerialSettings) {
	auto str = "{\"name\":\"pedal-a\",\"port\":\"COM3\",\"baudrate\":115200,\"enabled\":true}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto serial = UserConfig::SerialSettings::FromJson(json);

	ASSERT_TRUE(serial.has_value());
	ASSERT_EQ(0, serial.value().Name.compare("pedal-a"));
	ASSERT_EQ(0, serial.value().Port.compare("COM3"));
	ASSERT_EQ(115200u, serial.value().BaudRate);
	ASSERT_TRUE(serial.value().Enabled);
}

TEST(UserConfig, ParsesSerialDeviceList) {
	auto str = "{\"devices\":[{\"name\":\"pedal-a\",\"port\":\"COM3\",\"baudrate\":115200,\"enabled\":true},{\"name\":\"pedal-b\",\"port\":\"COM4\",\"baudrate\":57600,\"enabled\":false}]}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto serial = UserConfig::SerialConfig::FromJson(json);

	ASSERT_TRUE(serial.has_value());
	ASSERT_EQ(2u, serial->Devices.size());
	EXPECT_EQ(0, serial->Devices[0].Name.compare("pedal-a"));
	EXPECT_EQ(0, serial->Devices[0].Port.compare("COM3"));
	EXPECT_EQ(115200u, serial->Devices[0].BaudRate);
	EXPECT_TRUE(serial->Devices[0].Enabled);
	EXPECT_EQ(0, serial->Devices[1].Name.compare("pedal-b"));
	EXPECT_EQ(0, serial->Devices[1].Port.compare("COM4"));
	EXPECT_EQ(57600u, serial->Devices[1].BaudRate);
	EXPECT_FALSE(serial->Devices[1].Enabled);
}

TEST(UserConfig, ParsesFile) {
	std::string audio = "{\"name\":\"HDMI\",\"bufsize\":255,\"inlatency\":414,\"outlatency\":414,\"numchannelsin\":0,\"numchannelsout\":10}";
	std::string loop = "{\"fadeSamps\":54,\"seedGrainMinMs\":400,\"seedGrainTargetMaxMs\":3000,\"seedBpmMin\":80,\"seedQuantisation\":\"power\"}";
	std::string trigger = "{\"preDelaySamps\":0,\"debounceMs\":18}";
	std::string midi = "{\"devices\":[{\"name\":\"Launchkey\",\"enabled\":true},{\"name\":\"DrumPad\",\"enabled\":false}]}";
	std::string serial = "{\"devices\":[{\"name\":\"pedal-a\",\"port\":\"COM3\",\"baudrate\":115200,\"enabled\":true},{\"name\":\"pedal-b\",\"port\":\"COM4\",\"baudrate\":57600,\"enabled\":false}]}";
	
	auto str = "{\"name\":\"user\",\"audio\":" + audio + ",\"loop\":" + loop + ",\"trigger\":" + trigger + ",\"midi\":" + midi + ",\"serial\":" + serial + "}";
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

	ASSERT_EQ(0u, cfg.value().Trigger.PreDelaySamps);
	ASSERT_EQ(18, cfg.value().Trigger.DebounceMs);
	ASSERT_EQ(2u, cfg.value().Midi.Devices.size());
	ASSERT_EQ(0, cfg.value().Midi.Devices[0].Name.compare("Launchkey"));
	ASSERT_TRUE(cfg.value().Midi.Devices[0].Enabled);
	ASSERT_EQ(0, cfg.value().Midi.Devices[1].Name.compare("DrumPad"));
	ASSERT_FALSE(cfg.value().Midi.Devices[1].Enabled);
	ASSERT_EQ(2u, cfg.value().Serial.Devices.size());
	ASSERT_EQ(0, cfg.value().Serial.Devices[0].Name.compare("pedal-a"));
	ASSERT_EQ(0, cfg.value().Serial.Devices[0].Port.compare("COM3"));
	ASSERT_EQ(115200u, cfg.value().Serial.Devices[0].BaudRate);
	ASSERT_TRUE(cfg.value().Serial.Devices[0].Enabled);
	ASSERT_EQ(0, cfg.value().Serial.Devices[1].Name.compare("pedal-b"));
	ASSERT_EQ(0, cfg.value().Serial.Devices[1].Port.compare("COM4"));
	ASSERT_EQ(57600u, cfg.value().Serial.Devices[1].BaudRate);
	ASSERT_FALSE(cfg.value().Serial.Devices[1].Enabled);
}

TEST(UserConfig, DeducesDefaultLoopTimingFromLongLoop) {
	UserConfig cfg;

	auto timing = cfg.DeduceLoopTiming(48000ul * 8ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	ASSERT_EQ(24000u, timing->GrainSamps);
	ASSERT_EQ(16u, timing->LoopGrains);
	ASSERT_FLOAT_EQ(120.0f, timing->Bpm);
	ASSERT_EQ(16u, timing->Bpi);
}

TEST(UserConfig, DeducesDefaultLoopTimingBelowThreeSecondsWhenPossible) {
	UserConfig cfg;

	auto timing = cfg.DeduceLoopTiming(48000ul * 6ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	ASSERT_EQ(36000u, timing->GrainSamps);
	ASSERT_EQ(8u, timing->LoopGrains);
	ASSERT_FLOAT_EQ(80.0f, timing->Bpm);
	ASSERT_EQ(8u, timing->Bpi);
}

TEST(UserConfig, LoopTimingHonoursConfiguredTargetMaxGrain) {
	UserConfig cfg;
	cfg.Loop.SeedGrainTargetMaxMs = 5000u;

	auto timing = cfg.DeduceLoopTiming(48000ul * 6ul, 48000u);

	ASSERT_TRUE(timing.has_value());
	ASSERT_EQ(36000u, timing->GrainSamps);
	ASSERT_EQ(8u, timing->LoopGrains);
	ASSERT_FLOAT_EQ(80.0f, timing->Bpm);
	ASSERT_EQ(8u, timing->Bpi);
}

TEST(InitFile, DefaultJsonParsesWithoutVstDebugBlock) {
	const std::string roamingPath = "C:\\Users\\tester\\AppData\\Roaming\\Jamma";
	auto parsed = InitFile::FromStream(std::stringstream(InitFile::DefaultJson(roamingPath)));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(0, parsed->Jam.compare(L"C:\\Users\\tester\\AppData\\Roaming\\Jamma\\default.jam"));
	EXPECT_EQ(0, parsed->Rig.compare(L"C:\\Users\\tester\\AppData\\Roaming\\Jamma\\default.rig"));
	EXPECT_TRUE(parsed->ConsoleAutoStart);
}

TEST(InitFile, ParsesUiLoggingSetting) {
	auto parsed = InitFile::FromStream(std::stringstream("{\"logging\":{\"midi\":\"verbose\",\"ui\":\"verbose\"}}"));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ("verbose", parsed->Logging.Midi);
	EXPECT_EQ("verbose", parsed->Logging.Ui);
}

TEST(InitFile, ConsoleAutostartDefaultsOnAndPersistsOff) {
	auto legacy = InitFile::FromStream(std::stringstream("{}"));
	ASSERT_TRUE(legacy);
	EXPECT_TRUE(legacy->ConsoleAutoStart);
	legacy->ConsoleAutoStart = false;
	std::stringstream serialized;
	ASSERT_TRUE(InitFile::ToStream(*legacy, serialized));
	const auto disabled = InitFile::FromStream(std::move(serialized));
	ASSERT_TRUE(disabled);
	EXPECT_FALSE(disabled->ConsoleAutoStart);
	const auto wrongType = InitFile::FromStream(std::stringstream("{\"consoleautostart\":\"off\"}"));
	ASSERT_TRUE(wrongType);
	EXPECT_TRUE(wrongType->ConsoleAutoStart);
}

TEST(InitFile, ObsoleteConsoleHostPreferenceIsIgnored) {
	auto parsed = InitFile::FromStream(std::stringstream("{\"consoleforceconhost\":true,\"consoleautostart\":false}"));
	ASSERT_TRUE(parsed);
	EXPECT_FALSE(parsed->ConsoleAutoStart);
	std::stringstream serialized;
	ASSERT_TRUE(InitFile::ToStream(*parsed, serialized));
	EXPECT_EQ(serialized.str().find("consoleforceconhost"), std::string::npos);
}

TEST(InitFile, ToStreamWritesJsonThatParses) {
	InitFile ini;
	ini.Jam = L"C:\\Users\\matto\\AppData\\Roaming\\Jamma\\session.jam";
	ini.Rig = L"C:\\Users\\matto\\AppData\\Roaming\\Jamma\\default.rig";
	ini.JamLoadType = InitFile::LOAD_SPECIFIC;
	ini.RigLoadType = InitFile::LOAD_LAST;
	ini.WinPos = { -228, 23 };
	ini.WinSize = { 2030u, 1061u };
	ini.Logging.Ui = "verbose";

	std::stringstream ss;
	ASSERT_TRUE(InitFile::ToStream(ini, ss));

	auto parsed = InitFile::FromStream(std::stringstream(ss.str()));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(0, parsed->Jam.compare(L"C:\\Users\\matto\\AppData\\Roaming\\Jamma\\session.jam"));
	EXPECT_EQ(0, parsed->Rig.compare(L"C:\\Users\\matto\\AppData\\Roaming\\Jamma\\default.rig"));
	EXPECT_EQ(InitFile::LOAD_SPECIFIC, parsed->JamLoadType);
	EXPECT_EQ(InitFile::LOAD_LAST, parsed->RigLoadType);
	EXPECT_EQ(-228, parsed->WinPos.X);
	EXPECT_EQ(23, parsed->WinPos.Y);
	EXPECT_EQ(2030u, parsed->WinSize.Width);
	EXPECT_EQ(1061u, parsed->WinSize.Height);
	EXPECT_EQ("verbose", parsed->Logging.Ui);
}

TEST(InitFile, GeneratedOriginRoundTripsButLegacyDefaultsRemainUnmarked) {
	auto legacy = InitFile::FromStream(std::stringstream(InitFile::DefaultJson("C:\\Jamma")));
	ASSERT_TRUE(legacy.has_value());
	EXPECT_TRUE(legacy->RigOrigin.empty());
	EXPECT_TRUE(legacy->JamOrigin.empty());
	legacy->RigOrigin = std::string(io::StartupConfig::GeneratedOrigin);
	std::stringstream serialized;
	ASSERT_TRUE(InitFile::ToStream(*legacy, serialized));
	auto parsed = InitFile::FromStream(std::move(serialized));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(io::StartupConfig::GeneratedOrigin, parsed->RigOrigin);
	EXPECT_TRUE(parsed->JamOrigin.empty());
}

TEST(StartupConfig, RigAndJamRecoveryDecisionsAreIndependent) {
	using Startup = io::StartupConfig;
	const auto rig = Startup::Decide(Startup::Classify(true, true, true), "");
	const auto jam = Startup::Decide(Startup::Classify(true, true, false), "");
	EXPECT_TRUE(rig.UseExisting);
	EXPECT_FALSE(rig.Generate);
	EXPECT_FALSE(rig.PublishedGenerated); // A legacy bootstrap file is still selected.
	EXPECT_FALSE(jam.UseExisting);
	EXPECT_TRUE(jam.Generate);
	EXPECT_TRUE(jam.Recovery);
	EXPECT_EQ(Startup::FileState::Missing, Startup::Classify(false, false, false));
	EXPECT_EQ(Startup::FileState::Unreadable, Startup::Classify(true, false, false));
	const auto generated = Startup::Decide(Startup::FileState::Valid, Startup::GeneratedOrigin);
	EXPECT_TRUE(generated.PublishedGenerated);
}

TEST(StartupConfig, RejectsUnusableAudioAndDuplicateTriggerIdentity) {
	auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson));
	ASSERT_TRUE(rig.has_value());
	EXPECT_TRUE(io::StartupConfig::ValidateRig(*rig));
	rig->User.Audio.NumChannelsOut = 0u;
	EXPECT_TRUE(io::StartupConfig::ValidateRig(*rig)); // Input-only devices remain supported.
	rig->User.Audio.NumChannelsIn = 0u;
	EXPECT_FALSE(io::StartupConfig::ValidateRig(*rig));
	rig->User.Audio.NumChannelsOut = 2u;
	rig->Triggers[0].Id = "shared";
	rig->Triggers.push_back(rig->Triggers[0]);
	rig->Triggers.back().Name = "other";
	EXPECT_FALSE(io::StartupConfig::ValidateRig(*rig));
}

TEST(StartupConfig, RequiresDistinctNamedStations) {
	auto jam = io::JamFile::FromStream(std::stringstream(io::JamFile::DefaultJson));
	ASSERT_TRUE(jam.has_value());
	EXPECT_TRUE(io::StartupConfig::ValidateJam(*jam));
	jam->Stations.push_back(jam->Stations[0]);
	EXPECT_FALSE(io::StartupConfig::ValidateJam(*jam));
}

TEST(StartupConfig, SelectedRigRetainsUnavailableTargetsAndRejectsInvalidTargets) {
	auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson));
	auto jam = io::JamFile::FromStream(std::stringstream(io::JamFile::DefaultJson));
	ASSERT_TRUE(rig.has_value());
	ASSERT_TRUE(jam.has_value());
	rig->Triggers[0].StationTarget = "Station1";
	EXPECT_TRUE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
	rig->Triggers[0].StationTarget = "Missing";
	EXPECT_TRUE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
	rig->Triggers[0].StationTarget = std::string();
	EXPECT_FALSE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
	rig->Triggers[0].StationTarget.reset();
	EXPECT_TRUE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
	jam->Stations.push_back(jam->Stations[0]);
	EXPECT_FALSE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
}

TEST(StartupConfig, GeneratedRigUsesOpenedInputsAndFirstConnectedMidiWithoutCapture) {
	auto templateRig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson));
	ASSERT_TRUE(templateRig);
	for (const unsigned int channels : {0u, 1u, 2u, 8u}) {
		const auto rig = io::StartupConfig::GeneratedRig(*templateRig, channels, {"Failed port skipped", "Second port"});
		ASSERT_EQ(1u, rig.Triggers.size());
		const auto& trigger = rig.Triggers.front();
		EXPECT_EQ("first-run-trigger-1", trigger.Id); ASSERT_TRUE(trigger.StationTarget); EXPECT_EQ("Station1", *trigger.StationTarget);
		EXPECT_EQ(std::min(channels, 2u), trigger.CaptureInputs.Audio.size());
		for (unsigned int index = 0u; index < trigger.CaptureInputs.Audio.size(); ++index) EXPECT_EQ(index, trigger.CaptureInputs.Audio[index]);
		EXPECT_TRUE(trigger.CaptureInputs.Midi.empty());
		ASSERT_FALSE(trigger.ControlBindings.Activate.empty()); ASSERT_FALSE(trigger.ControlBindings.Ditch.empty());
		const auto& activate = trigger.ControlBindings.Activate.back();
		const auto& ditch = trigger.ControlBindings.Ditch.back();
		EXPECT_EQ(io::RigFile::BindingSource::Midi, activate.Source); EXPECT_EQ("midi-1", activate.Device);
		ASSERT_EQ(2u, rig.User.Midi.Devices.size()); EXPECT_EQ("Failed port skipped", rig.User.Midi.Devices[0].Name); EXPECT_EQ("midi-1", rig.User.Midi.Devices[0].Id);
		EXPECT_EQ(io::RigFile::NOTE, activate.Press.Kind); EXPECT_EQ(0u, activate.Press.Channel);
		EXPECT_EQ(1u, activate.Press.Code); EXPECT_EQ(1u, activate.Press.State); EXPECT_EQ(0u, activate.Release.State);
		EXPECT_EQ(2u, ditch.Press.Code);
		std::stringstream stream; ASSERT_TRUE(io::RigFile::ToJsonStream(rig, stream));
		const auto parsed = io::RigFile::FromStream(std::stringstream(stream.str())); ASSERT_TRUE(parsed);
		EXPECT_TRUE(io::StartupConfig::ValidateRig(*parsed)); EXPECT_EQ(trigger.CaptureInputs.Audio, parsed->Triggers.front().CaptureInputs.Audio);
		EXPECT_EQ(activate.Device, parsed->Triggers.front().ControlBindings.Activate.back().Device);
	}
	const auto noMidi = io::StartupConfig::GeneratedRig(*templateRig, 0u, {});
	EXPECT_TRUE(noMidi.User.Midi.Devices.empty());
	EXPECT_TRUE(std::none_of(noMidi.Triggers.front().ControlBindings.Activate.begin(), noMidi.Triggers.front().ControlBindings.Activate.end(), [](const io::RigFile::Binding& binding) { return binding.Source == io::RigFile::BindingSource::Midi; }));
}

TEST(StartupConfig, GeneratedRigKeepsDuplicateMidiPortsOnReload) {
	auto templateRig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson));
	ASSERT_TRUE(templateRig.has_value());
	const auto rig = io::StartupConfig::GeneratedRig(*templateRig, 2u,
		{ "Port A", "Port A", "Port B" });
	ASSERT_EQ(3u, rig.User.Midi.Devices.size());
	std::stringstream stream;
	ASSERT_TRUE(io::RigFile::ToJsonStream(rig, stream));
	auto parsed = io::RigFile::FromStream(std::stringstream(stream.str()));
	ASSERT_TRUE(parsed.has_value());
	ASSERT_EQ(3u, parsed->User.Midi.Devices.size());
	EXPECT_EQ("Port A", parsed->User.Midi.Devices[0].Name);
	EXPECT_EQ("Port A", parsed->User.Midi.Devices[1].Name);
	EXPECT_EQ("Port B", parsed->User.Midi.Devices[2].Name);
}

TEST(StartupConfig, GeneratedRigTargetsAnExistingSelectedJamStation) {
	auto templateRig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson));
	ASSERT_TRUE(templateRig.has_value());
	const auto rig = io::StartupConfig::GeneratedRig(*templateRig, 2u, {}, "Guitar");
	ASSERT_EQ(1u, rig.Triggers.size());
	ASSERT_TRUE(rig.Triggers.front().StationTarget.has_value());
	EXPECT_EQ("Guitar", *rig.Triggers.front().StationTarget);
	std::stringstream serialized;
	ASSERT_TRUE(io::RigFile::ToJsonStream(rig, serialized));
	const auto parsed = io::RigFile::FromStream(std::stringstream(serialized.str()));
	ASSERT_TRUE(parsed.has_value()) << serialized.str();
	EXPECT_EQ("Guitar", *parsed->Triggers.front().StationTarget);
}

TEST(StartupConfig, EmptyJamRoundTripsAsAUsableStationWithoutNinjamCredentials) {
	auto jam = io::JamFile::FromStream(std::stringstream(io::JamFile::DefaultJson));
	ASSERT_TRUE(jam.has_value());
	jam->Name = "First-run jam";
	jam->Ninjam.reset();
	jam->Stations.front().LoopTakes.clear();
	std::stringstream stream;
	ASSERT_TRUE(io::JamFile::ToStream(*jam, stream));
	auto parsed = io::JamFile::FromStream(std::stringstream(stream.str()));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_TRUE(io::StartupConfig::ValidateJam(*parsed));
	ASSERT_EQ(1u, parsed->Stations.size());
	EXPECT_EQ("Station1", parsed->Stations.front().Name);
	EXPECT_TRUE(parsed->Stations.front().LoopTakes.empty());
	EXPECT_FALSE(parsed->Ninjam.has_value());
}

TEST(UserConfig, OverdubTimingHelpersIncludeAndExcludeOutputLatencyAtRightPoints) {
	UserConfig cfg;
	cfg.Audio.SampleRate = 48000u;

	EXPECT_EQ(constants::MaxLoopFadeSamps + 9600u, cfg.TriggerLoopAlignmentSamps());
	EXPECT_EQ(-static_cast<long>(constants::MaxLoopFadeSamps + 9600u + 96u),
		cfg.OverdubSourceReadOffset(96u));
	EXPECT_EQ(cfg.LoopPlayPos(0, 4096ul, 0u), cfg.OverdubPlayPos(0, 4096ul));
}

TEST(UserConfig, ConvertsSignedFractionalTriggerMillisecondsAndFixedCapacity)
{
	EXPECT_EQ(8820u, UserConfig::TriggerHistorySamples(44100u).value());
	EXPECT_EQ(9600u, UserConfig::TriggerHistorySamples(48000u).value());
	EXPECT_EQ(3793, UserConfig::TriggerPreDelaySamples(86.0, 44100u).value());
	EXPECT_EQ(4128, UserConfig::TriggerPreDelaySamples(86.0, 48000u).value());
	EXPECT_EQ(-3793, UserConfig::TriggerPreDelaySamples(-86.0, 44100u).value());
	EXPECT_EQ(6, UserConfig::TriggerPreDelaySamples(0.125, 48000u).value());
	EXPECT_EQ(-6, UserConfig::TriggerPreDelaySamples(-0.125, 48000u).value());
	EXPECT_EQ(6, UserConfig::TriggerPreDelaySamples(0.125, 44100u).value());
	EXPECT_EQ(-6, UserConfig::TriggerPreDelaySamples(-0.125, 44100u).value());
	EXPECT_EQ(9600, UserConfig::TriggerPreDelaySamples(200.0, 48000u).value());
	EXPECT_FALSE(UserConfig::TriggerPreDelaySamples(200.001, 48000u));
	EXPECT_FALSE(UserConfig::TriggerPreDelaySamples(-200.001, 48000u));
	EXPECT_FALSE(UserConfig::TriggerPreDelaySamples(std::numeric_limits<double>::infinity(), 48000u));
	EXPECT_FALSE(UserConfig::TriggerPreDelaySamples(std::numeric_limits<double>::quiet_NaN(), 48000u));
	EXPECT_FALSE(UserConfig::TriggerPreDelaySamples(0.0, 0u));
	EXPECT_FALSE(UserConfig::TriggerPreDelaySamples(-200.0, 384000u));
	EXPECT_TRUE(UserConfig::TriggerPreDelaySamples(200.0, 384000u));
	EXPECT_FALSE(UserConfig::TriggerHistorySamples((std::numeric_limits<unsigned int>::max)()));
}

TEST(UserConfig, ActiveRateControlsAdcCapacityIndependentlyOfLegacyField)
{
	UserConfig cfg;
	cfg.Audio.SampleRate = 44100u;
	cfg.Trigger.PreDelaySamps = 123u; // Deprecated member never contributes timing.
	EXPECT_EQ(78820u, cfg.TriggerLoopAlignmentSamps());
	EXPECT_EQ(79600u, cfg.TriggerLoopAlignmentSamps(48000u));
	EXPECT_EQ(79500u, cfg.AdcBufferDelay(100u, 48000u));
	EXPECT_EQ(0u, cfg.AdcBufferDelay(80000u, 48000u));
	EXPECT_EQ(-79700l, cfg.OverdubSourceReadOffset(100u, 48000u));
}

TEST(UserConfig, RejectsNonzeroLegacyTriggerPredelay)
{
	auto parsed = Json::FromStream(std::stringstream(R"({"preDelaySamps":1,"debounceMs":280})"));
	ASSERT_TRUE(parsed);
	EXPECT_FALSE(UserConfig::TriggerSettings::FromJson(std::get<Json::JsonPart>(*parsed)));
}

TEST(UserConfig, DefaultConstructedAudioHasDefinedUnavailableValues)
{
    UserConfig config;
    EXPECT_EQ(0u, config.Audio.BufSize);
    EXPECT_EQ(0u, config.Audio.LatencyIn);
    EXPECT_EQ(0u, config.Audio.LatencyOut);
    EXPECT_EQ(0u, config.Audio.NumBuffers);
    EXPECT_EQ(0u, config.Audio.NumChannelsIn);
    EXPECT_EQ(0u, config.Audio.NumChannelsOut);
}
