#include "gtest/gtest.h"

#include <algorithm>
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
#include "engine/LocalTiming.h"
#include "engine/Station.h"
#include "midi/MidiLoop.h"
#include "ninjam/NinjamAudioTimingCommand.h"
#include "ninjam/NinjamMetronomeTiming.h"
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
		static void ConfigureMetronome(AudioHost& host) { host._ninjamMetronome.Configure(48000u); }
		static void Click(AudioHost& host, float* output, unsigned int frames, const ninjam::NinjamTiming& remote, const Timer* clock)
		{ host.MixMetronome(output, 1u, frames, 48000u, remote, clock); }
		static bool Apply(AudioHost& host, std::uint64_t blockStartSample,
			unsigned int sampleRate) noexcept
		{
			return host.ApplyDesiredTimingAtAudioBoundary(blockStartSample, sampleRate);
		}

		static void ApplyLocalMaster(AudioHost& host,
			const std::vector<std::shared_ptr<engine::Station>>& stations, Timer* clock) noexcept
		{
			host.ApplyLocalMasterTimingAtAudioBoundary(stations, clock);
		}

		static void ApplyDeferredMapTransition(AudioHost& host) noexcept
		{
			const auto stations = host._audioStations.load(std::memory_order_acquire);
			if (!stations)
				return;
			host.CaptureMappedSourceAnchorsAfterOffset(*stations);
		}

		static void RestoreMappedSource(AudioHost& host,
			std::uint64_t sceneCoordinateSamps) noexcept
		{
			const auto stations = host._audioStations.load(std::memory_order_acquire);
			if (stations)
				host.RestoreMappedSourceAtScene(stations.get(), sceneCoordinateSamps);
		}

		static ninjam::SyncPhaseMap SyncPhaseMap(
			const AudioHost& host) noexcept
		{
			return host._syncPhaseMap;
		}

		static std::optional<double> ConsumeLocalTransportOffset(AudioHost& host) noexcept
		{
			return host._localTransportOffsetLoopFracMailbox.ConsumeLatest();
		}

		static void ApplyLocalTransportOffset(AudioHost& host,
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			Timer* timingClock = nullptr) noexcept
		{
			host.ApplyLocalTransportOffsetAtAudioBoundary(stations, timingClock);
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

	unsigned long MidiSceneAnchor() const noexcept
	{
		return _midiSceneAnchor.load(std::memory_order_relaxed);
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
		take->Record({}, id, { 0u });
		take->EndMultiWrite(static_cast<unsigned int>(loopLength), true,
			base::Audible::AUDIOSOURCE_ADC);
		take->Play(position, loopLength, 0u);
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
		desired.RemoteMasterIntervalLengthSamps = length;
		desired.RemoteGridStepSamps = length == 0ul ? 0u : static_cast<unsigned int>(length / 16ul);
		desired.BeatsPerInterval = 16u;
		desired.TempoBpm = 120.0f;
		desired.Quantisation = Timer::QUANTISE_POWER;
		desired.RemoteMasterPhaseSamps = phase;
		desired.HasRemotePhaseDeviceSample = true;
		desired.RemotePhaseDeviceSample = observationSample;
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
		timing.HasDeviceAudioSampleAtObservation = true;
		timing.DeviceAudioSampleAtObservation = observationSample;
		timing.HasLocalTransport = true;
		timing.LocalTransport.MasterLengthSamps = length;
		timing.LocalTransport.MasterPhaseSamps = phase;
		timing.LocalTransport.AbsoluteSamplePos = observationSample;
		timing.LocalTransport.SceneSamplePos = observationSample;
		timing.LocalMasterAbsoluteSampleAtObservation = observationSample;
		return timing;
	}
};

TEST(AudioHostLocalTransportOffsetPublication, LatestPublicationWinsAndZeroIsDeliveredOnce)
{
	audio::AudioHost host{ io::UserConfig{} };
	host.PublishLocalTransportOffsetLoopFrac(0.005);
	host.PublishLocalTransportOffsetLoopFrac(0.010);
	host.PublishLocalTransportOffsetLoopFrac(0.0);

	const auto consumed = audio::NinjamAudioBoundaryTestAccess::ConsumeLocalTransportOffset(host);
	ASSERT_TRUE(consumed.has_value());
	EXPECT_DOUBLE_EQ(0.0, consumed.value());
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::ConsumeLocalTransportOffset(host).has_value());
}

TEST(AudioHostLocalTransportOffsetPublication, AppliesWhileDisconnectedAndNoSyncIsActive)
{
	constexpr unsigned long masterLength = 1000ul;
	auto take = NinjamProductionBoundaryFixture::MakeTake("local-offset", masterLength * 2ul, 100ul);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	const std::vector<std::shared_ptr<engine::Station>> stations{ station };

	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(masterLength);
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(stations));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 1u, 0u,
		ninjam::NinjamLocalFollowPolicy::NoSync, 0ul, 0u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));

	host.PublishLocalTransportOffsetLoopFrac(0.25);
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalTransportOffset(host, stations, clock.get());
	EXPECT_EQ(350ul, NinjamProductionBoundaryFixture::AudioPosition(*take));
	EXPECT_EQ(350ul, take->MidiTimingPosition());
}

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
	for (std::size_t i = 0u; i < updates.size(); ++i)
	{
		ASSERT_TRUE(updates[i].RemoteGrid.has_value());
		const auto& desired = updates[i].DesiredTransport.value();
		const auto& grid = updates[i].RemoteGrid.value();
		EXPECT_EQ(desired.RemoteMasterIntervalLengthSamps, grid.Geometry.IntervalLengthSamps);
		EXPECT_EQ(desired.BeatsPerInterval, grid.Geometry.Bpi);
		EXPECT_EQ(desired.RemoteMasterPhaseSamps, grid.Geometry.PhaseSamps);
		EXPECT_EQ(desired.TempoBpm, grid.Geometry.Bpm);
		EXPECT_EQ(desired.Generation, grid.Geometry.Generation);
		EXPECT_EQ(0, grid.OriginSamps);
		EXPECT_FALSE(updates[i].TempoRequest.has_value());
		EXPECT_FALSE(updates[i].PromptForTempoChange);
	}
	EXPECT_NE(updates[0].DesiredTransport->Version, updates[1].DesiredTransport->Version);
	const auto firstIsNewer = updates[0].DesiredTransport->Version
		> updates[1].DesiredTransport->Version;
	const auto& newer = firstIsNewer ? updates[0].DesiredTransport.value()
		: updates[1].DesiredTransport.value();
	const auto& older = firstIsNewer ? updates[1].DesiredTransport.value()
		: updates[0].DesiredTransport.value();
	EXPECT_EQ(older.Version + 1u, newer.Version);
	const auto coherentA = newer.RemoteMasterIntervalLengthSamps == 1000ul
		&& newer.RemoteMasterPhaseSamps == 111u && newer.BeatsPerInterval == 16u;
	const auto coherentB = newer.RemoteMasterIntervalLengthSamps == 2000ul
		&& newer.RemoteMasterPhaseSamps == 777u && newer.BeatsPerInterval == 8u;
	EXPECT_TRUE(coherentA || coherentB);

	host.PublishDesiredTiming(newer);
	host.PublishDesiredTiming(older);
	EXPECT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host,
		newer.RemotePhaseDeviceSample, 48000u));
	const auto receipt = host.LastAppliedDesiredTiming();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(9u, receipt->SessionEpoch);
	EXPECT_EQ(newer.Version, receipt->Version);
	EXPECT_EQ(newer.Generation, receipt->Generation);
	EXPECT_EQ(newer.RemoteMasterIntervalLengthSamps, clock->SeedSourceLength());
	EXPECT_EQ(newer.RemoteMasterPhaseSamps, clock->SampOffset());
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::Apply(host,
		newer.RemotePhaseDeviceSample, 48000u));
}

TEST(NinjamTimingProductionBoundary, ReconnectPreservesM2M3MEntityOffsetsAcrossEpochOne)
{
	constexpr unsigned long masterLength = 1000ul;
	auto takeM = NinjamProductionBoundaryFixture::MakeTake("epoch-m", masterLength, 100ul);
	auto take2M = NinjamProductionBoundaryFixture::MakeTake("epoch-2m", masterLength * 2ul, 1300ul);
	auto take3M = NinjamProductionBoundaryFixture::MakeTake("epoch-3m", masterLength * 3ul, 2600ul);
	const std::vector<std::shared_ptr<NinjamProductionBoundaryLoopTake>> takes{
		takeM, take2M, take3M };
	for (const auto& take : takes)
		ASSERT_EQ(1u, take->GetMidiLoops().size());
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

TEST(NinjamTimingProductionBoundary, RestoreBeforeRebasePreservesEntityAnchors)
{
	constexpr unsigned long localMasterLength = 1000ul;
	constexpr unsigned long remoteMasterLength = 1100ul;
	constexpr unsigned long oddLength = 777ul;
	auto takeM = NinjamProductionBoundaryFixture::MakeTake(
		"restore-rebase-m", localMasterLength, 100ul);
	auto take2M = NinjamProductionBoundaryFixture::MakeTake(
		"restore-rebase-2m", localMasterLength * 2ul, 1300ul);
	auto takeOdd = NinjamProductionBoundaryFixture::MakeTake(
		"restore-rebase-odd", oddLength, 700ul);
	const std::vector<std::shared_ptr<NinjamProductionBoundaryLoopTake>> takes{
		takeM, take2M, takeOdd };
	auto station = NinjamProductionBoundaryFixture::MakeStation(takes);

	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(localMasterLength);
	clock->Tick(100u, 0u);
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station }));

	// Establish the unequal remote ruler. The production callback performs this
	// deferred capture after any persistent local offset has been applied.
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, remoteMasterLength, 100u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	const std::array<unsigned long, 3u> audioAnchors{
		takeM->GetLoops().front()->SceneAnchor(),
		take2M->GetLoops().front()->SceneAnchor(),
		takeOdd->GetLoops().front()->SceneAnchor() };
	const std::array<unsigned long, 3u> midiAnchors{
		takeM->MidiSceneAnchor(), take2M->MidiSceneAnchor(), takeOdd->MidiSceneAnchor() };

	// Device-rate advancement is deliberately fifty samples ahead of the mapped
	// local ruler. The next accepted boundary must restore that residue before it
	// rebases the common map and applies the new remote phase.
	clock->Tick(550u, 0u);
	for (const auto& take : takes)
		take->EndMultiPlay(550u);
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 2u, 2u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, remoteMasterLength, 700u, 550u,
		ninjam::NinjamDesiredTimingIntent::PhaseDiscipline));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 550u, 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	EXPECT_EQ(audioAnchors[0], takeM->GetLoops().front()->SceneAnchor());
	EXPECT_EQ(audioAnchors[1], take2M->GetLoops().front()->SceneAnchor());
	EXPECT_EQ(audioAnchors[2], takeOdd->GetLoops().front()->SceneAnchor());
	EXPECT_EQ(midiAnchors[0], takeM->MidiSceneAnchor());
	EXPECT_EQ(midiAnchors[1], take2M->MidiSceneAnchor());
	EXPECT_EQ(midiAnchors[2], takeOdd->MidiSceneAnchor());

	EXPECT_EQ(636ul, NinjamProductionBoundaryFixture::AudioPosition(*takeM));
	EXPECT_EQ(1836ul, NinjamProductionBoundaryFixture::AudioPosition(*take2M));
	EXPECT_EQ(459ul, NinjamProductionBoundaryFixture::AudioPosition(*takeOdd));
	EXPECT_EQ(636ul, takeM->MidiTimingPosition());
	EXPECT_EQ(1836ul, take2M->MidiTimingPosition());
	EXPECT_EQ(459ul, takeOdd->MidiTimingPosition());

	// A later full remote interval maps to one local interval. Every entity keeps
	// its original offset and wraps by its own M, 2M, or non-divisor length.
	clock->Tick(remoteMasterLength, 0u);
	for (const auto& take : takes)
		take->EndMultiPlay(remoteMasterLength);
	audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
	EXPECT_EQ(636ul, NinjamProductionBoundaryFixture::AudioPosition(*takeM));
	EXPECT_EQ(836ul, NinjamProductionBoundaryFixture::AudioPosition(*take2M));
	EXPECT_EQ(682ul, NinjamProductionBoundaryFixture::AudioPosition(*takeOdd));
	EXPECT_EQ(636ul, takeM->MidiTimingPosition());
	EXPECT_EQ(836ul, take2M->MidiTimingPosition());
	EXPECT_EQ(682ul, takeOdd->MidiTimingPosition());
}

TEST(NinjamTimingProductionBoundary, CommonMapRestoreFansOutAcrossStationsAndUnequalEntities)
{
	constexpr unsigned long localMasterLength = 1000ul;
	constexpr unsigned long remoteMasterLength = 1100ul;
	auto takeM = NinjamProductionBoundaryFixture::MakeTake("fan-out-m", localMasterLength, 100ul);
	auto take2M = NinjamProductionBoundaryFixture::MakeTake("fan-out-2m", localMasterLength * 2ul, 1300ul);
	auto takeOdd = NinjamProductionBoundaryFixture::MakeTake("fan-out-odd", 777ul, 700ul);
	auto takeSecondStation = NinjamProductionBoundaryFixture::MakeTake(
		"fan-out-second-station", localMasterLength, 450ul);
	auto firstStation = NinjamProductionBoundaryFixture::MakeStation({ takeM, take2M });
	auto secondStation = NinjamProductionBoundaryFixture::MakeStation(
		{ takeOdd, takeSecondStation });
	const std::vector<std::shared_ptr<NinjamProductionBoundaryLoopTake>> takes{
		takeM, take2M, takeOdd, takeSecondStation };
	const std::vector<unsigned long> lengths{ localMasterLength, localMasterLength * 2ul,
		777ul, localMasterLength };

	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(localMasterLength);
	clock->Tick(100u, 0u);
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ firstStation, secondStation }));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, remoteMasterLength, 100u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	const auto map = audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host);
	ASSERT_TRUE(map.IsActive());
	const auto receipt = host.LastAppliedDesiredTiming();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(map.SourceLengthSamps, receipt->SourceLengthSamps);
	EXPECT_EQ(map.RemoteLengthSamps, receipt->RemoteLengthSamps);
	EXPECT_EQ(map.SourceCoordinateAtOrigin, receipt->SourceCoordinateSamps);

	clock->Tick(remoteMasterLength, 0u);
	for (const auto& take : takes)
		take->EndMultiPlay(remoteMasterLength);
	const auto sceneCoordinate = clock->SceneSamplePos();
	const auto sourceCoordinate = map.SourceCoordinateAt(sceneCoordinate);
	audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, sceneCoordinate);

	for (std::size_t index = 0u; index < takes.size(); ++index)
	{
		const auto& take = takes[index];
		const auto length = lengths[index];
		EXPECT_EQ(static_cast<unsigned long>(ninjam::PositiveModulo(sourceCoordinate
			- static_cast<long long>(take->GetLoops().front()->SceneAnchor()), length)),
			NinjamProductionBoundaryFixture::AudioPosition(*take));
		EXPECT_EQ(static_cast<unsigned long>(ninjam::PositiveModulo(sourceCoordinate
			- static_cast<long long>(take->MidiSceneAnchor()), length)),
			take->MidiTimingPosition());
	}
}

TEST(NinjamTimingProductionBoundary, StartupPlaceholderCannotSlowFirstMidiOnlyRecording)
{
	constexpr auto sampleRate = 44100u;
	constexpr auto remoteInterval = 313600ul;
	constexpr auto takeLength = remoteInterval / 2ul;
	constexpr auto blockSize = 512u;
	auto clock = std::make_shared<Timer>();
	clock->Tick(4096u, 0u);
	auto station = NinjamProductionBoundaryFixture::MakeStation({});
	station->SetClock(clock);
	audio::AudioHost host{ io::UserConfig{} };
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station }));
	ninjam::NinjamTimingCoordinator coordinator;
	ninjam::NinjamTempoJoinOptions options;
	const auto connected = coordinator.ObserveSessionStatus({ true, true, 1u }, options, std::nullopt);
	ASSERT_TRUE(connected.DesiredTransport);
	host.PublishDesiredTiming(*connected.DesiredTransport);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), sampleRate));

	// Apply exactly the NJClient observation sequence from fail-logs-1.txt.
	// Use the adapter's shared validity boundary before conversion/publication.
	ninjam::NinjamRemoteTiming remote;
	remote.IsConnected = true;
	remote.IntervalLengthSamps = 1000u;
	remote.SourceSampleRate = sampleRate;
	remote.Bpm = 120.0f;
	remote.Bpi = 32u;
	remote.HasDeviceAudioSampleAtObservation = true;
	remote.DeviceAudioSampleAtObservation = clock->SceneSamplePos();
	const auto observe = [&](bool hasLocalContent)
	{
		remote.IsValid = ninjam::IsValidRemoteTiming(remote.IntervalLengthSamps,
			remote.SourceSampleRate, remote.Bpm, remote.Bpi);
		auto timing = ninjam::ToDeviceTiming(remote, true, sampleRate, 0u, 0ul, 0u);
		const auto local = clock->ObserveTransport();
		timing.HasLocalTransport = true;
		timing.LocalTransport = { local.MasterLengthSamps, local.MasterPhaseSamps,
			local.LoopCount, local.AbsoluteSamplePos, local.SceneSamplePos };
		return coordinator.Observe(timing, std::nullopt, hasLocalContent, io::UserConfig{}, *clock,
			std::chrono::steady_clock::time_point{} + std::chrono::microseconds(
				remote.DeviceAudioSampleAtObservation * 1000000u / sampleRate));
	};
	const auto apply = [&](const ninjam::NinjamTimingUpdate& update)
	{
		if (update.DesiredTransport)
		{
			host.PublishDesiredTiming(*update.DesiredTransport);
			EXPECT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), sampleRate));
			audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
		}
	};
	const auto placeholder = observe(false);
	EXPECT_FALSE(placeholder.DesiredTransport);
	apply(placeholder);
	EXPECT_EQ(0ul, clock->SeedSourceLength());
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host).IsActive());

	remote.IntervalLengthSamps = remoteInterval;
	remote.IntervalPositionSamps = 3072u;
	remote.Bpm = 135.0f;
	remote.Bpi = 16u;
	const auto accepted = observe(false);
	ASSERT_TRUE(accepted.DesiredTransport);
	EXPECT_FALSE(accepted.PromptForTempoChange);
	EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::ContinuousSync, accepted.DesiredTransport->LocalFollowPolicy);
	apply(accepted);
	const auto map = audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host);
	ASSERT_EQ(remoteInterval, map.SourceLengthSamps);
	ASSERT_EQ(remoteInterval, map.RemoteLengthSamps);

	// Real captured MIDI material, with no audio loop or synthetic cursor setup.
	engine::LoopTakeParams params;
	params.Id = "first-remote-midi-take";
	params.Size = { 100, 100 };
	audio::MergeMixBehaviourParams merge;
	auto take = std::make_shared<engine::LoopTake>(params, engine::LoopTake::GetMixerParams(params.Size, merge));
	const auto recordStart = static_cast<std::uint32_t>(clock->SceneSamplePos());
	take->Record({}, station->Name(), { 0u }, { "Keys" }, {}, clock->AbsoluteSamplePos(), recordStart);
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(recordStart + 24u, 0u, 60u, 100u),
		"Keys", recordStart + 24u));
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(recordStart + 19600u, 0u, 60u),
		"Keys", recordStart + 19600u));
	take->EndMultiWrite(takeLength, true, base::Audible::AUDIOSOURCE_ADC);
	clock->Tick(takeLength, 0u);
	take->Play(0ul, takeLength, 0u);
	station->AddTake(take);
	station->CommitChanges();
	ASSERT_TRUE(take->GetLoops().empty());
	ASSERT_EQ(1u, take->GetMidiLoops().size());
	ASSERT_GT(take->GetMidiLoops().front()->EventCount(), 0u);
	EXPECT_EQ(remoteInterval, clock->SeedSourceLength());
	EXPECT_EQ(19600u, clock->QuantiseSamps());
	audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
	const auto start = take->MidiPlayIndex();

	// More than four remote intervals: ordinary advancement and mapped restore
	// must agree at every block, including remote discipline and take wraps.
	for (unsigned int block = 0u; block < 2500u; ++block)
	{
		take->EndMultiPlay(blockSize);
		clock->Tick(blockSize, 0u);
		if (block % 2u == 0u)
		{
			remote.IntervalPositionSamps = clock->SampOffset();
			remote.DeviceAudioSampleAtObservation = clock->SceneSamplePos();
			const auto update = observe(true);
			ASSERT_FALSE(update.PromptForTempoChange);
			ASSERT_FALSE(update.TempoRequest);
			apply(update);
		}
		audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
		ASSERT_EQ((start + static_cast<std::uint64_t>(block + 1u) * blockSize) % takeLength,
			take->MidiPlayIndex()) << "block=" << block;
	}
	EXPECT_FALSE(coordinator.PendingTempoProposal());
	EXPECT_EQ(0u, take->MappedMidiRestoreDiagnostic().LargestBackstepSamps);
	EXPECT_EQ(0u, take->MappedMidiRestoreDiagnostic().ChangedCount);
}

TEST(NinjamTimingProductionBoundary, EmptySessionTempoChangesCannotRetimeOrDuplicateFirstQuantisedMidiRecording)
{
	for (const auto sampleRate : { 44100u, 48000u })
	for (const auto initialBpi : { 8u, 32u })
	{
	SCOPED_TRACE(::testing::Message() << sampleRate << "/" << initialBpi);
	const auto remoteInterval = ninjam::IntervalSampsFromTempo(113.0f, 8u, sampleRate);
	const auto takeLength = remoteInterval;
	constexpr auto blockSize = 512u;
	auto clock = std::make_shared<Timer>();
	clock->Tick(4096u, 0u);
	auto station = NinjamProductionBoundaryFixture::MakeStation({});
	station->SetClock(clock);
	audio::AudioHost host{ io::UserConfig{} };
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station }));
	ninjam::NinjamTimingCoordinator coordinator;
	ninjam::NinjamTempoJoinOptions options;
	const auto connected = coordinator.ObserveSessionStatus({ true, true, 1u }, options, std::nullopt);
	ASSERT_TRUE(connected.DesiredTransport);
	host.PublishDesiredTiming(*connected.DesiredTransport);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), sampleRate));

	// Apply exactly the NJClient observation sequence from fail-logs-1.txt.
	// Use the adapter's shared validity boundary before conversion/publication.
	ninjam::NinjamRemoteTiming remote;
	remote.IsConnected = true;
	remote.IntervalLengthSamps = 1000u;
	remote.SourceSampleRate = sampleRate;
	remote.Bpm = 120.0f;
	remote.Bpi = 32u;
	remote.HasDeviceAudioSampleAtObservation = true;
	remote.DeviceAudioSampleAtObservation = clock->SceneSamplePos();
	engine::Quantiser quantiser;
	quantiser.SetClock(clock);
	const auto observe = [&](bool hasLocalContent)
	{
		remote.IsValid = ninjam::IsValidRemoteTiming(remote.IntervalLengthSamps,
			remote.SourceSampleRate, remote.Bpm, remote.Bpi);
		auto timing = ninjam::ToDeviceTiming(remote, true, sampleRate, 0u, 0ul, 0u);
		const auto local = clock->ObserveTransport();
		timing.HasLocalTransport = true;
		timing.LocalTransport = { local.MasterLengthSamps, local.MasterPhaseSamps,
			local.LoopCount, local.AbsoluteSamplePos, local.SceneSamplePos };
		return coordinator.Observe(timing, std::nullopt, hasLocalContent, io::UserConfig{}, *clock,
			std::chrono::steady_clock::time_point{} + std::chrono::microseconds(
				remote.DeviceAudioSampleAtObservation * 1000000u / sampleRate));
	};
	const auto apply = [&](const ninjam::NinjamTimingUpdate& update)
	{
		if (update.RemoteGrid)
			quantiser.SetRemoteMidiGrid(update.RemoteGrid->Geometry, update.RemoteGrid->OriginSamps, { station });
		if (update.DesiredTransport)
		{
			host.PublishDesiredTiming(*update.DesiredTransport);
			EXPECT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), sampleRate));
			audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
		}
	};
	const auto placeholder = observe(false);
	EXPECT_FALSE(placeholder.DesiredTransport);
	apply(placeholder);
	EXPECT_EQ(0ul, clock->SeedSourceLength());
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host).IsActive());

	remote.IntervalLengthSamps = ninjam::IntervalSampsFromTempo(400.0f, initialBpi, sampleRate);
	remote.IntervalPositionSamps = 3072u;
	remote.Bpm = 400.0f;
	remote.Bpi = initialBpi;
	const auto accepted = observe(false);
	ASSERT_TRUE(accepted.DesiredTransport);
	EXPECT_FALSE(accepted.PromptForTempoChange);
	EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::ContinuousSync, accepted.DesiredTransport->LocalFollowPolicy);
	apply(accepted);
	// Separate server votes can yield intermediate BPM-only and BPI-only geometries.
	for (const auto bpi : { 8u, 16u, 8u })
	{
		clock->Tick(blockSize, 0u);
		remote.Bpm = 113.0f;
		remote.Bpi = bpi;
		remote.IntervalLengthSamps = ninjam::IntervalSampsFromTempo(remote.Bpm, bpi, sampleRate);
		remote.IntervalPositionSamps = 3072u;
		remote.DeviceAudioSampleAtObservation = clock->SceneSamplePos();
		const auto changed = observe(false);
		ASSERT_TRUE(changed.DesiredTransport);
		ASSERT_FALSE(changed.PromptForTempoChange);
		apply(changed);
	}
	const auto map = audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host);
	EXPECT_EQ(remoteInterval, map.SourceLengthSamps);
	ASSERT_EQ(remoteInterval, map.RemoteLengthSamps);

	// Real captured MIDI material, with no audio loop or synthetic cursor setup.
	engine::LoopTakeParams params;
	params.Id = "first-remote-midi-take";
	params.Size = { 100, 100 };
	audio::MergeMixBehaviourParams merge;
	auto take = std::make_shared<engine::LoopTake>(params, engine::LoopTake::GetMixerParams(params.Size, merge));
	const auto recordStart = static_cast<std::uint32_t>(clock->SceneSamplePos());
	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);
	station->AddTake(take);
	midi::MidiQuantisationSettings settings;
	settings.Enabled = true;
	settings.Fraction = midi::MidiQuantisationFraction::Whole;
	take->SetMidiQuantisation(settings);
	take->Record({}, station->Name(), { 0u }, { "Keys" }, {}, clock->AbsoluteSamplePos(), recordStart);
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(recordStart + 24u, 0u, 60u, 100u),
		"Keys", recordStart + 24u));
	ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(recordStart + 19600u, 0u, 60u),
		"Keys", recordStart + 19600u));
	take->EndMultiWrite(takeLength, true, base::Audible::AUDIOSOURCE_ADC);
	clock->Tick(takeLength, 0u);
	take->Play(0ul, takeLength, 0u);
	station->CommitChanges();
	ASSERT_TRUE(take->GetLoops().empty());
	ASSERT_EQ(1u, take->GetMidiLoops().size());
	ASSERT_GT(take->GetMidiLoops().front()->EventCount(), 0u);
	EXPECT_EQ(remoteInterval, clock->SeedSourceLength());
	EXPECT_EQ((remoteInterval + 4u) / 8u, clock->QuantiseSamps());
	audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
	const auto start = take->MidiPlayIndex();

	class CapturingSink final : public midi::IMidiOutputSink
	{
	public:
		void OnEvent(unsigned int, const midi::MidiEvent& event) noexcept override
		{
			if (event.IsNoteOn()) NoteOns.push_back(event.sampleOffset);
		}
		std::vector<std::uint32_t> NoteOns;
	} sink;
	// More than four remote intervals: ordinary advancement and mapped restore
	// must agree at every block, including remote discipline and take wraps.
	bool continuousPlayback = true;
	for (unsigned int block = 0u; block < 2500u; ++block)
	{
		take->ReadMidiBlock(static_cast<std::uint32_t>(clock->SceneSamplePos()), blockSize, sink);
		take->EndMultiPlay(blockSize);
		clock->Tick(blockSize, 0u);
		if (block % 2u == 0u)
		{
			remote.IntervalPositionSamps = clock->SampOffset();
			remote.DeviceAudioSampleAtObservation = clock->SceneSamplePos();
			const auto update = observe(true);
			ASSERT_FALSE(update.PromptForTempoChange);
			ASSERT_FALSE(update.TempoRequest);
			apply(update);
		}
		audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
		continuousPlayback &= (start + static_cast<std::uint64_t>(block + 1u) * blockSize) % takeLength
			== take->MidiPlayIndex();
	}
	EXPECT_TRUE(continuousPlayback);
	midi::MidiEvent firstNote;
	bool foundNoteOn = false;
	for (std::size_t i = 0u; take->GetMidiLoops().front()->TryGetPlaybackEvent(i, firstNote); ++i)
		if (firstNote.IsNoteOn()) { foundNoteOn = true; break; }
	ASSERT_TRUE(foundNoteOn);
	const auto firstOffset = (firstNote.sampleOffset + takeLength - start) % takeLength;
	constexpr auto totalSamples = 2500u * blockSize;
	const auto expectedNotes = firstOffset < totalSamples ? 1u + (totalSamples - 1u - firstOffset) / takeLength : 0u;
	EXPECT_EQ(expectedNotes, sink.NoteOns.size());
	ASSERT_GE(sink.NoteOns.size(), 4u);
	for (std::size_t i = 1u; i < sink.NoteOns.size(); ++i)
		EXPECT_EQ(takeLength, sink.NoteOns[i] - sink.NoteOns[i - 1u]) << "note=" << i;
	EXPECT_FALSE(coordinator.PendingTempoProposal());
	EXPECT_EQ(0u, take->MappedMidiRestoreDiagnostic().LargestBackstepSamps);
	EXPECT_EQ(0u, take->MappedMidiRestoreDiagnostic().ChangedCount);
	}
}

TEST(NinjamTimingProductionBoundary, NewQuantisedTakeAndMetronomeShareMusicalOrigin)
{
	for (const auto sampleRate : { 44100u, 48000u })
	for (const auto bpm : { 100.0f, 135.0f, 137.0f })
	for (const auto bpi : { 1u, 16u, 32u })
	{
		SCOPED_TRACE(::testing::Message() << sampleRate << "/" << bpm << "/" << bpi);
		const auto interval = ninjam::IntervalSampsFromTempo(bpm, bpi, sampleRate);
		constexpr auto joinPhase = 3072u;
		// The device has run before joining; its zero differs from musical zero.
		auto clock = std::make_shared<Timer>();
		clock->Tick(634368u, 0u);
		auto station = NinjamProductionBoundaryFixture::MakeStation({});
		station->SetClock(clock);
		engine::Quantiser quantiser;
		quantiser.SetClock(clock);
		audio::AudioHost host{ io::UserConfig{} };
		host.SetTimingClock(clock);
		host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
			std::vector<std::shared_ptr<engine::Station>>{ station }));
		ninjam::NinjamTimingCoordinator coordinator;
		auto timing = NinjamProductionBoundaryFixture::Timing(interval, joinPhase, bpm, bpi, clock->SceneSamplePos());
		timing.DeviceSampleRate = sampleRate;
		timing.SourceSampleRate = sampleRate;
		const auto local = clock->ObserveTransport();
		timing.LocalTransport = { local.MasterLengthSamps, local.MasterPhaseSamps,
			local.LoopCount, local.AbsoluteSamplePos, local.SceneSamplePos };
		const auto update = coordinator.Observe(timing, std::nullopt, false, io::UserConfig{}, *clock);
		ASSERT_TRUE(update.DesiredTransport);
		ASSERT_TRUE(update.RemoteGrid);
		quantiser.SetRemoteMidiGrid(update.RemoteGrid->Geometry, update.RemoteGrid->OriginSamps, { station });
		host.PublishDesiredTiming(*update.DesiredTransport);
		ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), sampleRate));
		EXPECT_EQ(joinPhase, clock->SampOffset());
		station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);
		// Add after the initial remote publication, as in an empty-session join.
		auto take = station->AddTake();
		const auto settings = take->ResolvedMidiQuantisation();
		ASSERT_TRUE(settings.HasRemoteGrid());
		EXPECT_EQ(0, settings.RemoteOriginSamps);
		midi::MidiQuantisationSettings choice;
		choice.Enabled = true;
		choice.Fraction = midi::MidiQuantisationFraction::Whole;
		take->SetMidiQuantisation(choice);
		const auto deviceStart = static_cast<std::uint32_t>(clock->SceneSamplePos());
		const auto musicalStart = clock->AbsoluteSamplePos();
		take->Record({}, station->Name(), { 0u }, { "Keys" }, {}, musicalStart, deviceStart);
		station->CommitChanges();
		const auto beat = static_cast<std::uint32_t>((static_cast<std::uint64_t>(interval) + bpi / 2u) / bpi);
		const auto rawOffset = beat - joinPhase + 24u;
		station->EndMultiWrite(interval, true, base::Audible::AUDIOSOURCE_ADC);
		clock->Tick(interval, 0u);
		const auto captureNow = deviceStart + interval;
		ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(deviceStart + rawOffset, 0u, 60u, 100u),
			"Keys", captureNow));
		ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(deviceStart + rawOffset + 1000u, 0u, 60u),
			"Keys", captureNow));
		take->Play(0ul, interval, 0u);
		midi::MidiEvent note;
		ASSERT_TRUE(take->GetMidiLoops().front()->TryGetPlaybackEvent(0u, note));
		ASSERT_TRUE(note.IsNoteOn());
		EXPECT_EQ(beat, musicalStart + note.sampleOffset);
		class CapturingSink final : public midi::IMidiOutputSink
		{
		public:
			void OnEvent(unsigned int, const midi::MidiEvent& event) noexcept override
			{
				if (event.IsNoteOn()) { NoteOn = event; HasNoteOn = true; }
			}
			midi::MidiEvent NoteOn{};
			bool HasNoteOn = false;
		} sink;
		EXPECT_EQ(0u, take->MidiPlayIndex());
		EXPECT_EQ(1u, take->ReadMidiBlock(captureNow, interval, sink));
		ASSERT_TRUE(sink.HasNoteOn);
		EXPECT_EQ(captureNow + beat - musicalStart, sink.NoteOn.sampleOffset);
		ninjam::NinjamMetronomeTimingInput click;
		click.IntervalLengthSamps = interval;
		click.Bpi = bpi;
		click.IntervalPositionSamps = beat - 64u;
		click.NumFrames = 128u;
		ninjam::NinjamMetronomeTimingState clickState;
		const auto onsets = ninjam::NinjamMetronomeTiming::Compute(click, clickState);
		ASSERT_GT(onsets.OnsetCount, 0u);
		EXPECT_EQ(musicalStart + note.sampleOffset,
			static_cast<std::uint64_t>(click.IntervalPositionSamps) + onsets.Onsets[0].Offset);
		// Clearing authority also clears retained defaults for future takes.
		quantiser.SetRemoteMidiGrid({}, 0, { station });
		EXPECT_FALSE(station->AddTake()->ResolvedMidiQuantisation().HasRemoteGrid());
	}
}

TEST(AudioHostLocalMasterPromotion, PreservesRunningAudioPhaseAtQuarterAndOffGridPositions)
{
	for (unsigned long position : { 250ul, 333ul })
	for (unsigned int newLength : { 1000u, 2000u })
	{
		auto take = NinjamProductionBoundaryFixture::MakeTake("promoted-audio", 1000ul, position);
		auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
		const std::vector<std::shared_ptr<engine::Station>> stations{ station };
		audio::AudioHost host{ io::UserConfig{} };
		auto clock = std::make_shared<Timer>();
		clock->SetQuantisation(250u, Timer::QUANTISE_POWER);
		clock->SetSeedSourceLength(1000u);
		clock->InitialiseAbsoluteSamplePos(125u);
		host.SetTimingClock(clock);
		const auto version = host.PublishLocalMasterTiming(newLength / 4u, newLength,
			Timer::QUANTISE_POWER, take.get(), take->GetLoops().front().get());
		audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
		const auto receipt = host.LastPreparedLocalMasterTiming();
		ASSERT_TRUE(receipt.has_value());
		EXPECT_EQ(version, receipt->Version);
		const auto expectedPhase = static_cast<unsigned int>((position * newLength + 500ul) / 1000ul)
			% newLength;
		EXPECT_EQ(expectedPhase, receipt->NewPhaseSamps);
		EXPECT_EQ(1000ul, clock->SeedSourceLength());
		EXPECT_EQ(125u, clock->SampOffset());
		EXPECT_EQ(position, NinjamProductionBoundaryFixture::AudioPosition(*take));
		const auto rebasedStart = (receipt->NewPhaseSamps + newLength
			- static_cast<unsigned int>(receipt->OldAbsoluteSamplePos % newLength)) % newLength;
		ASSERT_TRUE(take->PrepareLocalMasterQuantisation(newLength / 4u, rebasedStart));
		host.CommitLocalMasterTiming(version);
		audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
		EXPECT_EQ(newLength, clock->SeedSourceLength());
		EXPECT_EQ(expectedPhase, clock->SampOffset());
		EXPECT_EQ(position, NinjamProductionBoundaryFixture::AudioPosition(*take));
		EXPECT_EQ(position, take->MidiTimingPosition());
	}
}

TEST(AudioHostLocalMasterPromotion, MidiOnlyTargetSuppliesPhaseAndActivatesGridAtBoundary)
{
	engine::LoopTakeParams params;
	params.Id = "promoted-midi-only";
	params.Size = { 100, 100 };
	audio::MergeMixBehaviourParams merge;
	auto take = std::make_shared<NinjamProductionBoundaryLoopTake>(params,
		engine::LoopTake::GetMixerParams(params.Size, merge));
	take->Record({}, "station", { 0u }, { "" });
	take->Play(0u, 1000u, 0u);
	take->SetMidiTimingState(250ul, 1000ul);
	auto loop = take->GetMidiLoops().at(0u);
	take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);
	midi::MidiQuantisationSettings oldGrid;
	oldGrid.Enabled = true;
	oldGrid.GrainSamps = 100u;
	oldGrid.Fraction = midi::MidiQuantisationFraction::Quarter;
	take->SetMidiQuantisation(oldGrid);
	ASSERT_TRUE(loop->SetQuantisation(take->ResolvedMidiQuantisation(), 0u));
	const auto attack = midi::MidiEvent::MakeNoteOn(37u, 0u, 60u, 100u);
	loop->ReplaceRecordedEvents(&attack, 1u, 1000u);
	midi::MidiEvent before{};
	ASSERT_TRUE(loop->TryGetPlaybackEvent(0u, before));
	EXPECT_EQ(25u, before.sampleOffset);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	// Station attachment applies its global policy to the take. Keep this
	// fixture quantised across the promotion as an actual audible-grid check.
	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);
	const std::vector<std::shared_ptr<engine::Station>> stations{ station };
	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetQuantisation(250u, Timer::QUANTISE_POWER);
	clock->SetSeedSourceLength(1000u);
	clock->InitialiseAbsoluteSamplePos(125u);
	host.SetTimingClock(clock);
	const auto version = host.PublishLocalMasterTiming(500u, 2000u,
		Timer::QUANTISE_POWER, take.get(), nullptr);
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	const auto receipt = host.LastPreparedLocalMasterTiming();
	ASSERT_TRUE(receipt.has_value());
	EXPECT_EQ(500u, receipt->NewPhaseSamps);
	const auto rebasedStart = (receipt->NewPhaseSamps + 2000u
		- static_cast<unsigned int>(receipt->OldAbsoluteSamplePos % 2000u)) % 2000u;
	ASSERT_TRUE(take->PrepareLocalMasterQuantisation(500u, rebasedStart));
	ASSERT_TRUE(loop->PreparedQuantisationReadyAtAudioBoundary());
	midi::MidiEvent stillOld{};
	ASSERT_TRUE(loop->TryGetPlaybackEvent(0u, stillOld));
	EXPECT_EQ(before.sampleOffset, stillOld.sampleOffset);
	host.CommitLocalMasterTiming(version);
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	EXPECT_EQ(version, host.LastActivatedLocalMasterTimingVersion());
	EXPECT_EQ(500u, clock->SampOffset());
	midi::MidiEvent committed{};
	ASSERT_TRUE(loop->TryGetPlaybackEvent(0u, committed));
	EXPECT_EQ(0u, committed.sampleOffset);
	EXPECT_EQ(250ul, take->MidiTimingPosition());
	struct EmittedMidi final : midi::IMidiOutputSink
	{
		void OnEvent(unsigned int, const midi::MidiEvent& event) noexcept override
		{
			Events.push_back(event);
		}
		std::vector<midi::MidiEvent> Events;
	} audible;
	// The committed event is at body offset zero. The take is still at 250,
	// so it sounds at the next seam rather than the first post-commit frame.
	take->ReadMidiBlock(0u, 1u, audible);
	EXPECT_TRUE(audible.Events.empty());
	take->EndMultiPlay(1u);
	take->ReadMidiBlock(1u, 750u, audible);
	ASSERT_EQ(1u, audible.Events.size());
	EXPECT_TRUE(audible.Events.front().IsNoteOn());
	EXPECT_EQ(750u, audible.Events.front().sampleOffset);
}

TEST(AudioHostLocalMasterPromotion, OffGridMidiAndAudioTransientShareFirstCommittedBlock)
{
	constexpr auto length = 1000u;
	constexpr auto cursor = 250u;
	engine::LoopTakeParams params;
	params.Id = "promoted-transients";
	params.Size = { 100, 100 };
	audio::MergeMixBehaviourParams merge;
	auto take = std::make_shared<NinjamProductionBoundaryLoopTake>(params,
		engine::LoopTake::GetMixerParams(params.Size, merge));
	take->Record({}, "station", { 0u }, { "" });
	take->Play(0u, length, 0u);
	audio::WireMixBehaviourParams wire;
	wire.Channels = { 0u };
	engine::LoopParams audioParams;
	audioParams.Wav = "promoted-audio";
	audioParams.FadeSamps = constants::DefaultFadeSamps;
	auto audioLoop = std::make_shared<engine::Loop>(audioParams,
		engine::Loop::GetMixerParams({ 80, 80 }, wire));
	audioLoop->Record();
	std::vector<float> source(constants::MaxLoopFadeSamps + length, 0.0f);
	source[constants::MaxLoopFadeSamps + cursor] = 1.0f;
	base::AudioWriteRequest request;
	request.samples = source.data();
	request.numSamps = static_cast<unsigned int>(source.size());
	request.stride = 1u;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;
	audioLoop->OnBlockWrite(request, 0);
	audioLoop->EndWrite(static_cast<unsigned int>(source.size()), true);
	audioLoop->Play(constants::MaxLoopFadeSamps, length, false);
	audioLoop->ShiftPlayIndex(cursor);
	take->AddLoop(audioLoop);
	take->CommitChanges();
	take->SetMidiTimingState(cursor, length);
	auto midiLoop = take->GetMidiLoops().at(0u);
	take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);
	midi::MidiQuantisationSettings oldGrid;
	oldGrid.Enabled = true;
	oldGrid.GrainSamps = 100u;
	oldGrid.Fraction = midi::MidiQuantisationFraction::Quarter;
	take->SetMidiQuantisation(oldGrid);
	ASSERT_TRUE(midiLoop->SetQuantisation(take->ResolvedMidiQuantisation(), 0u));
	const auto attack = midi::MidiEvent::MakeNoteOn(287u, 0u, 60u, 100u);
	midiLoop->ReplaceRecordedEvents(&attack, 1u, length);
	midi::MidiEvent oldEvent{};
	ASSERT_TRUE(midiLoop->TryGetPlaybackEvent(0u, oldEvent));
	ASSERT_EQ(275u, oldEvent.sampleOffset);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);
	const std::vector<std::shared_ptr<engine::Station>> stations{ station };
	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetQuantisation(250u, Timer::QUANTISE_POWER);
	clock->SetSeedSourceLength(length);
	clock->InitialiseAbsoluteSamplePos(125u);
	host.SetTimingClock(clock);
	const auto version = host.PublishLocalMasterTiming(500u, 2000u,
		Timer::QUANTISE_POWER, take.get(), audioLoop.get());
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	const auto receipt = host.LastPreparedLocalMasterTiming();
	ASSERT_TRUE(receipt.has_value());
	ASSERT_EQ(500u, receipt->NewPhaseSamps);
	const auto rebasedStart = (receipt->NewPhaseSamps + 2000u
		- static_cast<unsigned int>(receipt->OldAbsoluteSamplePos % 2000u)) % 2000u;
	ASSERT_TRUE(take->PrepareLocalMasterQuantisation(500u, rebasedStart));
	host.CommitLocalMasterTiming(version);
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	ASSERT_EQ(version, host.LastActivatedLocalMasterTimingVersion());
	EXPECT_EQ(cursor, audioLoop->BodyPlayIndex());
	EXPECT_EQ(cursor, take->MidiTimingPosition());
	midi::MidiEvent committed{};
	ASSERT_TRUE(midiLoop->TryGetPlaybackEvent(0u, committed));
	EXPECT_EQ(cursor, committed.sampleOffset);
	float audioSample = 0.0f;
	ASSERT_EQ(1u, audioLoop->ReadBlock(&audioSample, 0, 1u));
	EXPECT_FLOAT_EQ(1.0f, audioSample);
	struct EmittedMidi final : midi::IMidiOutputSink
	{
		void OnEvent(unsigned int index, const midi::MidiEvent& event) noexcept override
		{
			OutputIndex = index;
			Events.push_back(event);
		}
		unsigned int OutputIndex = 99u;
		std::vector<midi::MidiEvent> Events;
	} audible;
	take->ReadMidiBlock(0u, 1u, audible);
	ASSERT_EQ(1u, audible.Events.size());
	EXPECT_EQ(0u, audible.OutputIndex);
	EXPECT_TRUE(audible.Events.front().IsNoteOn());
	EXPECT_EQ(0u, audible.Events.front().sampleOffset);
}

TEST(AudioHostLocalMasterPromotion, ReplacedMidiSourceDefersClockUntilPreparedAgain)
{
	auto take = NinjamProductionBoundaryFixture::MakeTake("stale-midi-stage", 1000ul, 250ul);
	auto loop = take->GetMidiLoops().at(0u);
	midi::MidiQuantisationSettings grid;
	grid.Enabled = true;
	grid.GrainSamps = 100u;
	grid.Fraction = midi::MidiQuantisationFraction::Quarter;
	take->SetMidiQuantisation(grid);
	ASSERT_TRUE(loop->SetQuantisation(take->ResolvedMidiQuantisation(), 0u));
	const auto original = midi::MidiEvent::MakeNoteOn(37u, 0u, 60u, 100u);
	loop->ReplaceRecordedEvents(&original, 1u, 1000u);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	const std::vector<std::shared_ptr<engine::Station>> stations{ station };
	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetQuantisation(250u, Timer::QUANTISE_POWER);
	clock->SetSeedSourceLength(1000u);
	clock->InitialiseAbsoluteSamplePos(125u);
	host.SetTimingClock(clock);
	const auto version = host.PublishLocalMasterTiming(500u, 2000u,
		Timer::QUANTISE_POWER, take.get(), take->GetLoops().front().get());
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	const auto receipt = host.LastPreparedLocalMasterTiming();
	ASSERT_TRUE(receipt.has_value());
	const auto start = (receipt->NewPhaseSamps + 2000u
		- static_cast<unsigned int>(receipt->OldAbsoluteSamplePos % 2000u)) % 2000u;
	ASSERT_TRUE(take->PrepareLocalMasterQuantisation(500u, start));
	const auto replacement = midi::MidiEvent::MakeNoteOn(80u, 0u, 61u, 90u);
	loop->ReplaceRecordedEvents(&replacement, 1u, 1000u);
	EXPECT_FALSE(loop->PreparedQuantisationReadyAtAudioBoundary());
	host.CommitLocalMasterTiming(version);
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	EXPECT_EQ(1000ul, clock->SeedSourceLength());
	EXPECT_EQ(125u, clock->SampOffset());
	midi::MidiEvent current{};
	ASSERT_TRUE(loop->TryGetPlaybackEvent(0u, current));
	EXPECT_EQ(61u, current.data1);
	ASSERT_TRUE(take->PrepareLocalMasterQuantisation(500u, start));
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	EXPECT_EQ(2000ul, clock->SeedSourceLength());
	EXPECT_EQ(receipt->NewPhaseSamps, clock->SampOffset());
	ASSERT_TRUE(loop->TryGetPlaybackEvent(0u, current));
	EXPECT_EQ(61u, current.data1);
}

TEST(AudioHostLocalMasterPromotion, LatestPreparedTimingReplacesEarlierVersion)
{
	auto take = NinjamProductionBoundaryFixture::MakeTake("latest-master", 1000ul, 250ul);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	const std::vector<std::shared_ptr<engine::Station>> stations{ station };
	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetQuantisation(250u, Timer::QUANTISE_POWER);
	clock->SetSeedSourceLength(1000u);
	clock->InitialiseAbsoluteSamplePos(125u);
	host.SetTimingClock(clock);
	const auto oldVersion = host.PublishLocalMasterTiming(500u, 2000u,
		Timer::QUANTISE_POWER, take.get(), take->GetLoops().front().get());
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	const auto oldReceipt = host.LastPreparedLocalMasterTiming();
	ASSERT_TRUE(oldReceipt.has_value());
	const auto oldStart = (oldReceipt->NewPhaseSamps + 2000u
		- static_cast<unsigned int>(oldReceipt->OldAbsoluteSamplePos % 2000u)) % 2000u;
	ASSERT_TRUE(take->PrepareLocalMasterQuantisation(500u, oldStart));
	const auto newVersion = host.PublishLocalMasterTiming(1000u, 4000u,
		Timer::QUANTISE_POWER, take.get(), take->GetLoops().front().get());
	ASSERT_GT(newVersion, oldVersion);
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	const auto newReceipt = host.LastPreparedLocalMasterTiming();
	ASSERT_TRUE(newReceipt.has_value());
	EXPECT_EQ(newVersion, newReceipt->Version);
	EXPECT_EQ(1000u, newReceipt->NewPhaseSamps);
	host.CommitLocalMasterTiming(oldVersion);
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	EXPECT_EQ(1000ul, clock->SeedSourceLength());
	const auto newStart = (newReceipt->NewPhaseSamps + 4000u
		- static_cast<unsigned int>(newReceipt->OldAbsoluteSamplePos % 4000u)) % 4000u;
	ASSERT_TRUE(take->PrepareLocalMasterQuantisation(1000u, newStart));
	host.CommitLocalMasterTiming(newVersion);
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	EXPECT_EQ(4000ul, clock->SeedSourceLength());
	EXPECT_EQ(1000u, clock->SampOffset());
}

TEST(NinjamTimingProductionBoundary, RemoteBearingNoSyncReinitializesIdenticalGeometry)
{
    auto take = NinjamProductionBoundaryFixture::MakeTake("resume-offset", 777ul, 213ul);
    auto station = NinjamProductionBoundaryFixture::MakeStation({take});
    audio::AudioHost host{io::UserConfig{}};
    auto clock = std::make_shared<Timer>();
    clock->SetSeedSourceLength(1000ul);
    host.SetTimingClock(clock);
    host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
        std::vector<std::shared_ptr<engine::Station>>{station}));
    auto desired = NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
        ninjam::NinjamLocalFollowPolicy::BlockSync, 1100ul, 0u);
    host.PublishDesiredTiming(desired);
    ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
    audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
    desired.Version = 2u;
    desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::NoSync;
    desired.Intent = ninjam::NinjamDesiredTimingIntent::NoSync;
    host.PublishDesiredTiming(desired);
    ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
    ASSERT_FALSE(audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host).IsActive());
    const auto before = NinjamProductionBoundaryFixture::AudioPosition(*take);
    desired.Version = 3u;
    desired.Generation = 2u;
    desired.RemoteMasterPhaseSamps = 200u;
    desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::BlockSync;
    desired.Intent = ninjam::NinjamDesiredTimingIntent::Replacement;
    host.PublishDesiredTiming(desired);
    ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
    audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
    const auto map = audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host);
    ASSERT_TRUE(map.IsActive());
    // Loss of authority clears the map, not the recording's source ruler.
    EXPECT_EQ(1000ul, map.SourceLengthSamps);
    EXPECT_EQ(1100ul, map.RemoteLengthSamps);
    const auto correction = ninjam::SourcePhaseAtRemotePhase(200u, 1000ul, 1100ul);
    EXPECT_EQ((before + correction) % 777ul, NinjamProductionBoundaryFixture::AudioPosition(*take));
    clock->Tick(137u, 0u);
    take->EndMultiPlay(137u);
    audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
    EXPECT_EQ((before + correction + ninjam::MapRemoteElapsedToLocal(137u, 1000ul, 1100ul)) % 777ul,
        NinjamProductionBoundaryFixture::AudioPosition(*take));
    EXPECT_EQ(NinjamProductionBoundaryFixture::AudioPosition(*take), take->MidiTimingPosition());
}

TEST(NinjamTimingProductionBoundary, RecordingMembershipProtectsSourceRulerAcrossTempoChanges)
{
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(1000ul);
	auto station = NinjamProductionBoundaryFixture::MakeStation({});
	auto take = station->AddTake();
	take->Record({}, station->Name(), { 0u });
	station->CommitChanges();
	auto emptyStation = NinjamProductionBoundaryFixture::MakeStation({});
	audio::AudioHost host{ io::UserConfig{} };
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station, emptyStation }));
	EXPECT_TRUE(station->HasTimingContentAtAudioBoundary());
	EXPECT_FALSE(emptyStation->HasTimingContentAtAudioBoundary());
	for (const auto remoteLength : { 2000ul, 4000ul })
	{
		host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, remoteLength, remoteLength,
			ninjam::NinjamLocalFollowPolicy::BlockSync, remoteLength, 0u));
		ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
		EXPECT_EQ(1000ul, audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host).SourceLengthSamps);
	}
}

TEST(NinjamTimingProductionBoundary, NoSyncPreservesRecordingRulerForExplicitResync)
{
	auto take = NinjamProductionBoundaryFixture::MakeTake("retain-ruler", 3000ul, 123ul);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(1000ul);
	audio::AudioHost host{ io::UserConfig{} };
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station }));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, 4000ul, 0u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 2u, 1u,
		ninjam::NinjamLocalFollowPolicy::NoSync, 0ul, 0u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 3u, 2u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, 4000ul, 0u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	EXPECT_EQ(1000ul, audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host).SourceLengthSamps);
	EXPECT_EQ(123ul, take->MidiTimingPosition());
}

TEST(NinjamTimingProductionBoundary, CollapsedFollowRevisionAdoptsActivatedSourceAndRetainsOffsets)
{
	auto first = NinjamProductionBoundaryFixture::MakeTake("new-master", 1000ul, 250ul);
	auto other = NinjamProductionBoundaryFixture::MakeTake("other", 3000ul, 623ul);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ first, other });
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(1000ul);
	clock->Tick(250u, 0u);
	audio::AudioHost host{ io::UserConfig{} };
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station }));
	auto desired = NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, 1000ul, 250u, 250u);
	host.PublishDesiredTiming(desired);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 250u, 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	// Audio observes only the phase publication, not the intermediate suspension/replacement.
	desired.Version = 2u;
	desired.FollowRevision = 1u;
	desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::NoSync;
	host.PublishDesiredTiming(desired);
	desired.Version = 3u;
	desired.Generation = 2u;
	desired.SourceMasterIntervalLengthSamps = 2000ul;
	desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::BlockSync;
	host.PublishDesiredTiming(desired);
	desired.Version = 4u;
	desired.Generation = 3u;
	desired.Intent = ninjam::NinjamDesiredTimingIntent::PhaseDiscipline;
	host.PublishDesiredTiming(desired);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 250u, 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	EXPECT_EQ(2000ul, audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host).SourceLengthSamps);
	EXPECT_EQ(500ul, first->MidiTimingPosition());
	EXPECT_EQ(873ul, other->MidiTimingPosition());
	EXPECT_EQ(500ul, NinjamProductionBoundaryFixture::AudioPosition(*first));
	EXPECT_EQ(873ul, NinjamProductionBoundaryFixture::AudioPosition(*other));
	EXPECT_FALSE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 250u, 48000u));
}

TEST(AudioHostLocalMasterPromotion, ConnectedTapActivatesRoundedAudioLengthAndGridTogether)
{
	constexpr auto originalLength = 96001ul;
	auto take = NinjamProductionBoundaryFixture::MakeTake("tap-master", originalLength, 0ul);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	auto clock = std::make_shared<Timer>();
	clock->SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
	clock->SetSeedSourceLength(192000ul);
	station->SetClock(clock);
	const std::vector<std::shared_ptr<engine::Station>> stations{ station };
	audio::AudioHost host{ io::UserConfig{} };
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(stations));
	engine::Quantiser quantiser;
	quantiser.SetClock(clock);
	quantiser.SetRemoteMidiGrid({ 192000ul, 8u, 0u, 120.0f, 1u }, 0, stations);
	auto desired = NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::ContinuousSync, 192000ul, 0u);
	host.PublishDesiredTiming(desired);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	station->BeginCaptureAudioBoundary(0u, 0u, 0u, {}, { 192000ul, 8u, 0u, 120.0f, 1u });
	ASSERT_EQ(192000ul, take->RecordingMasterLengthSamps());
	quantiser.SetLocalMasterTimingPublisher([&](const engine::QuantisationTiming& timing,
		Timer::QuantisationType type, const engine::LoopTake* target, const engine::Loop* loop)
	{
		desired.Version++;
		desired.FollowRevision++;
		desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::NoSync;
		desired.Intent = ninjam::NinjamDesiredTimingIntent::NoSync;
		host.PublishDesiredTiming(desired);
		return host.PublishLocalMasterTiming(timing.SeedSamps, timing.MasterLoopSamps, type, target, loop);
	});
	ASSERT_FALSE(quantiser.HandleTapTempo(0u, 48000u, stations, io::UserConfig{}, true));
	ASSERT_TRUE(quantiser.HandleTapTempo(24000u, 48000u, stations, io::UserConfig{}, true));
	EXPECT_EQ(originalLength, take->GetLoops().front()->LoopLength());
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	engine::LocalTiming localTiming;
	localTiming.Advance(quantiser, host, stations);
	EXPECT_EQ(originalLength, take->GetLoops().front()->LoopLength());
	EXPECT_EQ(192000ul, clock->SeedSourceLength());
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalMaster(host, stations, clock.get());
	EXPECT_EQ(96000ul, take->GetLoops().front()->LoopLength());
	EXPECT_EQ(originalLength, take->GetLoops().front()->PhysicalLoopLength());
	EXPECT_EQ(96000ul, clock->SeedSourceLength());
	EXPECT_EQ(96000ul, take->RecordingMasterLengthSamps());
	EXPECT_EQ(4u, take->RecordingBeatsPerInterval());
	EXPECT_EQ(24000u, clock->QuantiseSamps());
	EXPECT_EQ(96000u, take->ResolvedMidiQuantisation().BaseIntervalSamps);
	EXPECT_EQ(4u, take->ResolvedMidiQuantisation().BaseDivisions);
	localTiming.Advance(quantiser, host, stations);
	EXPECT_FALSE(quantiser.LocalMasterTimingPending());
	EXPECT_TRUE(quantiser.HasActivatedLocalMaster(stations));
	// A delayed suspension can still carry the preceding edit's source geometry.
	desired.Version++;
	desired.SourceMasterIntervalLengthSamps = 192000ul;
	host.PublishDesiredTiming(desired);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	// Reconnect without a new push must use the actual activated local recording ruler.
	desired.Version++;
	desired.SessionEpoch++;
	desired.Generation++;
	desired.SourceMasterIntervalLengthSamps = 0ul;
	desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::BlockSync;
	desired.Intent = ninjam::NinjamDesiredTimingIntent::Replacement;
	host.PublishDesiredTiming(desired);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	EXPECT_EQ(96000ul, audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host).SourceLengthSamps);
}

TEST(NinjamTimingProductionBoundary, NewRecordingsUseAcceptedTempoWithOlderTakeAndAfterDitch)
{
	for (const auto sampleRate : { 44100u, 48000u })
	for (const auto blockSize : { 64u, 512u })
	for (const bool quantised : { false, true })
	{
		SCOPED_TRACE(::testing::Message() << sampleRate << "/" << blockSize << "/" << quantised);
		const auto oldInterval = ninjam::IntervalSampsFromTempo(113.0f, 8u, sampleRate);
		const auto newInterval = ninjam::IntervalSampsFromTempo(128.0f, 8u, sampleRate);
		auto oldTake = NinjamProductionBoundaryFixture::MakeTake("old-113", oldInterval, 0ul);
		auto station = NinjamProductionBoundaryFixture::MakeStation({ oldTake });
		auto clock = std::make_shared<Timer>();
		clock->SetSeedSourceLength(oldInterval);
		station->SetClock(clock);
		engine::Quantiser quantiser;
		quantiser.SetClock(clock);
		audio::AudioHost host{ io::UserConfig{} };
		host.SetTimingClock(clock);
		host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
			std::vector<std::shared_ptr<engine::Station>>{ station }));
		auto desired = NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
			ninjam::NinjamLocalFollowPolicy::ContinuousSync, oldInterval, 0u);
		desired.BeatsPerInterval = 8u;
		desired.TempoBpm = 113.0f;
		desired.RemoteGridStepSamps = (oldInterval + 4u) / 8u;
		host.PublishDesiredTiming(desired);
		ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, sampleRate));
		audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
		station->BeginCaptureAudioBoundary(0u, 0u, 0u, {}, { oldInterval, 8u, 0u, 113.0f, 1u });
		desired.Version++;
		desired.Generation++;
		desired.RemoteMasterIntervalLengthSamps = newInterval;
		desired.RemoteGridStepSamps = (newInterval + 4u) / 8u;
		desired.TempoBpm = 128.0f;
		desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::BlockSync;
		host.PublishDesiredTiming(desired);
		ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, sampleRate));
		audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
		quantiser.SetRemoteMidiGrid({ newInterval, 8u, 0u, 128.0f, desired.Generation }, 0, { station });
		station->SetGlobalMidiQuantState(quantised ? io::JamFile::GlobalMidiQuantState::All
			: io::JamFile::GlobalMidiQuantState::Off);
		const auto record = [&](const char* id)
		{
			engine::LoopTakeParams params;
			params.Id = id;
			params.Size = { 100, 100 };
			auto take = std::make_shared<engine::LoopTake>(params,
				engine::LoopTake::GetMixerParams(params.Size, audio::MergeMixBehaviourParams{}));
			station->AddTake(take);
			const auto start = static_cast<std::uint32_t>(clock->SceneSamplePos());
			take->Record({}, station->Name(), { 0u }, { "Keys" }, {}, clock->AbsoluteSamplePos(), start);
			station->CommitChanges();
			station->BeginCaptureAudioBoundary(start, clock->SceneSamplePos(), clock->AbsoluteSamplePos(), {},
				{ newInterval, 8u, clock->SampOffset(), 128.0f, desired.Generation });
			EXPECT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(start + 1000u, 0u, 60u, 100u), "Keys", start + 1000u));
			EXPECT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(start + 2000u, 0u, 60u), "Keys", start + 2000u));
			take->EndMultiWrite(newInterval, true, base::Audible::AUDIOSOURCE_ADC);
			clock->Tick(newInterval, 0u);
			oldTake->EndMultiPlay(newInterval);
			take->Play(0ul, newInterval, 0u);
			station->CommitChanges();
			return take;
		};
		const auto verify = [&](const std::shared_ptr<engine::LoopTake>& take)
		{
			class Sink final : public midi::IMidiOutputSink
			{
			public:
				void OnEvent(unsigned int, const midi::MidiEvent& event) noexcept override
				{ if (event.IsNoteOn()) Notes.push_back(event.sampleOffset); }
				std::vector<std::uint32_t> Notes;
			} sink;
			audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
			const auto initial = take->MidiPlayIndex();
			const auto outputStart = static_cast<std::uint32_t>(clock->SceneSamplePos());
			std::uint32_t processed = 0u;
			bool exactPhase = true;
			while (processed < newInterval * 5u)
			{
				take->ReadMidiBlock(outputStart + processed, blockSize, sink);
				take->EndMultiPlay(blockSize);
				oldTake->EndMultiPlay(blockSize);
				clock->Tick(blockSize, 0u);
				processed += blockSize;
				audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
				exactPhase &= take->MidiPlayIndex() == (initial + processed) % newInterval;
			}
			EXPECT_TRUE(exactPhase);
			EXPECT_GE(sink.Notes.size(), 5u);
			bool normalSpacing = true;
			for (std::size_t i = 1u; i < sink.Notes.size(); ++i)
				normalSpacing &= sink.Notes[i] - sink.Notes[i - 1u] == newInterval;
			EXPECT_TRUE(normalSpacing);
		};
		auto newTake = record("new-128");
		verify(newTake);
		// Each take follows every later accepted tempo using its own source ruler.
		for (const auto bpm : { 137.0f, 128.0f, 137.0f, 128.0f })
		{
			const auto interval = ninjam::IntervalSampsFromTempo(bpm, 8u, sampleRate);
			desired.Version++;
			desired.Generation++;
			desired.TempoBpm = bpm;
			desired.RemoteMasterIntervalLengthSamps = interval;
			desired.RemoteGridStepSamps = (interval + 4u) / 8u;
			desired.RemoteMasterPhaseSamps = 0u;
			desired.RemotePhaseDeviceSample = clock->SceneSamplePos();
			host.PublishDesiredTiming(desired);
			ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), sampleRate));
			audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
			EXPECT_EQ(0ul, newTake->MidiPlayIndex());
			EXPECT_EQ(0ul, oldTake->MidiPlayIndex());
			constexpr auto blocks = 401u;
			for (unsigned int i = 0u; i < blocks; ++i)
			{
				newTake->EndMultiPlay(blockSize);
				oldTake->EndMultiPlay(blockSize);
				clock->Tick(blockSize, 0u);
				audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
			}
			const auto elapsed = static_cast<std::uint64_t>(blocks) * blockSize;
			EXPECT_EQ(((elapsed * newInterval + interval / 2u) / interval) % newInterval, newTake->MidiPlayIndex());
			EXPECT_EQ(((elapsed * oldInterval + interval / 2u) / interval) % oldInterval, oldTake->MidiPlayIndex());
		}
		// Unity-rate recordings still consume remote drift/phase discipline.
		for (const auto correction : { 7ll, -11ll, 3ll, -2ll })
		{
			const auto before = newTake->MidiPlayIndex();
			desired.Version++;
			desired.Generation++;
			desired.Intent = ninjam::NinjamDesiredTimingIntent::PhaseDiscipline;
			desired.RemoteMasterPhaseSamps = static_cast<unsigned int>((clock->SampOffset() + newInterval + correction) % newInterval);
			desired.RemotePhaseDeviceSample = clock->SceneSamplePos();
			host.PublishDesiredTiming(desired);
			ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), sampleRate));
			EXPECT_EQ((before + newInterval + correction) % newInterval, newTake->MidiPlayIndex());
		}
		verify(newTake);
		EXPECT_EQ(oldInterval, audio::NinjamAudioBoundaryTestAccess::SyncPhaseMap(host).SourceLengthSamps);
		ASSERT_TRUE(station->DitchTake(newTake));
		ASSERT_TRUE(station->DitchTake(oldTake));
		station->CommitChanges();
		auto fresh = record("fresh-128-after-ditch");
		verify(fresh);
	}
}

TEST(NinjamTimingProductionBoundary, DisconnectedCaptureDoesNotAcquireOrUseRemoteRecordingBaseline)
{
	auto take = NinjamProductionBoundaryFixture::MakeTake("local-only", 1000ul, 123ul);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	station->BeginCaptureAudioBoundary(0u, 0u, 0u, {});
	EXPECT_EQ(0ul, take->RecordingMasterLengthSamps());
	EXPECT_EQ(0u, take->RecordingBeatsPerInterval());
	for (unsigned int i = 0u; i < 1000u; ++i) take->EndMultiPlay(64u);
	EXPECT_EQ(123ul, take->MidiTimingPosition());
	EXPECT_EQ(123ul, NinjamProductionBoundaryFixture::AudioPosition(*take));
}

TEST(NinjamTimingProductionBoundary, RecordingBaselineSurvivesNoSyncWithoutChangingLocalPlayback)
{
	auto take = NinjamProductionBoundaryFixture::MakeTake("remote-recording", 1000ul, 123ul);
	auto station = NinjamProductionBoundaryFixture::MakeStation({ take });
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(1000ul);
	station->SetClock(clock);
	station->BeginCaptureAudioBoundary(0u, 0u, 0u, {}, { 1000ul, 8u, 0u, 120.0f, 1u });
	audio::AudioHost host{ io::UserConfig{} };
	host.SetTimingClock(clock);
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
		std::vector<std::shared_ptr<engine::Station>>{ station }));
	auto desired = NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, 2000ul, 100u);
	host.PublishDesiredTiming(desired);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	const auto before = take->MidiPlayIndex();
	desired.Version++;
	desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::NoSync;
	host.PublishDesiredTiming(desired);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, 48000u));
	EXPECT_EQ(before, take->MidiPlayIndex());
	for (unsigned int i = 0u; i < 1000u; ++i)
	{
		take->EndMultiPlay(64u);
		clock->Tick(64u, 0u);
	}
	EXPECT_EQ(before, take->MidiPlayIndex());
	EXPECT_EQ(before, NinjamProductionBoundaryFixture::AudioPosition(*take));
	EXPECT_EQ(1000ul, take->RecordingMasterLengthSamps());
	desired.Version++;
	desired.Generation++;
	desired.RemoteMasterIntervalLengthSamps = 1000ul;
	desired.RemoteMasterPhaseSamps = 0u;
	desired.RemotePhaseDeviceSample = clock->SceneSamplePos();
	desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::ContinuousSync;
	host.PublishDesiredTiming(desired);
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), 48000u));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	const auto resumed = take->MidiPlayIndex();
	for (unsigned int i = 0u; i < 1001u; ++i)
	{
		take->EndMultiPlay(64u);
		clock->Tick(64u, 0u);
		audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
	}
	EXPECT_EQ((resumed + 64u) % 1000ul, take->MidiPlayIndex());
	EXPECT_EQ(take->MidiPlayIndex(), NinjamProductionBoundaryFixture::AudioPosition(*take));
}

TEST(NinjamTimingProductionBoundary, ThreeRecordingBaselinesRemainDistinctAcrossRepeatedRemoteChanges)
{
	for (const auto sampleRate : { 44100u, 48000u })
	{
		auto station = NinjamProductionBoundaryFixture::MakeStation({});
		auto clock = std::make_shared<Timer>();
		station->SetClock(clock);
		audio::AudioHost host{ io::UserConfig{} };
		host.SetTimingClock(clock);
		host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>(
			std::vector<std::shared_ptr<engine::Station>>{ station }));
		auto desired = NinjamProductionBoundaryFixture::Desired(1u, 0u, 0u,
			ninjam::NinjamLocalFollowPolicy::ContinuousSync, 0ul, 0u);
		const auto accept = [&](float bpm)
		{
			desired.Version++;
			desired.Generation++;
			desired.RemoteMasterIntervalLengthSamps = ninjam::IntervalSampsFromTempo(bpm, 8u, sampleRate);
			desired.RemoteGridStepSamps = (desired.RemoteMasterIntervalLengthSamps + 4u) / 8u;
			desired.RemoteMasterPhaseSamps = 0u;
			desired.RemotePhaseDeviceSample = clock->SceneSamplePos();
			desired.BeatsPerInterval = 8u;
			desired.TempoBpm = bpm;
			host.PublishDesiredTiming(desired);
			EXPECT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, clock->SceneSamplePos(), sampleRate));
			audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
		};
		std::vector<std::shared_ptr<NinjamProductionBoundaryLoopTake>> takes;
		std::vector<unsigned long> recordedIntervals;
		for (const auto bpm : { 113.0f, 128.0f, 137.0f })
		{
			accept(bpm);
			const auto length = desired.RemoteMasterIntervalLengthSamps;
			auto take = NinjamProductionBoundaryFixture::MakeTake("recording-" + std::to_string(bpm), length, 0ul);
			station->AddTake(take);
			station->CommitChanges();
			// Also covers a completed take first published at its admission boundary.
			station->BeginCaptureAudioBoundary(static_cast<std::uint32_t>(clock->SceneSamplePos()), clock->SceneSamplePos(),
				clock->AbsoluteSamplePos(), {}, { length, 8u, 0u, bpm, desired.Generation });
			takes.push_back(take);
			recordedIntervals.push_back(length);
			audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
			for (unsigned int block = 0u; block < 501u; ++block)
			{
				for (const auto& member : takes) member->EndMultiPlay(64u);
				clock->Tick(64u, 0u);
				audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
				ASSERT_EQ((static_cast<std::uint64_t>(block + 1u) * 64u) % length, take->MidiPlayIndex());
				ASSERT_EQ(take->MidiPlayIndex(), NinjamProductionBoundaryFixture::AudioPosition(*take));
			}
		}
		for (const auto bpm : { 128.0f, 113.0f, 137.0f, 128.0f })
		{
			accept(bpm);
			for (const auto& take : takes) EXPECT_EQ(0ul, take->MidiPlayIndex());
			for (unsigned int block = 0u; block < 401u; ++block)
			{
				for (const auto& take : takes) take->EndMultiPlay(64u);
				clock->Tick(64u, 0u);
				audio::NinjamAudioBoundaryTestAccess::RestoreMappedSource(host, clock->SceneSamplePos());
			}
			for (std::size_t i = 0u; i < takes.size(); ++i)
			{
				const auto source = recordedIntervals[i];
				const auto remote = desired.RemoteMasterIntervalLengthSamps;
				const auto expected = ((401ull * 64u * source + remote / 2u) / remote) % source;
				EXPECT_EQ(source, takes[i]->RecordingMasterLengthSamps());
				EXPECT_EQ(expected, takes[i]->MidiPlayIndex());
				EXPECT_EQ(expected, NinjamProductionBoundaryFixture::AudioPosition(*takes[i]));
			}
		}
	}
}

TEST(AudioHostMetronome, LocalMasterUsesRenderPhaseAndStopsOnReclock)
{
	audio::AudioHost host{ io::UserConfig{} };
	audio::NinjamAudioBoundaryTestAccess::ConfigureMetronome(host);
	EXPECT_FALSE(host.NinjamMetronomeEnabled());
	host.SetNinjamMetronomeEnabled(true);
	Timer clock;
	clock.SetQuantisation(1000u, Timer::QUANTISE_OFF);
	clock.SetSeedSourceLength(4001ul);
	clock.SetMasterLoopSampleOffset(990ul);
	std::array<float, 64> output{};
	audio::NinjamAudioBoundaryTestAccess::Click(host, output.data(), 64u, {}, &clock);
	EXPECT_TRUE(std::all_of(output.begin(), output.begin() + 10, [](float x) { return x == 0.0f; }));
	EXPECT_TRUE(std::any_of(output.begin() + 10, output.end(), [](float x) { return x != 0.0f; }));
	clock.Clear();
	output.fill(0.0f);
	audio::NinjamAudioBoundaryTestAccess::Click(host, output.data(), 64u, {}, &clock);
	EXPECT_EQ(output, (std::array<float, 64>{}));
}

TEST(AudioHostMetronome, ConnectedServerPhaseWinsOverLocalAndInvalidRemoteStaysSilent)
{
	audio::AudioHost host{ io::UserConfig{} };
	audio::NinjamAudioBoundaryTestAccess::ConfigureMetronome(host);
	host.SetNinjamMetronomeEnabled(true);
	Timer clock;
	clock.SetQuantisation(1000u, Timer::QUANTISE_MULTIPLE);
	clock.SetSeedSourceLength(4000ul);
	ninjam::NinjamTiming remote;
	remote.IsConnected = true; remote.IsValid = true;
	remote.IntervalLengthSamps = 4000u; remote.Bpi = 4u;
	remote.IntervalPositionSamps = 500u;
	std::array<float, 64> output{};
	audio::NinjamAudioBoundaryTestAccess::Click(host, output.data(), 64u, remote, &clock);
	EXPECT_EQ(output, (std::array<float, 64>{}));
	remote.IntervalPositionSamps = 0u;
	audio::NinjamAudioBoundaryTestAccess::Click(host, output.data(), 64u, remote, &clock);
	EXPECT_TRUE(std::any_of(output.begin(), output.end(), [](float x) { return x != 0.0f; }));
	// Identical geometry on returning local still discards the server's click tail.
	clock.SetMasterLoopSampleOffset(500ul);
	output.fill(0.0f);
	audio::NinjamAudioBoundaryTestAccess::Click(host, output.data(), 64u, {}, &clock);
	EXPECT_EQ(output, (std::array<float, 64>{}));
	audio::NinjamAudioBoundaryTestAccess::Click(host, output.data(), 64u, remote, &clock);
	remote.IsValid = false;
	output.fill(0.0f);
	audio::NinjamAudioBoundaryTestAccess::Click(host, output.data(), 64u, remote, &clock);
	EXPECT_EQ(output, (std::array<float, 64>{}));
}
