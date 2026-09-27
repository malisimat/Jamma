
#include "gtest/gtest.h"
#include <algorithm>
#include <regex>
#include "resources/ResourceLib.h"
#include "io/InitFile.h"
#include "io/StartupConfig.h"
#include "io/Json.h"
#include "io/UserConfig.h"

using io::Json;
using io::InitFile;
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
	std::string trigger = "{\"preDelay\":21,\"debounceSamps\":18}";
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

	ASSERT_EQ(21, cfg.value().Trigger.PreDelay);
	ASSERT_EQ(18, cfg.value().Trigger.DebounceSamps);
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
}

TEST(InitFile, ParsesUiLoggingSetting) {
	auto parsed = InitFile::FromStream(std::stringstream("{\"logging\":{\"midi\":\"verbose\",\"ui\":\"verbose\"}}"));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ("verbose", parsed->Logging.Midi);
	EXPECT_EQ("verbose", parsed->Logging.Ui);
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

TEST(StartupConfig, SelectedRigRequiresExplicitTargetsInSelectedJam) {
	auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson));
	auto jam = io::JamFile::FromStream(std::stringstream(io::JamFile::DefaultJson));
	ASSERT_TRUE(rig.has_value());
	ASSERT_TRUE(jam.has_value());
	rig->Triggers[0].StationTarget = "Station1";
	EXPECT_TRUE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
	rig->Triggers[0].StationTarget = "Missing";
	EXPECT_FALSE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
	rig->Triggers[0].StationTarget = std::string();
	EXPECT_TRUE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
	rig->Triggers[0].StationTarget.reset();
	EXPECT_TRUE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
	jam->Stations.push_back(jam->Stations[0]);
	EXPECT_FALSE(io::StartupConfig::ValidateRigForJam(*rig, *jam));
}

TEST(StartupConfig, GeneratedRigUsesOpenedInputsAndFirstConnectedMidiWithoutCapture) {
	auto templateRig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson));
	ASSERT_TRUE(templateRig.has_value());
	for (const unsigned int channels : { 0u, 1u, 2u, 8u }) {
		const auto rig = io::StartupConfig::GeneratedRig(*templateRig, channels,
			{ "Failed port skipped", "Second port" });
		ASSERT_EQ(1u, rig.Triggers.size());
		const auto& trigger = rig.Triggers.front();
		EXPECT_EQ("first-run-trigger-1", trigger.Id);
		ASSERT_TRUE(trigger.StationTarget.has_value());
		EXPECT_EQ("Station1", *trigger.StationTarget);
		EXPECT_EQ(std::min(channels, 2u), trigger.InputChannels.size());
		for (unsigned int index = 0; index < trigger.InputChannels.size(); ++index)
			EXPECT_EQ(index, trigger.InputChannels[index]);
		EXPECT_EQ(io::RigFile::Trigger::MidiInputMode::None, trigger.MidiInputs);
		EXPECT_TRUE(trigger.MidiInputDevices.empty());
		ASSERT_TRUE(trigger.MidiTrigger.has_value());
		EXPECT_EQ("Failed port skipped", trigger.MidiTrigger->Device);
		EXPECT_EQ(io::RigFile::NOTE, trigger.MidiTrigger->Activate.Kind);
		EXPECT_EQ(0u, trigger.MidiTrigger->Activate.Channel);
		EXPECT_EQ(1u, trigger.MidiTrigger->Activate.Id);
		EXPECT_EQ(1u, trigger.MidiTrigger->Activate.State);
		EXPECT_FALSE(trigger.MidiTrigger->Activate.MatchAnyChannel);
		EXPECT_EQ(2u, trigger.MidiTrigger->Ditch.Id);
		std::stringstream stream;
		ASSERT_TRUE(io::RigFile::ToJsonStream(rig, stream));
		auto parsed = io::RigFile::FromStream(std::stringstream(stream.str()));
		ASSERT_TRUE(parsed.has_value());
		EXPECT_TRUE(io::StartupConfig::ValidateRig(*parsed));
		EXPECT_EQ(trigger.InputChannels, parsed->Triggers.front().InputChannels);
		EXPECT_EQ(trigger.MidiTrigger->Device, parsed->Triggers.front().MidiTrigger->Device);
	}
	const auto noMidi = io::StartupConfig::GeneratedRig(*templateRig, 0u, {});
	EXPECT_TRUE(noMidi.User.Midi.Devices.empty());
	EXPECT_FALSE(noMidi.Triggers.front().MidiTrigger.has_value());
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
	cfg.Trigger.PreDelay = 128u;

	EXPECT_EQ(constants::MaxLoopFadeSamps + 128u, cfg.TriggerLoopAlignmentSamps());
	EXPECT_EQ(-static_cast<long>(constants::MaxLoopFadeSamps + 128u + 96u),
		cfg.OverdubSourceReadOffset(96u));
	EXPECT_EQ(cfg.LoopPlayPos(0, 4096ul, 0u), cfg.OverdubPlayPos(0, 4096ul));
}
