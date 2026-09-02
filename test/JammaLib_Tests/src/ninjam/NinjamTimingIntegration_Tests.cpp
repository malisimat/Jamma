#include "gtest/gtest.h"

#include <atomic>
#include <array>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/AudioHost.h"
#include "base/AudioSink.h"
#include "engine/Loop.h"
#include "engine/LoopTake.h"
#include "engine/Station.h"
#include "ninjam/NinjamAudioTimingCommand.h"
#include "ninjam/NinjamNetworkService.h"
#include "ninjam/NinjamSession.h"
#include "io/UserConfig.h"
#include "utils/Timer.h"

// Complete desired-state production-boundary coverage using real audio, MIDI,
// automation, Timer, Station, LoopTake, and Loop behavior.

using utils::Timer;

namespace audio
{
	class NinjamAudioBoundaryTestAccess
	{
	public:
		static bool Apply(AudioHost& host, std::uint64_t blockStartSample,
			unsigned int sampleRate) noexcept
		{
			return host.ApplyDesiredTimingAtAudioBoundary(blockStartSample, sampleRate);
		}
	};
}

class NinjamProductionBoundaryLoopTake : public engine::LoopTake
{
public:
	NinjamProductionBoundaryLoopTake(engine::LoopTakeParams params,
		audio::AudioMixerParams mixerParams) : LoopTake(params, mixerParams)
	{
	}

	void SetMidiTimingState(unsigned long position, unsigned long length) noexcept
	{
		_midiVisualPlayIndex.store(position, std::memory_order_relaxed);
		_midiVisualLoopLength.store(length, std::memory_order_relaxed);
	}

	unsigned long MidiTimingPosition() const noexcept
	{
		return _midiVisualPlayIndex.load(std::memory_order_relaxed);
	}
};

class NinjamProductionBoundaryFixture
{
public:
	static std::shared_ptr<engine::Loop> MakeLoop(unsigned long loopLength,
		unsigned long position)
	{
		audio::WireMixBehaviourParams mixBehaviour;
		mixBehaviour.Channels = { 0u };
		audio::AudioMixerParams mixerParams;
		mixerParams.Size = { 160, 320 };
		mixerParams.Position = { 6, 6 };
		mixerParams.Behaviour = mixBehaviour;

		engine::LoopParams loopParams;
		loopParams.Wav = "desired-state-phase-test";
		loopParams.Size = { 80, 80 };
		loopParams.Position = { 10, 22 };
		auto loop = std::make_shared<engine::Loop>(loopParams, mixerParams);

		loop->Record();
		const auto recordedLength = constants::MaxLoopFadeSamps + loopLength;
		std::vector<float> samples(recordedLength, 1.0f);
		base::AudioWriteRequest request;
		request.samples = samples.data();
		request.numSamps = static_cast<unsigned int>(recordedLength);
		request.stride = 1u;
		request.fadeCurrent = 0.0f;
		request.fadeNew = 1.0f;
		request.source = base::Audible::AUDIOSOURCE_ADC;
		loop->OnBlockWrite(request, 0);
		loop->EndWrite(static_cast<unsigned int>(recordedLength), true);
		loop->Play(constants::MaxLoopFadeSamps, loopLength, false);
		loop->ShiftPlayIndex(static_cast<long long>(position));
		return loop;
	}

	static std::shared_ptr<NinjamProductionBoundaryLoopTake> MakeTake(
		const std::string& id, unsigned long loopLength, unsigned long position)
	{
		engine::LoopTakeParams params;
		params.Id = id;
		params.Size = { 100, 100 };
		audio::MergeMixBehaviourParams merge;
		auto mixerParams = engine::LoopTake::GetMixerParams(params.Size, merge);
		auto take = std::make_shared<NinjamProductionBoundaryLoopTake>(params, mixerParams);
		take->AddLoop(MakeLoop(loopLength, position));
		take->CommitChanges();
		take->SetMidiTimingState(position % loopLength, loopLength);
		return take;
	}

	static std::shared_ptr<engine::Station> MakeStation(
		const std::vector<std::shared_ptr<NinjamProductionBoundaryLoopTake>>& takes)
	{
		engine::StationParams params;
		params.Name = "desired-state-station";
		params.Size = { 200, 320 };
		audio::MergeMixBehaviourParams merge;
		auto station = std::make_shared<engine::Station>(params,
			engine::Station::GetMixerParams(params.Size, merge));
		for (const auto& take : takes)
			station->AddTake(take);
		station->CommitChanges();
		return station;
	}

	static unsigned long AudioPosition(const NinjamProductionBoundaryLoopTake& take)
	{
		return take.GetLoops().front()->BodyPlayIndex();
	}

	static ninjam::NinjamDesiredTransportState Desired(std::uint64_t epoch,
		std::uint64_t version, std::uint64_t generation,
		ninjam::NinjamLocalFollowPolicy policy, unsigned long length,
		unsigned int phase, std::uint64_t observationSample = 0u,
		ninjam::NinjamDesiredTimingIntent intent = ninjam::NinjamDesiredTimingIntent::Replacement)
	{
		ninjam::NinjamDesiredTransportState desired;
		desired.SessionEpoch = epoch;
		desired.Version = version;
		desired.Generation = generation;
		desired.Intent = policy == ninjam::NinjamLocalFollowPolicy::NoSync
			? ninjam::NinjamDesiredTimingIntent::NoSync : intent;
		desired.LocalFollowPolicy = policy;
		desired.HasRemoteTiming = policy != ninjam::NinjamLocalFollowPolicy::NoSync;
		desired.IntervalLengthSamps = length;
		desired.GrainSamps = length == 0ul ? 0u : static_cast<unsigned int>(length / 16ul);
		desired.BeatsPerInterval = 16u;
		desired.TempoBpm = 120.0f;
		desired.Quantisation = Timer::QUANTISE_POWER;
		desired.RemotePhaseSamps = phase;
		desired.HasObservationSample = true;
		desired.ObservationSample = observationSample;
		return desired;
	}

	static ninjam::NinjamTiming Timing(unsigned int length, unsigned int phase,
		float bpm, unsigned int bpi, std::uint64_t observationSample)
	{
		ninjam::NinjamTiming timing;
		timing.IsConnected = true;
		timing.IsValid = true;
		timing.DeviceSampleRate = 48000u;
		timing.SourceSampleRate = 48000u;
		timing.IntervalLengthSamps = length;
		timing.IntervalPositionSamps = phase;
		timing.Bpm = bpm;
		timing.Bpi = bpi;
		timing.HasAudioBlockStartSample = true;
		timing.AudioBlockStartSample = observationSample;
		timing.HasLocalTransport = true;
		timing.LocalTransport.MasterLengthSamps = length;
		timing.LocalTransport.MasterPhaseSamps = phase;
		timing.LocalTransport.AbsoluteSamplePos = observationSample;
		timing.LocalTransport.SceneSamplePos = observationSample;
		timing.LocalBlockStartSample = observationSample;
		return timing;
	}
};

TEST(NinjamTimingProductionBoundary, CompleteDesiredStateSupersedesFormerCommandSequences)
{
	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(1000ul);
	clock->Tick(100u, 0u);
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>());

	// The old Invalidate -> Replace -> Discipline history is represented by one
	// self-sufficient latest value. Only the final value reaches the boundary.
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 1u, 0u,
		ninjam::NinjamLocalFollowPolicy::NoSync, 0ul, 0u));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 2u, 1u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, 1000ul, 200u));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 3u, 2u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, 1000ul, 350u, 0u,
		ninjam::NinjamDesiredTimingIntent::PhaseDiscipline));
	EXPECT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	EXPECT_EQ(1000ul, clock->SeedSourceLength());
	EXPECT_EQ(350u, clock->SampOffset());
	auto receipt = host.LastAppliedDesiredTiming();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(1u, receipt->SessionEpoch);
	EXPECT_EQ(3u, receipt->Version);

	// A complete geometry-change value also subsumes a preceding replacement
	// publication and cannot be interpreted against the old 1000-sample geometry.
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 4u, 3u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, 2000ul, 500u));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 5u, 4u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, 2000ul, 750u));
	EXPECT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	EXPECT_EQ(2000ul, clock->SeedSourceLength());
	EXPECT_EQ(750u, clock->SampOffset());
	receipt = host.LastAppliedDesiredTiming();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(5u, receipt->Version);
	const auto phaseBeforeDuplicate = clock->SampOffset();
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 6u, 4u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, 2000ul, 750u, 0u,
		ninjam::NinjamDesiredTimingIntent::PhaseDiscipline));
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	EXPECT_EQ(phaseBeforeDuplicate, clock->SampOffset());
}

TEST(NinjamTimingProductionBoundary, OverlappingIntentsPublishOneCoherentDesiredVersion)
{
	ninjam::NinjamNetworkService service;
	ninjam::NinjamTempoJoinOptions options;
	options.PushLocalTempoOnJoin = false;
	options.PromptBeforeApplyingRemoteTempo = false;
	service.SetTempoJoinOptions(options);
	service.PrepareTempoSyncOnConnect(std::nullopt);
	ninjam::NinjamSessionTimingStatus available;
	available.IsAvailable = true;
	available.Changed = true;
	available.SessionEpoch = 9u;
	service.ObserveSessionStatus(available, std::nullopt);

	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>());
	std::array<ninjam::NinjamTimingUpdate, 2u> updates;
	std::array<Timer, 2u> producerClocks;
	const std::array<ninjam::NinjamTiming, 2u> intents{
		NinjamProductionBoundaryFixture::Timing(1000u, 111u, 120.0f, 16u, 10000u),
		NinjamProductionBoundaryFixture::Timing(2000u, 777u, 90.0f, 8u, 20000u) };
	std::atomic_uint ready = 0u;
	std::atomic_bool start = false;
	const auto runIntent = [&](std::size_t index)
		{
			ready.fetch_add(1u, std::memory_order_release);
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			updates[index] = service.ObserveTiming(intents[index], std::nullopt, false,
				io::UserConfig{}, producerClocks[index]);
		};
	std::thread job(runIntent, 0u);
	std::thread ui(runIntent, 1u);
	while (ready.load(std::memory_order_acquire) != 2u)
		std::this_thread::yield();
	start.store(true, std::memory_order_release);
	job.join();
	ui.join();
	ASSERT_TRUE(updates[0].DesiredTransport.has_value());
	ASSERT_TRUE(updates[1].DesiredTransport.has_value());
	EXPECT_NE(updates[0].DesiredTransport->Version, updates[1].DesiredTransport->Version);
	const auto firstIsNewer = updates[0].DesiredTransport->Version
		> updates[1].DesiredTransport->Version;
	const auto& newer = firstIsNewer ? updates[0].DesiredTransport.value()
		: updates[1].DesiredTransport.value();
	const auto& older = firstIsNewer ? updates[1].DesiredTransport.value()
		: updates[0].DesiredTransport.value();
	EXPECT_EQ(older.Version + 1u, newer.Version);
	const auto coherentA = newer.IntervalLengthSamps == 1000ul
		&& newer.RemotePhaseSamps == 111u && newer.BeatsPerInterval == 16u;
	const auto coherentB = newer.IntervalLengthSamps == 2000ul
		&& newer.RemotePhaseSamps == 777u && newer.BeatsPerInterval == 8u;
	EXPECT_TRUE(coherentA || coherentB);

	host.PublishDesiredTiming(newer);
	host.PublishDesiredTiming(older);
	EXPECT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host,
		newer.ObservationSample, 48000u));
	const auto receipt = host.LastAppliedDesiredTiming();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(9u, receipt->SessionEpoch);
	EXPECT_EQ(newer.Version, receipt->Version);
	EXPECT_EQ(newer.Generation, receipt->Generation);
	EXPECT_EQ(newer.IntervalLengthSamps, clock->SeedSourceLength());
	EXPECT_EQ(newer.RemotePhaseSamps, clock->SampOffset());
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::Apply(host,
		newer.ObservationSample, 48000u));
}

TEST(NinjamTimingProductionBoundary, ReconnectPreservesM2M3MEntityOffsetsAcrossEpochOne)
{
	constexpr unsigned long masterLength = 1000ul;
	auto takeM = NinjamProductionBoundaryFixture::MakeTake("epoch-m", masterLength, 100ul);
	auto take2M = NinjamProductionBoundaryFixture::MakeTake("epoch-2m", masterLength * 2ul, 1300ul);
	auto take3M = NinjamProductionBoundaryFixture::MakeTake("epoch-3m", masterLength * 3ul, 2600ul);
	const std::vector<std::shared_ptr<NinjamProductionBoundaryLoopTake>> takes{
		takeM, take2M, take3M };
	auto station = NinjamProductionBoundaryFixture::MakeStation(takes);

	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(masterLength);
	clock->Tick(100u, 0u);
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station }));

	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 8u, 8u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 100u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	const auto beforeNoSyncAudio = std::vector<unsigned long>{
		NinjamProductionBoundaryFixture::AudioPosition(*takeM),
		NinjamProductionBoundaryFixture::AudioPosition(*take2M),
		NinjamProductionBoundaryFixture::AudioPosition(*take3M) };
	const auto beforeNoSyncMidi = std::vector<unsigned long>{ takeM->MidiTimingPosition(),
		take2M->MidiTimingPosition(), take3M->MidiTimingPosition() };
	const auto beforeNoSyncAutomation = std::vector<long long>{ takeM->MidiAnchorCorrection(),
		take2M->MidiAnchorCorrection(), take3M->MidiAnchorCorrection() };

	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 9u, 0u,
		ninjam::NinjamLocalFollowPolicy::NoSync, 0ul, 0u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	for (std::size_t i = 0u; i < takes.size(); ++i)
	{
		EXPECT_EQ(beforeNoSyncAudio[i], NinjamProductionBoundaryFixture::AudioPosition(*takes[i]));
		EXPECT_EQ(beforeNoSyncMidi[i], takes[i]->MidiTimingPosition());
		EXPECT_EQ(beforeNoSyncAutomation[i], takes[i]->MidiAnchorCorrection());
	}

	clock->Tick(137u, 0u);
	for (const auto& take : takes)
		take->EndMultiPlay(137u);
	const auto afterFreeRunAudio = std::vector<unsigned long>{
		NinjamProductionBoundaryFixture::AudioPosition(*takeM),
		NinjamProductionBoundaryFixture::AudioPosition(*take2M),
		NinjamProductionBoundaryFixture::AudioPosition(*take3M) };
	const auto afterFreeRunMidi = std::vector<unsigned long>{ takeM->MidiTimingPosition(),
		take2M->MidiTimingPosition(), take3M->MidiTimingPosition() };

	// Session 2 deliberately restarts generation at one. Epoch authority resets
	// every gate after real free-run advancement and applies one shared +250.
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(2u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 350u, 0u,
		ninjam::NinjamDesiredTimingIntent::JoinAlignment));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 137u, 48000u));
	EXPECT_EQ(487u, clock->SampOffset());
	for (std::size_t i = 0u; i < takes.size(); ++i)
	{
		EXPECT_EQ((afterFreeRunAudio[i] + 250ul) % (masterLength * (i + 1u)),
			NinjamProductionBoundaryFixture::AudioPosition(*takes[i]));
		EXPECT_EQ((afterFreeRunMidi[i] + 250ul) % (masterLength * (i + 1u)),
			takes[i]->MidiTimingPosition());
		EXPECT_EQ(beforeNoSyncAutomation[i] - 250, takes[i]->MidiAnchorCorrection());
	}

	const auto acceptedAudio = NinjamProductionBoundaryFixture::AudioPosition(*take2M);
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 99u, 99u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 800u));
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 137u, 48000u));
	EXPECT_EQ(acceptedAudio, NinjamProductionBoundaryFixture::AudioPosition(*take2M));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(2u, 2u, 1u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 900u));
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 137u, 48000u));
	EXPECT_EQ(acceptedAudio, NinjamProductionBoundaryFixture::AudioPosition(*take2M));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(2u, 3u, 0u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, masterLength, 950u));
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 137u, 48000u));
	EXPECT_EQ(acceptedAudio, NinjamProductionBoundaryFixture::AudioPosition(*take2M));
}

