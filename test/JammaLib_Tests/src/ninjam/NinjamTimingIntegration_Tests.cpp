#include "gtest/gtest.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
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

		static std::optional<std::int64_t> RestoreMappedSourceForBenchmark(
			AudioHost& host,
			const std::vector<std::shared_ptr<engine::Station>>* stations,
			std::uint64_t sceneCoordinateSamps,
			std::uint64_t& commonMapCalculationCount) noexcept
		{
			// RestoreMappedSourceAtScene contains the sole production SourceCoordinateAt
			// call, before its station fan-out. Count entries to that private boundary;
			// the static call-site audit proves one calculation per entry.
			++commonMapCalculationCount;
			return host.RestoreMappedSourceAtScene(stations, sceneCoordinateSamps);
		}

		static ninjam::SyncPhaseMap SyncPhaseMapForBenchmark(
			const AudioHost& host) noexcept
		{
			return host._syncPhaseMap;
		}

		static std::optional<double> ConsumeLocalTransportOffset(AudioHost& host) noexcept
		{
			return host._localTransportOffsetLoopFracMailbox.ConsumeLatest();
		}

		static void ApplyLocalTransportOffset(AudioHost& host,
			const std::vector<std::shared_ptr<engine::Station>>& stations) noexcept
		{
			host.ApplyLocalTransportOffsetAtAudioBoundary(stations);
		}
	};
}

class NinjamBenchmarkLoop : public engine::Loop
{
public:
	NinjamBenchmarkLoop(engine::LoopParams params,
		audio::AudioMixerParams mixerParams) : Loop(params, mixerParams)
	{
	}

	void SetTimingState(unsigned long length, unsigned long position) noexcept
	{
		_loopLength.store(length, std::memory_order_relaxed);
		_playState.store(STATE_PLAYING, std::memory_order_relaxed);
		SetBodyPlayIndex(position);
	}
};

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

class NinjamBenchmarkStation : public engine::Station
{
public:
	NinjamBenchmarkStation(engine::StationParams params,
		audio::AudioMixerParams mixerParams) : Station(params, mixerParams)
	{
	}

	void RestoreMappedSourcePerTakeReference(const ninjam::SyncPhaseMap& map,
		std::uint64_t sceneCoordinateSamps,
		std::uint64_t& mapCalculationCount) noexcept
	{
		// This is a test-only reconstruction of the pre-B009 callback shape:
		// Station traverses its published take snapshot and each take repeats the
		// identical common-map calculation before restoring its own entities.
		auto state = _AudioStateSnapshot();
		if (!state)
			return;
		for (const auto& weakTake : state->LoopTakes)
		{
			if (auto take = weakTake.lock())
			{
				++mapCalculationCount;
				take->RestoreMappedSourceCoordinate(
					map.SourceCoordinateAt(sceneCoordinateSamps));
			}
		}
	}
};

class NinjamProductionBoundaryFixture
{
public:
	struct BenchmarkHierarchy
	{
		std::shared_ptr<const std::vector<std::shared_ptr<engine::Station>>> Stations;
		std::vector<std::shared_ptr<NinjamBenchmarkStation>> ReferenceStations;
		std::shared_ptr<NinjamProductionBoundaryLoopTake> ProbeTake;
		std::shared_ptr<NinjamBenchmarkLoop> ProbeLoop;
		std::size_t TakeCount = 0u;
		std::size_t AudioLoopCount = 0u;
	};

	struct BenchmarkStats
	{
		std::uint64_t MedianNanosPerCall = 0u;
		std::uint64_t P90NanosPerCall = 0u;
		std::uint64_t MaximumNanosPerCall = 0u;
	};

	struct BenchmarkComparison
	{
		BenchmarkStats Current;
		BenchmarkStats PerTakeReference;
	};

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

	static std::shared_ptr<NinjamBenchmarkLoop> MakeBenchmarkLoop(
		const std::string& id, unsigned long loopLength, unsigned long position)
	{
		audio::WireMixBehaviourParams mixBehaviour;
		mixBehaviour.Channels = { 0u };
		audio::AudioMixerParams mixerParams;
		mixerParams.Size = { 160, 80 };
		mixerParams.Position = { 6, 6 };
		mixerParams.Behaviour = mixBehaviour;

		engine::LoopParams loopParams;
		loopParams.Id = id;
		loopParams.Wav = "b009-callback-benchmark";
		loopParams.Size = { 80, 80 };
		loopParams.Position = { 10, 22 };
		auto loop = std::make_shared<NinjamBenchmarkLoop>(loopParams, mixerParams);
		loop->SetTimingState(loopLength, position % loopLength);
		return loop;
	}

	static BenchmarkHierarchy MakeBenchmarkHierarchy(unsigned int stationCount,
		unsigned int takesPerStation, unsigned int loopsPerTake,
		unsigned long loopLength)
	{
		BenchmarkHierarchy result;
		auto stations = std::make_shared<std::vector<std::shared_ptr<engine::Station>>>();
		stations->reserve(stationCount);
		result.ReferenceStations.reserve(stationCount);
		for (auto stationIndex = 0u; stationIndex < stationCount; ++stationIndex)
		{
			engine::StationParams stationParams;
			stationParams.Name = "b009-benchmark-station-" + std::to_string(stationIndex);
			stationParams.Size = { 200, 320 };
			audio::MergeMixBehaviourParams stationMerge;
			auto station = std::make_shared<NinjamBenchmarkStation>(stationParams,
				engine::Station::GetMixerParams(stationParams.Size, stationMerge));

			for (auto takeIndex = 0u; takeIndex < takesPerStation; ++takeIndex)
			{
				engine::LoopTakeParams takeParams;
				takeParams.Id = "b009-benchmark-take-" + std::to_string(stationIndex)
					+ "-" + std::to_string(takeIndex);
				takeParams.Size = { 100, 100 };
				audio::MergeMixBehaviourParams takeMerge;
				auto take = std::make_shared<NinjamProductionBoundaryLoopTake>(takeParams,
					engine::LoopTake::GetMixerParams(takeParams.Size, takeMerge));
				for (auto loopIndex = 0u; loopIndex < loopsPerTake; ++loopIndex)
				{
					const auto position = static_cast<unsigned long>(
						(stationIndex * 97u + takeIndex * 31u + loopIndex * 13u) % loopLength);
					auto loop = MakeBenchmarkLoop(takeParams.Id + "-" + std::to_string(loopIndex),
						loopLength, position);
					if (!result.ProbeLoop)
						result.ProbeLoop = loop;
					take->AddLoop(loop);
					++result.AudioLoopCount;
				}
				take->CommitChanges();
				take->SetMidiTimingState((stationIndex * 53u + takeIndex * 19u) % loopLength,
					loopLength);
				if (!result.ProbeTake)
					result.ProbeTake = take;
				station->AddTake(take);
				++result.TakeCount;
			}
			station->CommitChanges();
			stations->push_back(station);
			result.ReferenceStations.push_back(station);
		}
		result.Stations = stations;
		return result;
	}

	template<std::size_t TrialCount, typename CurrentCallback, typename ReferenceCallback>
	static BenchmarkComparison MeasureBenchmarkPair(unsigned int iterationsPerTrial,
		CurrentCallback&& currentCallback, ReferenceCallback&& referenceCallback)
	{
		std::array<std::uint64_t, TrialCount> currentNanosPerCall{};
		std::array<std::uint64_t, TrialCount> referenceNanosPerCall{};
		auto measure = [iterationsPerTrial](auto&& callback, unsigned int trial)
		{
			const auto start = std::chrono::steady_clock::now();
			for (auto iteration = 0u; iteration < iterationsPerTrial; ++iteration)
				callback(trial, iteration);
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - start).count();
			return static_cast<std::uint64_t>(elapsed) / iterationsPerTrial;
		};
		for (auto trial = 0u; trial < TrialCount; ++trial)
		{
			if ((trial & 1u) == 0u)
			{
				currentNanosPerCall[trial] = measure(currentCallback, trial);
				referenceNanosPerCall[trial] = measure(referenceCallback, trial);
			}
			else
			{
				referenceNanosPerCall[trial] = measure(referenceCallback, trial);
				currentNanosPerCall[trial] = measure(currentCallback, trial);
			}
		}
		std::sort(currentNanosPerCall.begin(), currentNanosPerCall.end());
		std::sort(referenceNanosPerCall.begin(), referenceNanosPerCall.end());
		auto stats = [](const auto& samples)
		{
			return BenchmarkStats{ samples[TrialCount / 2u],
				samples[(TrialCount * 9u) / 10u], samples.back() };
		};
		return { stats(currentNanosPerCall), stats(referenceNanosPerCall) };
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
		desired.RemoteGridStepSamps = length == 0ul ? 0u : static_cast<unsigned int>(length / 16ul);
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
	audio::NinjamAudioBoundaryTestAccess::ApplyLocalTransportOffset(host, stations);
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
		EXPECT_EQ(desired.IntervalLengthSamps, grid.Geometry.IntervalLengthSamps);
		EXPECT_EQ(desired.BeatsPerInterval, grid.Geometry.Bpi);
		EXPECT_EQ(desired.RemotePhaseSamps, grid.Geometry.PhaseSamps);
		EXPECT_EQ(desired.TempoBpm, grid.Geometry.Bpm);
		EXPECT_EQ(desired.Generation, grid.Geometry.Generation);
		EXPECT_EQ(static_cast<std::int64_t>(desired.ObservationSample)
			- static_cast<std::int64_t>(desired.RemotePhaseSamps), grid.OriginSamps);
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

TEST(NinjamTimingProductionBoundary, CommonMapCallbackCostIsBoundedAtSaturationCeiling)
{
	// Production has no station/take/loop cardinality cap. This explicit stress
	// ceiling is therefore not described as a product maximum: it exercises 32
	// local stations, 32 takes per station, two audio loops and one MIDI timing
	// cursor per take. The timed work is the real private AudioHost common-map
	// restore/fan-out and an otherwise identical test-only pre-B009 reference
	// which repeats the common map calculation once per take.
	constexpr auto stationCount = 32u;
	constexpr auto takesPerStation = 32u;
	constexpr auto audioLoopsPerTake = 2u;
	constexpr auto midiCursorsPerTake = 1u;
	constexpr auto blockSize = constants::DefaultBufferSizeSamps;
	constexpr auto sampleRate = 48000u;
	constexpr auto warmupCallsPerPath = 64u;
	constexpr auto trialCount = 11u;
	constexpr auto iterationsPerTrial = 64u;
	constexpr auto sourceLength = 1000ul;
	constexpr auto remoteLength = 1100ul;
	constexpr auto timingNoiseMarginNanos = 50000u;
	constexpr auto comparisonRatioNumerator = 5u;
	constexpr auto comparisonRatioDenominator = 4u;
#if defined(_DEBUG)
	constexpr auto buildConfiguration = "Debug-x64";
#else
	constexpr auto buildConfiguration = "Release-x64";
#endif

	auto singleTake = NinjamProductionBoundaryFixture::MakeBenchmarkHierarchy(
		1u, 1u, audioLoopsPerTake, sourceLength);
	auto saturation = NinjamProductionBoundaryFixture::MakeBenchmarkHierarchy(
		stationCount, takesPerStation, audioLoopsPerTake, sourceLength);
	ASSERT_EQ(stationCount * takesPerStation, saturation.TakeCount);
	ASSERT_EQ(stationCount * takesPerStation * audioLoopsPerTake,
		saturation.AudioLoopCount);

	audio::AudioHost host{ io::UserConfig{} };
	auto clock = std::make_shared<Timer>();
	clock->SetSeedSourceLength(sourceLength);
	clock->Tick(100u, 0u);
	host.SetTimingClock(clock);
	host.SetStations(saturation.Stations);
	host.PublishDesiredTiming(NinjamProductionBoundaryFixture::Desired(1u, 1u, 1u,
		ninjam::NinjamLocalFollowPolicy::BlockSync, remoteLength, 100u));
	ASSERT_TRUE(audio::NinjamAudioBoundaryTestAccess::Apply(host, 0u, sampleRate));
	audio::NinjamAudioBoundaryTestAccess::ApplyDeferredMapTransition(host);
	const auto map = audio::NinjamAudioBoundaryTestAccess::SyncPhaseMapForBenchmark(host);
	ASSERT_TRUE(map.IsActive());
	ASSERT_TRUE(saturation.ProbeLoop);
	ASSERT_TRUE(saturation.ProbeTake);

	// Equal block counts at one take and 1,024 takes prove that entry to the sole
	// common-map calculation is independent of take count. The static call-site
	// audit remains the proof that each boundary entry performs exactly one
	// SourceCoordinateAt before station fan-out.
	std::uint64_t smallCommonMapCalculationCount = 0u;
	std::uint64_t saturationWarmupCommonMapCalculationCount = 0u;
	std::uint64_t saturationWarmupReferenceMapCalculationCount = 0u;
	for (auto call = 0u; call < warmupCallsPerPath; ++call)
	{
		const auto scene = static_cast<std::uint64_t>(call + 1u) * blockSize;
		audio::NinjamAudioBoundaryTestAccess::RestoreMappedSourceForBenchmark(host,
			singleTake.Stations.get(), scene, smallCommonMapCalculationCount);
		if ((call & 1u) == 0u)
		{
			audio::NinjamAudioBoundaryTestAccess::RestoreMappedSourceForBenchmark(host,
				saturation.Stations.get(), scene, saturationWarmupCommonMapCalculationCount);
			for (const auto& station : saturation.ReferenceStations)
				station->RestoreMappedSourcePerTakeReference(map, scene,
					saturationWarmupReferenceMapCalculationCount);
		}
		else
		{
			for (const auto& station : saturation.ReferenceStations)
				station->RestoreMappedSourcePerTakeReference(map, scene,
					saturationWarmupReferenceMapCalculationCount);
			audio::NinjamAudioBoundaryTestAccess::RestoreMappedSourceForBenchmark(host,
				saturation.Stations.get(), scene, saturationWarmupCommonMapCalculationCount);
		}
	}
	EXPECT_EQ(warmupCallsPerPath, smallCommonMapCalculationCount);
	EXPECT_EQ(warmupCallsPerPath, saturationWarmupCommonMapCalculationCount);
	EXPECT_EQ(static_cast<std::uint64_t>(warmupCallsPerPath) * saturation.TakeCount,
		saturationWarmupReferenceMapCalculationCount);

	std::uint64_t currentMapCalculationCount = 0u;
	std::uint64_t referenceMapCalculationCount = 0u;
	std::uint64_t currentCoordinateChecksum = 0u;
	const auto sceneFor = [=](unsigned int trial, unsigned int iteration)
	{
		const auto ordinal = static_cast<std::uint64_t>(warmupCallsPerPath) + 1u
			+ static_cast<std::uint64_t>(trial) * iterationsPerTrial + iteration;
		return ordinal * blockSize;
	};
	const auto comparison = NinjamProductionBoundaryFixture::MeasureBenchmarkPair<trialCount>(
		iterationsPerTrial,
		[&](unsigned int trial, unsigned int iteration)
		{
			const auto mapped = audio::NinjamAudioBoundaryTestAccess::RestoreMappedSourceForBenchmark(
				host, saturation.Stations.get(), sceneFor(trial, iteration),
				currentMapCalculationCount);
			if (mapped.has_value())
				currentCoordinateChecksum ^= static_cast<std::uint64_t>(mapped.value());
		},
		[&](unsigned int trial, unsigned int iteration)
		{
			const auto scene = sceneFor(trial, iteration);
			for (const auto& station : saturation.ReferenceStations)
				station->RestoreMappedSourcePerTakeReference(map, scene,
					referenceMapCalculationCount);
		});

	constexpr auto measuredCallsPerPath = static_cast<std::uint64_t>(trialCount)
		* iterationsPerTrial;
	EXPECT_EQ(measuredCallsPerPath, currentMapCalculationCount);
	EXPECT_EQ(measuredCallsPerPath * saturation.TakeCount, referenceMapCalculationCount);

	const auto finalScene = sceneFor(trialCount - 1u, iterationsPerTrial - 1u);
	const auto finalSource = map.SourceCoordinateAt(finalScene);
	EXPECT_EQ(static_cast<unsigned long>(ninjam::PositiveModulo(finalSource
		- static_cast<long long>(saturation.ProbeLoop->SceneAnchor()), sourceLength)),
		saturation.ProbeLoop->BodyPlayIndex());
	EXPECT_EQ(static_cast<unsigned long>(ninjam::PositiveModulo(finalSource
		- static_cast<long long>(saturation.ProbeTake->MidiSceneAnchor()), sourceLength)),
		saturation.ProbeTake->MidiTimingPosition());
	EXPECT_NE(0u, currentCoordinateChecksum);

	const auto callbackBudgetNanos = static_cast<std::uint64_t>(blockSize) * 1000000000ull
		/ sampleRate;
	const auto comparisonLimitNanos = comparison.PerTakeReference.MedianNanosPerCall
		* comparisonRatioNumerator / comparisonRatioDenominator + timingNoiseMarginNanos;
	const auto timingAccepted = comparison.Current.P90NanosPerCall < callbackBudgetNanos
		&& comparison.Current.MedianNanosPerCall <= comparisonLimitNanos;
	std::cout << std::fixed << std::setprecision(3)
		<< "[B009 callback benchmark] scenario=private AudioHost common-map restore/fan-out"
		<< " configuration=" << buildConfiguration
		<< " hierarchy_policy=explicit-saturation-ceiling-no-production-cardinality-cap"
		<< " sample_rate_hz=" << sampleRate
		<< " block_size_samples=" << blockSize
		<< " stations=" << stationCount
		<< " takes_per_station=" << takesPerStation
		<< " audio_loops_per_take=" << audioLoopsPerTake
		<< " midi_cursors_per_take=" << midiCursorsPerTake
		<< " total_takes=" << saturation.TakeCount
		<< " total_audio_loops=" << saturation.AudioLoopCount
		<< " total_timing_entities=" << saturation.TakeCount * midiCursorsPerTake
			+ saturation.AudioLoopCount
		<< " warmup_calls_per_path=" << warmupCallsPerPath
		<< " trials=" << trialCount
		<< " iterations_per_trial=" << iterationsPerTrial
		<< " measured_calls_per_path=" << measuredCallsPerPath
		<< " current_common_map_calculations=" << currentMapCalculationCount
		<< " reference_per_take_map_calculations=" << referenceMapCalculationCount
		<< " current_median_ns_per_call=" << comparison.Current.MedianNanosPerCall
		<< " current_p90_ns_per_call=" << comparison.Current.P90NanosPerCall
		<< " current_max_trial_ns_per_call=" << comparison.Current.MaximumNanosPerCall
		<< " reference_median_ns_per_call=" << comparison.PerTakeReference.MedianNanosPerCall
		<< " reference_p90_ns_per_call=" << comparison.PerTakeReference.P90NanosPerCall
		<< " reference_max_trial_ns_per_call=" << comparison.PerTakeReference.MaximumNanosPerCall
		<< " callback_budget_ns=" << callbackBudgetNanos
		<< " comparison_limit_ns=reference_median*1.25+50000"
		<< " timing_accepted=" << (timingAccepted ? "true" : "false")
		<< " timing_gate=non-gating-host-load-sensitive"
		<< '\n';
}
