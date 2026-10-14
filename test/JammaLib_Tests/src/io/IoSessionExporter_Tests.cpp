#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>

#include "base/AudioSink.h"
#include "engine/Loop.h"
#include "engine/LoopTake.h"
#include "engine/Quantiser.h"
#include "engine/Station.h"
#include "engine/Trigger.h"
#include "io/IoSessionExporter.h"
#include "io/JamFile.h"
#include "io/NativeMidiSidecar.h"

using base::AudioWriteRequest;
using base::Audible;
using engine::Loop;
using engine::LoopParams;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Quantiser;
using engine::Station;
using engine::StationParams;
using engine::Trigger;
using engine::TriggerParams;
using midi::MidiEvent;

class IoSessionExporterTest
{
public:
	static std::shared_ptr<Station> MakeStation(const std::string& name)
	{
		StationParams params;
		params.Name = name;
		params.Size = { 200u, 200u };
		audio::MergeMixBehaviourParams merge;
		auto station = std::make_shared<Station>(params, Station::GetMixerParams(params.Size, merge));
		station->SetupBuffers(64u);
		station->SetNumDacChannels(2u);
		return station;
	}

	static std::shared_ptr<LoopTake> MakeTake(const std::string& id)
	{
		LoopTakeParams params;
		params.Id = id;
		params.Size = { 100u, 100u };
		audio::MergeMixBehaviourParams merge;
		return std::make_shared<LoopTake>(params, LoopTake::GetMixerParams(params.Size, merge));
	}

	static std::shared_ptr<Loop> MakeAudioLoop(unsigned long length,
		unsigned int channel,
		unsigned long bodyPlayIndex,
		float amplitudeScale = 1.0f)
	{
		LoopParams params;
		params.Id = "audio-" + std::to_string(channel) + "-" + std::to_string(length);
		params.Channel = channel;
		params.Size = { 80u, 80u };
		audio::WireMixBehaviourParams wire;
		wire.Channels = { channel };
		auto loop = std::make_shared<Loop>(params, Loop::GetMixerParams(params.Size, wire));

		loop->Record();
		std::vector<float> samples(constants::MaxLoopFadeSamps + length, 0.0f);
		for (unsigned long sample = 0ul; sample < length; ++sample)
			samples[constants::MaxLoopFadeSamps + sample] = static_cast<float>(sample + 1ul) * amplitudeScale;
		AudioWriteRequest request;
		request.samples = samples.data();
		request.numSamps = static_cast<unsigned int>(samples.size());
		request.stride = 1u;
		request.fadeCurrent = 0.0f;
		request.fadeNew = 1.0f;
		request.source = Audible::AUDIOSOURCE_ADC;
		loop->OnBlockWrite(request, 0);
		loop->EndWrite(static_cast<unsigned int>(samples.size()), true);
		loop->Play(constants::MaxLoopFadeSamps, length, false);
		loop->SetBodyPlayIndex(bodyPlayIndex);
		return loop;
	}

	static std::filesystem::path MakeDirectory()
	{
		static unsigned int serial = 0u;
		const auto dir = std::filesystem::temp_directory_path() /
			("jamma-export-roundtrip-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++serial));
		std::filesystem::create_directory(dir);
		return dir;
	}

	static std::optional<io::JamFile> ExportManifest(
		const std::vector<std::shared_ptr<Station>>& stations,
		const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot = {},
		const engine::PerformanceGroups* groups = nullptr)
	{
		Quantiser quantiser;
		quantiser.SetClock(std::make_shared<utils::Timer>());
		audio::AudioStreamParams stream{};
		io::UserConfig user{};
		std::mutex sceneMutex;
		const auto dir = MakeDirectory();
		const bool exported = io::IoSessionExporter::ExportSessionToDirectory(stations, rigSnapshot, quantiser,
			io::JamFile::GlobalMidiQuantState::Off, 0.0, user, stream, nullptr,
			sceneMutex, nullptr, dir.wstring(), groups);
		std::optional<io::JamFile> jam;
		if (exported)
		{
			std::ifstream manifest(dir / "session.jam");
			if (manifest)
			{
				std::stringstream contents;
				contents << manifest.rdbuf();
				jam = io::JamFile::FromStream(std::move(contents));
			}
		}
		std::filesystem::remove_all(dir);
		return jam;
	}

	static std::shared_ptr<const engine::RigSnapshot> Snapshot(
		std::initializer_list<std::pair<std::string, std::shared_ptr<Trigger>>> triggers)
	{
		auto snapshot = std::make_shared<engine::RigSnapshot>();
		for (const auto& [id, trigger] : triggers)
			snapshot->Triggers.push_back({ id, 0u, trigger, std::nullopt, {}, {}, {},
				io::RigFile::Trigger::MidiInputMode::None, {}, {} });
		return snapshot;
	}
};

TEST(IoSessionExporter, PersistsDistinctHistoryForEveryTrigger)
{
	auto station = IoSessionExporterTest::MakeStation("shared");
	auto first = std::make_shared<Trigger>(TriggerParams{});
	first->RestoreTakes({
		{ engine::TriggerTake::SOURCE_ADC, "source-a", "target-a" },
		{ engine::TriggerTake::SOURCE_ADC, "source-b", "target-b" }
	});
	auto second = std::make_shared<Trigger>(TriggerParams{});
	second->RestoreTakes({ { engine::TriggerTake::SOURCE_ADC, "other-source", "other-target" } });
	const auto jam = IoSessionExporterTest::ExportManifest({ station },
		IoSessionExporterTest::Snapshot({ { "first-id", first }, { "second-id", second } }));
	ASSERT_TRUE(jam.has_value());
	ASSERT_EQ(1u, jam->Stations.size());
	ASSERT_EQ(2u, jam->TriggerHistories.size());
	EXPECT_EQ("first-id", jam->TriggerHistories[0].TriggerId);
	ASSERT_EQ(2u, jam->TriggerHistories[0].Takes.size());
	EXPECT_EQ("source-a", jam->TriggerHistories[0].Takes[0].SourceTakeId);
	EXPECT_EQ("target-a", jam->TriggerHistories[0].Takes[0].TargetTakeId);
	EXPECT_EQ("source-b", jam->TriggerHistories[0].Takes[1].SourceTakeId);
	EXPECT_EQ("target-b", jam->TriggerHistories[0].Takes[1].TargetTakeId);
	EXPECT_EQ("second-id", jam->TriggerHistories[1].TriggerId);
	ASSERT_EQ(1u, jam->TriggerHistories[1].Takes.size());
	EXPECT_EQ("other-target", jam->TriggerHistories[1].Takes[0].TargetTakeId);
}

TEST(IoSessionExporter, PersistsHistoryForAnUnboundTrigger)
{
	auto firstStation = IoSessionExporterTest::MakeStation("first");
	auto secondStation = IoSessionExporterTest::MakeStation("second");
	auto trigger = std::make_shared<Trigger>(TriggerParams{});
	trigger->RestoreTakes({ { engine::TriggerTake::SOURCE_ADC, "source", "target" } });
	const auto jam = IoSessionExporterTest::ExportManifest({ firstStation, secondStation },
		IoSessionExporterTest::Snapshot({ { "trigger-id", trigger } }));
	ASSERT_TRUE(jam.has_value());
	ASSERT_EQ(1u, jam->TriggerHistories.size());
	EXPECT_EQ("trigger-id", jam->TriggerHistories[0].TriggerId);
	EXPECT_EQ("source", jam->TriggerHistories[0].Takes[0].SourceTakeId);
	EXPECT_EQ("target", jam->TriggerHistories[0].Takes[0].TargetTakeId);
}

TEST(IoSessionExporter, ExplicitDirectoryRoundTripsLocalManifestAndSidecars)
{
	auto firstStation = IoSessionExporterTest::MakeStation("first");
	auto firstTrigger = std::make_shared<Trigger>(TriggerParams{});
	firstTrigger->RestoreTakes({ { engine::TriggerTake::SOURCE_ADC, "prior-take", "saved-take" } });
	auto secondTrigger = std::make_shared<Trigger>(TriggerParams{});
	secondTrigger->RestoreTakes({ { engine::TriggerTake::SOURCE_ADC, "other-source", "other-take" } });
	firstStation->SetAllowedMidiChannels({ 1, 3, 16 });
	auto firstTake = IoSessionExporterTest::MakeTake("first-take");
	firstTake->SetMidiQuantisation({ true, midi::MidiQuantisationFraction::Eighth, 0u, 7 });
	LoopTake::MidiExportState midiState;
	midiState.PlayIndex = 23u;
	midiState.LoopLengthSamps = 97u;
	midiState.Quantisation = firstTake->MidiQuantisation();
	midiState.QuantisationTransportStartSamps = 777u;
	LoopTake::MidiStreamExport midiStream;
	midiStream.Channel = 0u;
	midiStream.Device = "keyboard";
	midiStream.Loop.LoopLengthSamps = 97u;
	midiStream.Loop.EventCount = 3u;
	midiStream.Loop.Events[0] = MidiEvent::MakeNoteOn(40u, 0u, 60u, 91u);
	midiStream.Loop.Events[1] = MidiEvent::MakeNoteOn(40u, 0u, 64u, 92u);
	midiStream.Loop.Events[2] = MidiEvent::MakeNoteOff(50u, 0u, 60u);
	midiState.Streams.push_back(std::move(midiStream));
	ASSERT_TRUE(firstTake->RestoreMidiFromExport(midiState));
	firstTake->AddLoop(IoSessionExporterTest::MakeAudioLoop(43u, 0u, 13u));
	firstTake->CommitChanges();
	firstStation->AddTake(firstTake);
	firstStation->CommitChanges();
	ASSERT_TRUE(firstTake->RestoreMixerLevels(0.62, { 0.72 }));
	ASSERT_TRUE(firstStation->RestoreMixerLevels(0.42, { 0.52, 0.82 }));

	auto secondStation = IoSessionExporterTest::MakeStation("second");
	auto secondTake = IoSessionExporterTest::MakeTake("second-take");
	secondTake->SetMidiQuantisation({ false, midi::MidiQuantisationFraction::Quarter, 0u, -9 });
	secondTake->AddLoop(IoSessionExporterTest::MakeAudioLoop(61u, 1u, 37u));
	secondTake->CommitChanges();
	secondStation->AddTake(secondTake);
	secondStation->CommitChanges();

	auto timer = std::make_shared<utils::Timer>();
	Quantiser quantiser;
	quantiser.SetClock(timer);
	constexpr unsigned int localGrainSamps = 16u;
	timer->SetQuantisation(localGrainSamps, utils::Timer::QUANTISE_MULTIPLE);
	timer->SetSeedSourceLength(128u);
	ASSERT_TRUE(timer->InitialiseAbsoluteSamplePos((9ull * 128ull) + 17ull));
	quantiser.SetMidiGrain(localGrainSamps, "test first-loop seed", { firstStation, secondStation });
	EXPECT_EQ(localGrainSamps, timer->QuantiseSamps());
	EXPECT_EQ(localGrainSamps, quantiser.EffectiveSamps());

	audio::AudioStreamParams stream{};
	stream.SampleRate = 48000u;
	io::UserConfig user{};
	std::mutex sceneMutex;
	const auto dir = IoSessionExporterTest::MakeDirectory();
	ASSERT_TRUE(io::IoSessionExporter::ExportSessionToDirectory({ firstStation, secondStation },
		IoSessionExporterTest::Snapshot({ { "first-id", firstTrigger }, { "second-id", secondTrigger } }),
		quantiser,
		io::JamFile::GlobalMidiQuantState::All,
		0.25,
		user,
		stream,
		nullptr,
		sceneMutex,
		nullptr,
		dir.wstring()));

	std::ifstream manifest(dir / "session.jam");
	ASSERT_TRUE(manifest);
	std::stringstream manifestContents;
	manifestContents << manifest.rdbuf();
	auto jam = io::JamFile::FromStream(std::move(manifestContents));
	ASSERT_TRUE(jam.has_value());
	EXPECT_EQ((9ull * 128ull) + 17ull, jam->AbsoluteSamplePos);
	EXPECT_EQ(128ul, jam->MasterLengthSamps);
	EXPECT_EQ(localGrainSamps, jam->QuantiseSamps);
	EXPECT_EQ(utils::Timer::QUANTISE_MULTIPLE, jam->Quantisation);
	EXPECT_EQ(2u, jam->Stations.size());
	EXPECT_DOUBLE_EQ(0.42, jam->Stations[0].MasterLevel);
	ASSERT_GE(jam->Stations[0].BusLevels.size(), 2u);
	EXPECT_DOUBLE_EQ(0.52, jam->Stations[0].BusLevels[0]);
	EXPECT_DOUBLE_EQ(0.82, jam->Stations[0].BusLevels[1]);
	ASSERT_EQ(1u, jam->Stations[0].LoopTakes.size());
	EXPECT_EQ(std::vector<int>({ 1, 3, 16 }), jam->Stations[0].AllowedMidiChannels);
	ASSERT_EQ(2u, jam->TriggerHistories.size());
	EXPECT_EQ("prior-take", jam->TriggerHistories[0].Takes[0].SourceTakeId);
	EXPECT_EQ("saved-take", jam->TriggerHistories[0].Takes[0].TargetTakeId);
	const auto& savedTake = jam->Stations[0].LoopTakes[0];
	EXPECT_DOUBLE_EQ(0.62, savedTake.MasterLevel);
	EXPECT_EQ((std::vector<double>{ 0.72 }), savedTake.BusLevels);
	EXPECT_TRUE(savedTake.MidiQuantEnabled);
	EXPECT_EQ(97ul, savedTake.MidiPlayLength);
	EXPECT_EQ(23ul, savedTake.MidiPlayIndex);
	EXPECT_EQ(777ull, savedTake.MidiQuantTransportStart);
	ASSERT_EQ(1u, savedTake.Loops.size());
	EXPECT_EQ(43ul, savedTake.Loops[0].Length);
	EXPECT_EQ(13ul, savedTake.Loops[0].BodyPlayIndex);
	ASSERT_EQ(1u, savedTake.MidiStreams.size());

	std::ifstream midiFile(dir / savedTake.MidiStreams[0].SidecarPath, std::ios::binary);
	ASSERT_TRUE(midiFile);
	std::string sidecarError;
	auto midi = io::NativeMidiSidecar::FromStream(midiFile, &sidecarError);
	ASSERT_TRUE(midi.has_value()) << sidecarError;
	ASSERT_EQ(97u, midi->LogicalLength);
	ASSERT_EQ(3u, midi->Events.size());
	EXPECT_EQ(40u, midi->Events[0].SampleOffset);
	EXPECT_EQ(60u, midi->Events[0].Data1);
	EXPECT_EQ(40u, midi->Events[1].SampleOffset);
	EXPECT_EQ(64u, midi->Events[1].Data1);
	EXPECT_EQ(50u, midi->Events[2].SampleOffset);
	EXPECT_EQ(60u, midi->Events[2].Data1);

	LoopTakeParams restoredParams;
	restoredParams.Id = savedTake.Name;
	restoredParams.Size = { 80u, 80u };
	auto restoredTake = LoopTake::FromFile(restoredParams, savedTake, dir.wstring());
	ASSERT_TRUE(restoredTake.has_value());
	restoredTake.value()->CommitChanges();
	EXPECT_DOUBLE_EQ(0.62, restoredTake.value()->MasterLevelForExport());
	EXPECT_EQ((std::vector<double>{ 0.72 }), restoredTake.value()->BusLevelsForExport());
	EXPECT_EQ(23ul, restoredTake.value()->MidiPlayIndex());
	EXPECT_EQ(97ul, restoredTake.value()->MidiLoopLengthSamps());
	ASSERT_EQ(1u, restoredTake.value()->GetLoops().size());
	EXPECT_EQ(13ul, restoredTake.value()->GetLoops()[0]->BodyPlayIndex());
	ASSERT_EQ(1u, restoredTake.value()->GetMidiLoops().size());
	MidiEvent restoredEvent{};
	ASSERT_TRUE(restoredTake.value()->GetMidiLoops()[0]->TryGetEvent(0u, restoredEvent));
	EXPECT_EQ(40u, restoredEvent.sampleOffset);
	EXPECT_EQ(60u, restoredEvent.data1);
	ASSERT_TRUE(restoredTake.value()->GetMidiLoops()[0]->TryGetEvent(1u, restoredEvent));
	EXPECT_EQ(40u, restoredEvent.sampleOffset);
	EXPECT_EQ(64u, restoredEvent.data1);

	// Cross both the audio and MIDI loop boundaries and verify that the restored
	// entity-local cursors advance exactly like the source session.
	firstTake->EndMultiPlay(100u);
	restoredTake.value()->EndMultiPlay(100u);
	EXPECT_EQ(firstTake->MidiPlayIndex(), restoredTake.value()->MidiPlayIndex());
	ASSERT_EQ(firstTake->GetLoops().size(), restoredTake.value()->GetLoops().size());
	EXPECT_EQ(firstTake->GetLoops()[0]->BodyPlayIndex(),
		restoredTake.value()->GetLoops()[0]->BodyPlayIndex());

	midiFile.close();
	manifest.close();
	std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, AudioOnlyTakeRestoresTapBaseGridAfterReload)
{
	auto station = IoSessionExporterTest::MakeStation("audio-only");
	auto take = IoSessionExporterTest::MakeTake("tap-base");
	take->AddLoop(IoSessionExporterTest::MakeAudioLoop(101u, 0u, 0u, 0.005f));
	take->SetMidiBaseGrid(101u, 4u);
	take->CommitChanges();
	station->AddTake(take);
	station->CommitChanges();
	const auto dir = IoSessionExporterTest::MakeDirectory();
	Quantiser quantiser;
	quantiser.SetClock(std::make_shared<utils::Timer>());
	quantiser.ApplyTiming({ 25u, 100ul, 4u, 120.0f, 4u }, "audio-only tap-base fixture");
	std::mutex sceneMutex;
    audio::AudioStreamParams stream{}; stream.SampleRate = 48000u;
	ASSERT_TRUE(io::IoSessionExporter::ExportSessionToDirectory({ station }, {}, quantiser,
		io::JamFile::GlobalMidiQuantState::Off, 0.0, {}, stream, nullptr, sceneMutex, nullptr, dir.wstring()));
	std::ifstream manifest(dir / "session.jam");
	ASSERT_TRUE(manifest);
	std::stringstream contents;
	contents << manifest.rdbuf();
	manifest.close();
	const auto saved = io::JamFile::FromStream(std::move(contents));
	ASSERT_TRUE(saved);
	ASSERT_EQ(1u, saved->Stations.size());
	ASSERT_EQ(1u, saved->Stations[0].LoopTakes.size());
	const auto& savedTake = saved->Stations[0].LoopTakes[0];
	ASSERT_EQ(1u, savedTake.Loops.size());
	ASSERT_TRUE(savedTake.MidiStreams.empty());
	EXPECT_EQ(101u, savedTake.BaseIntervalSamps);
	EXPECT_EQ(4u, savedTake.BaseDivisions);
	LoopTakeParams restoredParams;
	restoredParams.Id = savedTake.Name;
	restoredParams.Size = { 80u, 80u };
	const auto restored = LoopTake::FromFile(restoredParams, savedTake, dir.wstring());
	ASSERT_TRUE(restored);
	EXPECT_TRUE((*restored)->GetMidiLoops().empty());
	const auto settings = (*restored)->ResolvedMidiQuantisation();
	EXPECT_EQ(101u, settings.BaseIntervalSamps);
	EXPECT_EQ(4u, settings.BaseDivisions);
	std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, SavesLooplessStationConfigurationWithoutTransport)
{
	auto station = IoSessionExporterTest::MakeStation("configured station");
	station->SetAllowedMidiChannels({ 2, 9 });
	station->CommitChanges();
	Quantiser quantiser;
	quantiser.SetClock(std::make_shared<utils::Timer>());
	audio::AudioStreamParams stream{};
	io::UserConfig user{};
	std::mutex sceneMutex;
	const auto dir = IoSessionExporterTest::MakeDirectory();

	ASSERT_TRUE(io::IoSessionExporter::ExportSessionToDirectory({ station }, nullptr, quantiser,
		io::JamFile::GlobalMidiQuantState::Off, 0.0, user, stream, nullptr,
		sceneMutex, nullptr, dir.wstring()));

	std::ifstream manifest(dir / "session.jam");
	ASSERT_TRUE(manifest);
	std::stringstream contents;
	contents << manifest.rdbuf();
	auto jam = io::JamFile::FromStream(std::move(contents));
	ASSERT_TRUE(jam.has_value());
	EXPECT_FALSE(jam->TransportInitialised);
	ASSERT_EQ(1u, jam->Stations.size());
	EXPECT_EQ("configured station", jam->Stations[0].Name);

	manifest.close();
	std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, RoundedLogicalAudioBoundaryRetainsPhysicalTailAfterReload)
{
	auto station = IoSessionExporterTest::MakeStation("retained-tail");
	auto take = IoSessionExporterTest::MakeTake("rounded-take");
	auto loop = IoSessionExporterTest::MakeAudioLoop(101u, 0u, 37u, 0.005f);
	const auto recorded = loop->ExportSamples();
	loop->Play(loop->PlayIndex(), 100u, false);
	take->AddLoop(loop);
	take->CommitChanges();
	station->AddTake(take);
	station->CommitChanges();
	const auto dir = IoSessionExporterTest::MakeDirectory();
	Quantiser quantiser;
	quantiser.SetClock(std::make_shared<utils::Timer>());
	quantiser.ApplyTiming({ 25u, 100ul, 4u, 120.0f, 4u }, "retained-tail fixture");
	std::mutex sceneMutex;
    audio::AudioStreamParams stream{}; stream.SampleRate = 48000u;
	ASSERT_TRUE(io::IoSessionExporter::ExportSessionToDirectory({ station }, {}, quantiser,
		io::JamFile::GlobalMidiQuantState::Off, 0.0, {}, stream, nullptr, sceneMutex, nullptr, dir.wstring()));
	std::ifstream manifest(dir / "session.jam");
	std::stringstream text;
	text << manifest.rdbuf();
	manifest.close();
	const auto saved = io::JamFile::FromStream(std::move(text));
	ASSERT_TRUE(saved);
	ASSERT_EQ(1u, saved->Stations[0].LoopTakes[0].Loops.size());
	const auto restored = Loop::FromFile({}, saved->Stations[0].LoopTakes[0].Loops[0], dir.wstring());
	ASSERT_TRUE(restored);
	EXPECT_EQ(100u, (*restored)->LoopLength());
	EXPECT_EQ(101u, (*restored)->PhysicalLoopLength());
	const auto restoredSamples = (*restored)->ExportSamples(true);
	ASSERT_EQ(recorded.size(), restoredSamples.size());
	for (std::size_t index = 0u; index < recorded.size(); ++index)
		EXPECT_NEAR(recorded[index], restoredSamples[index], 2.0f / 32768.0f); // Existing PCM16 scaling and truncation.
	std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, NumberedVersionsOwnDistinctAssetsAndReloadOriginalMaterial)
{
    auto station = IoSessionExporterTest::MakeStation("versions");
    auto take = IoSessionExporterTest::MakeTake("stable-take-id");
    take->AddLoop(IoSessionExporterTest::MakeAudioLoop(43u, 0u, 13u, 0.01f));
    take->CommitChanges();
    station->AddTake(take);
    station->CommitChanges();
    Quantiser quantiser;
    quantiser.SetClock(std::make_shared<utils::Timer>());
    quantiser.ApplyTiming({ 16u, 128ul, 8u, 120.0f, 4u }, "version fixture");
    audio::AudioStreamParams stream{};
    stream.SampleRate = 48000u;
    stream.NumOutputChannels = 2u;
    std::mutex mutex;
    const auto dir = IoSessionExporterTest::MakeDirectory();
    auto save = [&]() { return io::IoSessionExporter::ExportSessionVersionToDirectory(
        { station }, {}, quantiser, io::JamFile::GlobalMidiQuantState::Off, 0.0,
        {}, stream, nullptr, mutex, nullptr, dir.wstring()); };
    const auto first = save();
    ASSERT_TRUE(first.Success) << first.Error;
    EXPECT_TRUE(first.AudioResumed);
    EXPECT_EQ(L"session-0001.jam", std::filesystem::path(first.Path).filename().wstring());
    const auto second = save();
    ASSERT_TRUE(second.Success) << second.Error;
    EXPECT_EQ(L"session-0002.jam", std::filesystem::path(second.Path).filename().wstring());
    std::ifstream firstFile(first.Path), secondFile(second.Path);
    std::stringstream firstBytes, secondBytes;
    firstBytes << firstFile.rdbuf(); secondBytes << secondFile.rdbuf();
    firstFile.close(); secondFile.close();
    const auto firstJam = io::JamFile::FromStream(std::move(firstBytes));
    const auto secondJam = io::JamFile::FromStream(std::move(secondBytes));
    ASSERT_TRUE(firstJam); ASSERT_TRUE(secondJam);
    const auto& firstLoop = firstJam->Stations[0].LoopTakes[0].Loops[0];
    const auto& secondLoop = secondJam->Stations[0].LoopTakes[0].Loops[0];
    EXPECT_NE(firstLoop.Name, secondLoop.Name);
    EXPECT_EQ("stable-take-id", firstJam->Stations[0].LoopTakes[0].Name);
    const auto restored = Loop::FromFile({}, firstLoop, dir.wstring());
    ASSERT_TRUE(restored);
    EXPECT_EQ(43u, (*restored)->LoopLength());
    EXPECT_EQ(13u, (*restored)->BodyPlayIndex());
    std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, NumberingSkipsGapsAndFailedExportPreservesExistingVersion)
{
    const auto dir = IoSessionExporterTest::MakeDirectory();
    { std::ofstream old(dir / "session-0017.jam"); old << "previous version"; }
    Quantiser quantiser;
    quantiser.SetClock(std::make_shared<utils::Timer>());
    std::mutex mutex;
    const auto failure = io::IoSessionExporter::ExportSessionVersionToDirectory(
        {}, {}, quantiser, io::JamFile::GlobalMidiQuantState::Off, 0.0,
        {}, {}, nullptr, mutex, nullptr, dir.wstring());
    EXPECT_FALSE(failure.Success);
    EXPECT_FALSE(failure.Error.empty());
    EXPECT_EQ(1u, std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}));
    auto station = IoSessionExporterTest::MakeStation("empty");
    auto emptyTake = IoSessionExporterTest::MakeTake("empty-take");
    station->AddTake(emptyTake);
    station->CommitChanges();
    auto routedTake = IoSessionExporterTest::MakeTake("internal-bus-take");
    routedTake->AddLoop(IoSessionExporterTest::MakeAudioLoop(43u, 0u, 0u, 0.01f));
    routedTake->CommitChanges();
    station->AddTake(routedTake);
    station->CommitChanges();
    ASSERT_TRUE(routedTake->RestoreAudioRoutes({ { 3u } }));
    quantiser.ApplyTiming({ 16u, 128ul, 8u, 120.0f, 4u }, "internal bus fixture");
    ASSERT_TRUE(station->RestoreAudioRoutes({ { 0u, 1u }, {} }));
    audio::AudioStreamParams stream{}; stream.NumOutputChannels = 1u; stream.SampleRate = 48000u;
    const auto saved = io::IoSessionExporter::ExportSessionVersionToDirectory(
        { station }, {}, quantiser, io::JamFile::GlobalMidiQuantState::Off, 0.0,
        {}, stream, nullptr, mutex, nullptr, dir.wstring());
    ASSERT_TRUE(saved.Success) << saved.Error;
    EXPECT_EQ(L"session-0018.jam", std::filesystem::path(saved.Path).filename().wstring());
    std::ifstream manifest(saved.Path); std::stringstream bytes; bytes << manifest.rdbuf(); manifest.close();
    const auto jam = io::JamFile::FromStream(std::move(bytes));
    ASSERT_TRUE(jam);
    ASSERT_EQ(2u, jam->Stations[0].LoopTakes.size());
    EXPECT_EQ("empty-take", jam->Stations[0].LoopTakes[0].Name);
    LoopTakeParams emptyParams; emptyParams.Id = "empty-take"; emptyParams.Size = {80u, 80u};
    const auto restoredEmpty = LoopTake::FromFile(emptyParams, jam->Stations[0].LoopTakes[0], dir.wstring());
    ASSERT_TRUE(restoredEmpty.has_value());
    EXPECT_TRUE(restoredEmpty.value()->GetLoops().empty());
    EXPECT_EQ(emptyTake->MidiQuantisation().Enabled, restoredEmpty.value()->MidiQuantisation().Enabled);
    EXPECT_EQ((std::vector<std::vector<unsigned long>>{{3u}}), jam->Stations[0].LoopTakes[1].AudioRoutes);
    EXPECT_TRUE(jam->Stations[0].HasAudioRoutes);
    auto retainedRoutes = station->SnapshotAudioRoutesForExport();
    auto savedRoutes = retainedRoutes; savedRoutes[0] = {0u};
    EXPECT_EQ(savedRoutes, jam->Stations[0].AudioRoutes);
    EXPECT_EQ((std::vector<unsigned long>{0u, 1u}), retainedRoutes[0]);
    std::ifstream previous(dir / "session-0017.jam"); std::string original; std::getline(previous, original); previous.close();
    EXPECT_EQ("previous version", original);
    std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, CancelAndNumberOverflowDoNotCreateAssets)
{
    Quantiser quantiser;
    std::mutex mutex;
    auto station = IoSessionExporterTest::MakeStation("empty");
    const auto cancel = io::IoSessionExporter::ExportSessionVersionToDirectory(
        {station}, {}, quantiser, io::JamFile::GlobalMidiQuantState::Off, 0.0,
        {}, {}, nullptr, mutex, nullptr, L"");
    EXPECT_TRUE(cancel.Cancelled);
    EXPECT_FALSE(cancel.Success);
    const auto dir = IoSessionExporterTest::MakeDirectory();
    { std::ofstream existing(dir / "session-18446744073709551615.jam"); existing << "retained"; }
    const auto overflow = io::IoSessionExporter::ExportSessionVersionToDirectory(
        {station}, {}, quantiser, io::JamFile::GlobalMidiQuantState::Off, 0.0,
        {}, {}, nullptr, mutex, nullptr, dir.wstring());
    EXPECT_FALSE(overflow.Success);
    EXPECT_FALSE(overflow.Error.empty());
    EXPECT_EQ(1u, std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}));
    std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, StoppedHostRemainsStoppedAndResumeFailureIsSeparateFromFileSuccess)
{
    auto station = IoSessionExporterTest::MakeStation("stopped");
    Quantiser quantiser;
    quantiser.SetClock(std::make_shared<utils::Timer>());
    audio::AudioHost host(io::UserConfig{});
    host.CloseCallbackAdmission(); // Model a previously stopped, closed host.
    std::mutex mutex;
    const auto dir = IoSessionExporterTest::MakeDirectory();
    bool restoredBeforeDisk = false;
    const auto saved = io::IoSessionExporter::ExportSessionVersionToDirectory(
        {station}, {}, quantiser, io::JamFile::GlobalMidiQuantState::Off, 0.0,
        {}, {}, &host, mutex, nullptr, dir.wstring(), [&](std::string& error)
        {
            EXPECT_TRUE(host.CallbackAdmissionIsQuiescent());
            EXPECT_EQ(std::filesystem::directory_iterator{}, std::filesystem::directory_iterator(dir));
            // The capture Scene mutex must have been released before restoration.
            const bool unlocked = mutex.try_lock(); EXPECT_TRUE(unlocked);
            if (unlocked) mutex.unlock();
            restoredBeforeDisk = true;
            error = "injected restoration failure";
            return false;
        });
    EXPECT_TRUE(restoredBeforeDisk);
    EXPECT_TRUE(saved.Success) << saved.Error;
    EXPECT_FALSE(saved.AudioResumed);
    EXPECT_NE(std::string::npos, saved.Error.find("injected restoration failure"));
    EXPECT_TRUE(host.CallbackAdmissionIsQuiescent());
    EXPECT_TRUE(std::filesystem::exists(saved.Path));
    std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, ConcurrentManifestClaimFailsWithoutOverwritingOrLeavingAttemptAssets)
{
	auto station = IoSessionExporterTest::MakeStation("collision");
	auto take = IoSessionExporterTest::MakeTake("stable");
	take->AddLoop(IoSessionExporterTest::MakeAudioLoop(43u,0u,0u,0.01f)); take->CommitChanges(); station->AddTake(take); station->CommitChanges();
	Quantiser quantiser; quantiser.SetClock(std::make_shared<utils::Timer>());
	quantiser.ApplyTiming({16u,128ul,8u,120.0f,4u}, "collision fixture");
	std::mutex mutex; audio::AudioStreamParams stream{}; stream.SampleRate=48000u; stream.NumOutputChannels=2u;
	const auto dir = IoSessionExporterTest::MakeDirectory();
	{std::ofstream previous(dir/"session-0001.jam"); previous << "previous";}
	const auto collision = io::IoSessionExporter::ExportSessionVersionToDirectory({station},{},quantiser,
		io::JamFile::GlobalMidiQuantState::Off,0.0,{},stream,nullptr,mutex,nullptr,dir.wstring(),
		[&](std::string&) {std::ofstream claimed(dir/"session-0002.jam"); claimed << "other saver"; return true;});
	EXPECT_FALSE(collision.Success);
	EXPECT_NE(collision.Error.find("commit failed"), std::string::npos);
	EXPECT_EQ(2u, std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}));
	std::ifstream claim(dir/"session-0002.jam"); std::string bytes; std::getline(claim,bytes); claim.close();
	EXPECT_EQ(bytes,"other saver");
	std::filesystem::remove_all(dir);
}
TEST(IoSessionExporter, QuiescentEligibilityFailureRestoresAndWritesNothing)
{
	Quantiser quantiser; std::mutex mutex; const auto dir = IoSessionExporterTest::MakeDirectory();
	auto station = IoSessionExporterTest::MakeStation("blocked");
	audio::AudioHost host(io::UserConfig{}); bool released = false;
	const auto blocked = io::IoSessionExporter::ExportSessionVersionToDirectory({station},{},quantiser,
		io::JamFile::GlobalMidiQuantState::Off,0.0,{},{},&host,mutex,nullptr,dir.wstring(),
		[&](std::string&) {released=true; return true;},
		[&](std::string& error) {EXPECT_TRUE(host.CallbackAdmissionIsQuiescent()); error="capture won admission"; return false;});
	EXPECT_FALSE(blocked.Success); EXPECT_TRUE(released); EXPECT_TRUE(blocked.AudioResumed);
	EXPECT_NE(blocked.Error.find("capture won admission"),std::string::npos);
	EXPECT_EQ(std::filesystem::directory_iterator(dir),std::filesystem::directory_iterator{});
	std::filesystem::remove_all(dir);
}

TEST(IoSessionExporter, PreservesSparseGroupRowsAndRuntimeIdentity)
{
    auto station = IoSessionExporterTest::MakeStation("groups");
    engine::PerformanceGroups groups;
    engine::SelectionGroupSnapshot selection;
    selection.Depth = engine::GroupTargetKind::Station;
    selection.Targets.push_back({ { engine::GroupTargetKind::Station, station->Id(), {}, {} }, true });
    groups.Selection.Store(4u, selection);
    groups.Mute.Store(8u, {});
    const auto jam = IoSessionExporterTest::ExportManifest({ station }, {}, &groups);
    ASSERT_TRUE(jam);
    ASSERT_EQ(jam->Stations.size(), 1u);
    EXPECT_EQ(jam->Stations[0].Id, station->Id());
    ASSERT_TRUE(jam->SelectionGroups[3]);
    EXPECT_FALSE(jam->SelectionGroups[0]);
    EXPECT_EQ(jam->SelectionGroups[3]->Targets[0].Target.StationId, station->Id());
    EXPECT_EQ(jam->SelectionGroups[3]->Depth, engine::GroupTargetKind::Station);
    EXPECT_EQ(jam->SelectionActiveGroup, 3u);
    ASSERT_TRUE(jam->MuteGroups[7]);
    EXPECT_TRUE(jam->MuteGroups[7]->Targets.empty());
    EXPECT_EQ(jam->MuteActiveGroup, 7u);
}
