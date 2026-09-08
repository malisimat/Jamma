#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "base/AudioSink.h"
#include "engine/Loop.h"
#include "engine/LoopTake.h"
#include "engine/Quantiser.h"
#include "engine/Station.h"
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
		unsigned long bodyPlayIndex)
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
			samples[constants::MaxLoopFadeSamps + sample] = static_cast<float>(sample + 1ul);
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
};

TEST(IoSessionExporter, ExplicitDirectoryRoundTripsLocalManifestAndSidecars)
{
	auto firstStation = IoSessionExporterTest::MakeStation("first");
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
	quantiser.Set(16u, utils::Timer::QUANTISE_MULTIPLE);
	timer->SetSeedSourceLength(128u);
	ASSERT_TRUE(timer->InitialiseAbsoluteSamplePos((9ull * 128ull) + 17ull));

	audio::AudioStreamParams stream{};
	stream.SampleRate = 48000u;
	io::UserConfig user;
	std::mutex sceneMutex;
	const auto dir = IoSessionExporterTest::MakeDirectory();
	ASSERT_TRUE(io::IoSessionExporter::ExportSessionToDirectory({ firstStation, secondStation },
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
	EXPECT_EQ(2u, jam->Stations.size());
	ASSERT_EQ(1u, jam->Stations[0].LoopTakes.size());
	EXPECT_EQ(std::vector<int>({ 1, 3, 16 }), jam->Stations[0].AllowedMidiChannels);
	const auto& savedTake = jam->Stations[0].LoopTakes[0];
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
