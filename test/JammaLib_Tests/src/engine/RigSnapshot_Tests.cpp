#include "engine/RigCoordinator.h"
#include "engine/StationManagement.h"
#include "gui/GuiStationControls.h"
#include <glm/gtc/matrix_transform.hpp>
#include "../../../../Jamma/src/RigDefaultSave.h"
#include "audio/AudioHost.h"
#include "engine/Station.h"
#include "engine/PerformanceGroupService.h"
#include "gui/GuiHud.h"
#include "base/PublishedAudioSnapshot.h"
#include "utils/Timer.h"
#include <array>
#include <future>
#include <thread>

namespace audio
{
	class RigAudioBoundaryTestAccess
	{
	public:
		static void ApplyPending(AudioHost& host) noexcept
		{
			host.ApplyPendingRigSnapshotAtAudioBoundary();
		}

		static void PublishTransition(AudioHost& host) noexcept
		{
			host.PublishRigTriggerTransitionAtAudioBoundary();
		}
		static bool EnterCallback(AudioHost& host) noexcept { return host.TryEnterCallback(); }
		static void LeaveCallback(AudioHost& host) noexcept { host.LeaveCallback(); }
		static bool ApplyDesiredTiming(AudioHost& host) noexcept
		{
			return host.ApplyDesiredTimingAtAudioBoundary(0u, 48000u);
		}
		static void LateCallback(AudioHost& host, float* output, unsigned int frames,
			unsigned int channels) noexcept
		{
			host._callbackOutputChannels.store(channels, std::memory_order_release);
			host.AudioCallback(output, nullptr, frames, 0.0, 0u, &host);
		}
		static auto BorrowStations(AudioHost& host) noexcept
		{
			return host._audioStations.borrow();
		}
		static auto BorrowClock(AudioHost& host) noexcept
		{
			return host._timingClock.borrow();
		}
		static bool CachedStationReadSucceeded(const AudioHost& host) noexcept
		{
			return host._lastAudioStations.ReadSucceeded();
		}
		static bool HasCachedStations(const AudioHost& host) noexcept
		{
			return static_cast<bool>(host._lastAudioStations);
		}
		static bool CachedClockReadSucceeded(const AudioHost& host) noexcept
		{
			return host._lastAudioTimingClock.ReadSucceeded();
		}
		static bool HasCachedClock(const AudioHost& host) noexcept
		{
			return static_cast<bool>(host._lastAudioTimingClock);
		}
		static bool AdmissionClosed(const AudioHost& host) noexcept
		{
			return (host._callbackAdmission.load(std::memory_order_acquire)
				& AudioHost::_CallbackAdmissionClosed) != 0u;
		}
		static void ConfigureCallback(AudioHost& host, const std::shared_ptr<utils::Timer>& clock)
		{
			AudioStreamParams params{};
			params.SampleRate = 48000u;
			params.BufSize = 4u;
			params.NumOutputChannels = 2u;
			host._preparedStreamParams = params;
			host._callbackOutputChannels.store(2u, std::memory_order_release);
			host._tickCallback = [clock](Time, unsigned int frames,
				const std::optional<io::UserConfig>&,
				const std::optional<AudioStreamParams>&)
			{
				clock->Tick(frames, 0u);
			};
		}
		static void RunBlock(AudioHost& host, float* output) noexcept
		{
			host.AudioCallback(output, nullptr, 4u, 0.0, 0u, &host);
		}
	};
}

TEST(PerformanceGroupBoundary, RunningCallbackAppliesNewestPublicationAndJobCollectsOwnership)
{
	audio::AudioHost host(io::UserConfig{});
	auto stations = std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>();
	auto clock = std::make_shared<utils::Timer>();
	host.SetStations(stations);
	host.SetTimingClock(clock);
	audio::RigAudioBoundaryTestAccess::ConfigureCallback(host, clock);
	engine::StationParams params;
	params.Size = { 160u, 320u };
	auto station = std::make_shared<engine::Station>(params,
		engine::Station::GetMixerParams(params.Size, audio::MergeMixBehaviourParams{}));
	auto take = station->AddTake();
	auto first = std::make_shared<engine::GroupMuteApplication>();
	first->Version = 1u;
	first->Takes.push_back({ take, true });
	host.PublishGroupMuteApplication(first);
	EXPECT_FALSE(take->IsMuted());
	float output[8]{};
	audio::RigAudioBoundaryTestAccess::RunBlock(host, output);
	EXPECT_TRUE(take->IsMuted());
	EXPECT_EQ(host.GroupMuteReceipt(), 1u);
	std::weak_ptr<const engine::GroupMuteApplication> lifetime = first;
	first.reset();
	host.CollectRetiredSnapshots();
	EXPECT_TRUE(lifetime.expired());
	// Two newer publications before a block apply only the latest desired value.
	auto second = std::make_shared<engine::GroupMuteApplication>();
	second->Version = 2u;
	second->Takes.push_back({ take, true });
	host.PublishGroupMuteApplication(second);
	auto third = std::make_shared<engine::GroupMuteApplication>();
	third->Version = 3u;
	third->Takes.push_back({ take, false });
	host.PublishGroupMuteApplication(third);
	audio::RigAudioBoundaryTestAccess::RunBlock(host, output);
	EXPECT_FALSE(take->IsMuted());
	EXPECT_EQ(host.GroupMuteReceipt(), 3u);
	// Membership-stamped work cannot apply against a different accepted rig.
	auto stale = std::make_shared<engine::GroupMuteApplication>();
	stale->Version = 4u;
	stale->TopologyRevision = 99u;
	stale->Takes.push_back({ take, true });
	host.PublishGroupMuteApplication(stale);
	audio::RigAudioBoundaryTestAccess::RunBlock(host, output);
	EXPECT_FALSE(take->IsMuted());
	EXPECT_EQ(host.GroupMuteReceipt(), 3u);
}

TEST(AudioCallbackAdmission, SnapshotBorrowFailureReusesLastSuccessfulBlockState)
{
	audio::AudioHost host(io::UserConfig{});
	auto stations = std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>();
	auto clock = std::make_shared<utils::Timer>();
	host.SetStations(stations);
	host.SetTimingClock(clock);
	audio::RigAudioBoundaryTestAccess::ConfigureCallback(host, clock);

	float output[8]{};
	audio::RigAudioBoundaryTestAccess::RunBlock(host, output);
	EXPECT_EQ(4u, host.GetAudioSampleCounter());
	EXPECT_EQ(4u, clock->SceneSamplePos());

	std::array<base::PublishedAudioSnapshot<const audio::StationInventory>::Lease, 7u>
		stationLeases;
	std::array<base::PublishedAudioSnapshot<utils::Timer>::Lease, 7u> clockLeases;
	for (auto& lease : stationLeases)
	{
		lease = audio::RigAudioBoundaryTestAccess::BorrowStations(host);
		ASSERT_TRUE(lease.ReadSucceeded());
	}
	for (auto& lease : clockLeases)
	{
		lease = audio::RigAudioBoundaryTestAccess::BorrowClock(host);
		ASSERT_TRUE(lease.ReadSucceeded());
	}

	audio::RigAudioBoundaryTestAccess::RunBlock(host, output);
	EXPECT_EQ(8u, host.GetAudioSampleCounter());
	EXPECT_EQ(8u, clock->SceneSamplePos());
	EXPECT_EQ(2u, host.AudioCallbackHeartbeat());

	for (auto& lease : stationLeases) lease.reset();
	for (auto& lease : clockLeases) lease.reset();
	host.SetStations(nullptr);
	host.SetTimingClock(nullptr);
	audio::RigAudioBoundaryTestAccess::RunBlock(host, output);
	EXPECT_TRUE(audio::RigAudioBoundaryTestAccess::CachedStationReadSucceeded(host));
	EXPECT_FALSE(audio::RigAudioBoundaryTestAccess::HasCachedStations(host));
	EXPECT_TRUE(audio::RigAudioBoundaryTestAccess::CachedClockReadSucceeded(host));
	EXPECT_FALSE(audio::RigAudioBoundaryTestAccess::HasCachedClock(host));
}

TEST(AudioCallbackAdmission, StartupBorrowFailureSilencesButAdvancesPhysicalProgress)
{
	audio::AudioHost host(io::UserConfig{});
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>());
	auto clock = std::make_shared<utils::Timer>();
	host.SetTimingClock(clock);
	audio::RigAudioBoundaryTestAccess::ConfigureCallback(host, clock);

	std::array<base::PublishedAudioSnapshot<const audio::StationInventory>::Lease, 8u>
		stationLeases;
	std::array<base::PublishedAudioSnapshot<utils::Timer>::Lease, 8u> clockLeases;
	for (auto& lease : stationLeases)
	{
		lease = audio::RigAudioBoundaryTestAccess::BorrowStations(host);
		ASSERT_TRUE(lease.ReadSucceeded());
	}
	for (auto& lease : clockLeases)
	{
		lease = audio::RigAudioBoundaryTestAccess::BorrowClock(host);
		ASSERT_TRUE(lease.ReadSucceeded());
	}

	float output[8];
	std::fill_n(output, 8u, 1.0f);
	audio::RigAudioBoundaryTestAccess::RunBlock(host, output);
	for (const auto sample : output)
		EXPECT_EQ(0.0f, sample);
	EXPECT_EQ(4u, host.GetAudioSampleCounter());
	EXPECT_EQ(4u, clock->SceneSamplePos());
	EXPECT_EQ(1u, host.AudioCallbackHeartbeat());
}

TEST(AudioCallbackAdmission, ClosingWaitsForAdmittedCallbackAndRejectsLateCallbacks)
{
	audio::AudioHost host(io::UserConfig{});
	EXPECT_TRUE(audio::RigAudioBoundaryTestAccess::EnterCallback(host));
	EXPECT_FALSE(audio::RigAudioBoundaryTestAccess::EnterCallback(host));
	host.CloseCallbackAdmission();
	EXPECT_FALSE(host.CallbackAdmissionIsQuiescent());
	EXPECT_FALSE(host.ReopenCallbackAdmission());
	float output[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
	audio::RigAudioBoundaryTestAccess::LateCallback(host, output, 4u, 2u);
	for (const auto sample : output)
		EXPECT_EQ(0.0f, sample);
	EXPECT_EQ(0u, host.AudioCallbackHeartbeat());
	audio::RigAudioBoundaryTestAccess::LeaveCallback(host);
	EXPECT_TRUE(host.CallbackAdmissionIsQuiescent());
	EXPECT_FALSE(audio::RigAudioBoundaryTestAccess::EnterCallback(host));
	EXPECT_TRUE(host.ReopenCallbackAdmission());
	EXPECT_TRUE(audio::RigAudioBoundaryTestAccess::EnterCallback(host));
	audio::RigAudioBoundaryTestAccess::LeaveCallback(host);
}

TEST(AudioCallbackAdmission, RepeatedCloseCannotReopenAnActiveReader)
{
	audio::AudioHost host(io::UserConfig{});
	host.CloseCallbackAdmission();
	host.CloseCallbackAdmission();
	EXPECT_TRUE(host.CallbackAdmissionIsQuiescent());
	EXPECT_TRUE(host.ReopenCallbackAdmission());
	EXPECT_FALSE(host.ReopenCallbackAdmission());
	ASSERT_TRUE(audio::RigAudioBoundaryTestAccess::EnterCallback(host));
	host.CloseCallbackAdmission();
	host.CloseCallbackAdmission();
	EXPECT_FALSE(host.CallbackAdmissionIsQuiescent());
	audio::RigAudioBoundaryTestAccess::LeaveCallback(host);
	EXPECT_TRUE(host.CallbackAdmissionIsQuiescent());
}

TEST(AudioCallbackAdmission, CloseWaitsForAnAdmittedCallbackBeforeReturning)
{
	audio::AudioHost host(io::UserConfig{});
	ASSERT_TRUE(audio::RigAudioBoundaryTestAccess::EnterCallback(host));
	std::promise<void> closeStarted;
	auto closeStartedFuture = closeStarted.get_future();
	auto closeFinished = std::async(std::launch::async, [&] {
		closeStarted.set_value();
		host.Close();
	});
	closeStartedFuture.wait();
	const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (!audio::RigAudioBoundaryTestAccess::AdmissionClosed(host)
		&& std::chrono::steady_clock::now() < closeDeadline)
		std::this_thread::yield();
	const auto admissionWasClosed = audio::RigAudioBoundaryTestAccess::AdmissionClosed(host);
	const auto returnedWhileCallbackActive = closeFinished.wait_for(std::chrono::milliseconds(20))
		== std::future_status::ready;
	audio::RigAudioBoundaryTestAccess::LeaveCallback(host);
	EXPECT_TRUE(admissionWasClosed);
	EXPECT_FALSE(returnedWhileCallbackActive);
	EXPECT_EQ(std::future_status::ready, closeFinished.wait_for(std::chrono::seconds(1)));
	closeFinished.get();
	EXPECT_TRUE(host.CallbackAdmissionIsQuiescent());
	EXPECT_FALSE(audio::RigAudioBoundaryTestAccess::EnterCallback(host));
}

TEST(AudioCallbackAdmission, DesiredTimingRemainsPendingWhenSnapshotBorrowIsExhausted)
{
	audio::AudioHost host(io::UserConfig{});
	host.SetStations(std::make_shared<const std::vector<std::shared_ptr<engine::Station>>>());
	std::array<base::PublishedAudioSnapshot<const audio::StationInventory>::Lease, 8u>
		leases;
	for (auto& lease : leases)
	{
		lease = host.BorrowStationsSnapshot();
		ASSERT_TRUE(lease.ReadSucceeded());
	}

	ninjam::NinjamDesiredTransportState desired{};
	desired.Version = 1u;
	desired.SessionEpoch = 1u;
	desired.LocalFollowPolicy = ninjam::NinjamLocalFollowPolicy::NoSync;
	host.PublishDesiredTiming(desired);
	EXPECT_FALSE(audio::RigAudioBoundaryTestAccess::ApplyDesiredTiming(host));
	EXPECT_FALSE(host.LastAppliedDesiredTiming().has_value());

	for (auto& lease : leases)
		lease.reset();
	EXPECT_TRUE(audio::RigAudioBoundaryTestAccess::ApplyDesiredTiming(host));
	ASSERT_TRUE(host.LastAppliedDesiredTiming().has_value());
	EXPECT_EQ(1u, host.LastAppliedDesiredTiming()->Version);
}

TEST(AudioCallbackAdmission, QuiescencePublishesExitedReadersPayload)
{
	audio::AudioHost host(io::UserConfig{});
	std::promise<bool> admitted;
	std::promise<void> releaseReader;
	auto release = releaseReader.get_future();
	unsigned int audioOwnedPayload = 0u;
	std::thread reader([&] {
		const auto entered = audio::RigAudioBoundaryTestAccess::EnterCallback(host);
		admitted.set_value(entered);
		release.wait();
		if (entered)
		{
			audioOwnedPayload = 42u;
			audio::RigAudioBoundaryTestAccess::LeaveCallback(host);
		}
	});
	EXPECT_TRUE(admitted.get_future().get());
	host.CloseCallbackAdmission();
	EXPECT_FALSE(host.CallbackAdmissionIsQuiescent());
	EXPECT_FALSE(host.ReopenCallbackAdmission());
	float output[2] = { 1.0f, 1.0f };
	audio::RigAudioBoundaryTestAccess::LateCallback(host, output, 1u, 2u);
	EXPECT_EQ(0.0f, output[0]);
	EXPECT_EQ(0.0f, output[1]);
	releaseReader.set_value();
	// Test-only wait: production recovery retries on subsequent job ticks.
	while (!host.CallbackAdmissionIsQuiescent())
		std::this_thread::yield();
	EXPECT_EQ(42u, audioOwnedPayload);
	reader.join();
	EXPECT_TRUE(host.ReopenCallbackAdmission());
}

class RigSnapshotTest : public testing::Test
{
protected:
	static io::RigFile EmptyRig()
	{
		auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson)).value();
		rig.Triggers.clear();
		return rig;
	}

	static io::JamFile::Station StationDescriptor(std::string name)
	{
		io::JamFile::Station station{};
		station.Name = std::move(name);
		return station;
	}

	static io::RigFile::Trigger TriggerDescriptor(std::string name,
		std::optional<std::string> target)
	{
		io::RigFile::Trigger trigger{};
		trigger.Id = name + "-id";
		trigger.Name = std::move(name);
		trigger.StationTarget = std::move(target);
		return trigger;
	}

	static io::RigFile::Binding KeyboardButton(unsigned int key)
	{
		io::RigFile::Binding binding;
		binding.Press = { key, 1u };
		binding.Release = { key, 0u };
		return binding;
	}

	static std::shared_ptr<engine::Station> RuntimeStation(std::string name)
	{
		engine::StationParams params;
		params.Name = std::move(name);
		params.Size = { 100u, 100u };
		audio::MergeMixBehaviourParams merge;
		return std::make_shared<engine::Station>(params, engine::Station::GetMixerParams(params.Size, merge));
	}

	static void ConfigureMidiRegistry(io::RigFile& rig, const std::vector<std::string>& ids)
	{
		for (const auto& id : ids)
			if (std::none_of(rig.User.Midi.Devices.begin(), rig.User.Midi.Devices.end(),
				[&id](const auto& device) { return device.Id == id; }))
				rig.User.Midi.Devices.push_back({ id, true, id });
	}

	static engine::RigCoordinator::SnapshotPtr BuildSnapshot(const io::RigFile& rig,
		const std::vector<io::JamFile::Station>& descriptors,
		unsigned int adcChannels,
		const std::vector<std::string>& midiDevices)
	{
		std::vector<std::shared_ptr<engine::Station>> stations;
		for (const auto& descriptor : descriptors)
			stations.push_back(RuntimeStation(descriptor.Name));
		auto canonical = rig;
		ConfigureMidiRegistry(canonical, midiDevices);
		for (const auto& trigger : canonical.Triggers)
			ConfigureMidiRegistry(canonical, trigger.CaptureInputs.Midi);
		engine::RigCoordinator coordinator;
		if (!coordinator.BuildInitial(canonical, descriptors, stations, adcChannels, midiDevices,
			engine::TriggerParams(), [](const io::RigFile&) { return false; }))
			return {};
		return coordinator.Accepted();
	}
};

TEST_F(RigSnapshotTest, BuildsEveryTriggerAndResolvesTargetsByName)
{
	auto rig = EmptyRig();
	rig.Triggers = {
		TriggerDescriptor("to-drums", "Drums"),
		TriggerDescriptor("to-bass", "Bass"),
		TriggerDescriptor("missing", "Missing"),
		TriggerDescriptor("unbound", std::nullopt)
	};
	const std::vector<io::JamFile::Station> stations = {
		StationDescriptor("Bass"),
		StationDescriptor("Drums")
	};

	const auto runtime = BuildSnapshot(rig, stations, 2u, {});

	ASSERT_TRUE(runtime);
	ASSERT_EQ(4u, runtime->Graph.Triggers.size());
	ASSERT_EQ(4u, runtime->Triggers.size());
	ASSERT_TRUE(runtime->Triggers[0].StationIndex.has_value());
	EXPECT_EQ(1u, runtime->Triggers[0].StationIndex.value());
	ASSERT_TRUE(runtime->Triggers[1].StationIndex.has_value());
	EXPECT_EQ(0u, runtime->Triggers[1].StationIndex.value());
	EXPECT_FALSE(runtime->Triggers[2].StationIndex.has_value());
	EXPECT_FALSE(runtime->Triggers[3].StationIndex.has_value());
	EXPECT_EQ(io::RigFileRouting::Warning::TargetMissing, runtime->Graph.Triggers[2].Reason);
	EXPECT_EQ(io::RigFileRouting::Warning::None, runtime->Graph.Triggers[3].Reason);
}

TEST_F(RigSnapshotTest, NullTargetStaysUnboundAndKeepsRuntimeIdentity)
{
	auto rig = EmptyRig();
	rig.Triggers = { TriggerDescriptor("legacy", std::nullopt) };

	const auto runtime = BuildSnapshot(rig,
		{ StationDescriptor("Station") },
		0u,
		{});

	ASSERT_TRUE(runtime);
	ASSERT_EQ(1u, runtime->Triggers.size());
	EXPECT_FALSE(runtime->Triggers[0].StationIndex.has_value());
	EXPECT_FALSE(runtime->Rig.Triggers[0].StationTarget.has_value());
	EXPECT_FALSE(runtime->Rig.Triggers[0].Id.empty());
	EXPECT_EQ(io::RigFileRouting::Warning::None, runtime->Graph.Triggers[0].Reason);
}

TEST_F(RigSnapshotTest, StartupNormalizationDoesNotPersistSelectedRig)
{
	auto rig = EmptyRig();
	rig.Triggers = { TriggerDescriptor("legacy", std::nullopt) };
	unsigned int writes = 0u;
	auto station = RuntimeStation("Station");
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(rig, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [&writes](const io::RigFile&) { ++writes; return true; }));
	EXPECT_EQ(0u, writes);
	ASSERT_TRUE(coordinator.Accepted());
	EXPECT_FALSE(coordinator.Accepted()->Rig.Triggers[0].StationTarget.has_value());
}

TEST_F(RigSnapshotTest, RuntimeAvailabilityUsesOpenedInputsWithoutRewritingSelectedRig)
{
	auto rig = EmptyRig();
	rig.User.Audio.NumChannelsIn = 4u;
	auto trigger = TriggerDescriptor("capture", "Station");
	trigger.CaptureInputs.Audio = { 0u, 3u };
	rig.Triggers = { trigger };
	auto station = RuntimeStation("Station");
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(rig, { StationDescriptor("Station") }, { station },
		4u, {}, engine::TriggerParams(), {}));
	ASSERT_TRUE(coordinator.Accepted());
	ASSERT_TRUE(coordinator.Accepted()->Graph.Triggers[0].Sources[1].Available);

	const auto runtime = coordinator.RefreshRuntimeAvailability(1u, {});
	ASSERT_TRUE(runtime);
	EXPECT_EQ(4u, runtime->Rig.User.Audio.NumChannelsIn);
	EXPECT_EQ(std::vector<unsigned int>({ 0u, 3u }), runtime->Rig.Triggers[0].CaptureInputs.Audio);
	ASSERT_EQ(2u, runtime->Graph.Triggers[0].Sources.size());
	EXPECT_TRUE(runtime->Graph.Triggers[0].Sources[0].Available);
	EXPECT_FALSE(runtime->Graph.Triggers[0].Sources[1].Available);

	EXPECT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(runtime->Rig));
	ASSERT_TRUE(coordinator.Staged());
	EXPECT_FALSE(coordinator.Staged()->Graph.Triggers[0].Sources[1].Available);
}

TEST_F(RigSnapshotTest, ReopenedRegistryInputRestoresCaptureWithoutReplacingTriggerOrSavedSelectors)
{
	auto rig = EmptyRig();
	rig.User.Midi.Devices = {{"Hardware keys 0", true, "keys"}};
	auto definition = TriggerDescriptor("record", "Station");
	definition.CaptureInputs.Midi = {"keys"};
	definition.ControlBindings.Activate.push_back({io::RigFile::BindingSource::Midi, "keys",
		{60u, 1u, io::RigFile::NOTE, 0u}, {60u, 0u, io::RigFile::NOTE, 0u}});
	rig.Triggers = {definition};
	auto station = RuntimeStation("Station");
	engine::RigCoordinator coordinator;
	unsigned int writes = 0u;
	ASSERT_TRUE(coordinator.BuildInitial(rig, {StationDescriptor("Station")}, {station}, 0u, {}, {},
		[&](const auto&) { ++writes; return true; }));
	const auto initial = coordinator.Accepted();
	ASSERT_EQ(1u, initial->Graph.Triggers[0].Sources.size());
	EXPECT_FALSE(initial->Graph.Triggers[0].Sources[0].Available);
	const auto reopened = coordinator.RefreshRuntimeAvailability(0u, {"keys"});
	ASSERT_TRUE(reopened);
	EXPECT_TRUE(reopened->Graph.Triggers[0].Sources[0].Available);
	ASSERT_EQ(1u, reopened->InputDispatch.LiveMidi.size());
	EXPECT_EQ("keys", reopened->InputDispatch.LiveMidi[0].DeviceName);
	ASSERT_EQ(1u, reopened->InputDispatch.LiveMidi[0].Recipients.size());
	EXPECT_EQ(station, reopened->InputDispatch.LiveMidi[0].Recipients[0]);
	EXPECT_EQ(initial->Triggers[0].Instance, reopened->Triggers[0].Instance);
	EXPECT_EQ(rig, reopened->Rig);
	const auto missingAgain = coordinator.RefreshRuntimeAvailability(0u, {});
	ASSERT_TRUE(missingAgain);
	EXPECT_FALSE(missingAgain->Graph.Triggers[0].Sources[0].Available);
	EXPECT_EQ(initial->Triggers[0].Instance, missingAgain->Triggers[0].Instance);
	EXPECT_EQ(rig, missingAgain->Rig); EXPECT_EQ(0u, writes);
	std::stringstream serialized;
	ASSERT_TRUE(io::RigFile::ToJsonStream(missingAgain->Rig, serialized));
	const auto reloaded = io::RigFile::FromStream(std::move(serialized));
	ASSERT_TRUE(reloaded); EXPECT_EQ(rig, *reloaded);
}

TEST_F(RigSnapshotTest, RetainsUnavailableSourcesWithoutCreatingWildcardMidiRoutes)
{
	auto rig = EmptyRig();
	auto first = TriggerDescriptor("first", "Shared");
	first.CaptureInputs.Audio = { 0u, 3u };
	first.CaptureInputs.Midi = { "Missing keyboard" };
	auto second = TriggerDescriptor("second", "Shared");
	rig.Triggers = { first, second };

	const auto runtime = BuildSnapshot(rig,
		{ StationDescriptor("Shared"), StationDescriptor("Other") },
		1u,
		{ "Present keyboard" });

	ASSERT_TRUE(runtime);
	ASSERT_EQ(2u, runtime->Triggers.size());
	EXPECT_EQ(0u, runtime->Triggers[0].StationIndex.value());
	EXPECT_EQ(0u, runtime->Triggers[1].StationIndex.value());
	ASSERT_EQ(3u, runtime->Graph.Triggers[0].Sources.size());
	EXPECT_TRUE(runtime->Graph.Triggers[0].Sources[0].Available);
	EXPECT_FALSE(runtime->Graph.Triggers[0].Sources[1].Available);
	EXPECT_FALSE(runtime->Graph.Triggers[0].Sources[2].Available);
	EXPECT_TRUE(runtime->Graph.Triggers[1].Sources.empty());
	ASSERT_EQ(1u, runtime->InputDispatch.LiveMidi.size());
	ASSERT_EQ("Present keyboard", runtime->InputDispatch.LiveMidi[0].DeviceName);
	EXPECT_TRUE(runtime->InputDispatch.LiveMidi[0].Recipients.empty());
}

TEST_F(RigSnapshotTest, LiveMidiEligibilityRequiresAnExplicitDeviceSelection)
{
	auto noneConfig = TriggerDescriptor("none", "Station");
	auto selectedConfig = TriggerDescriptor("selected", "Station");
	selectedConfig.CaptureInputs.Midi = { "Keys" };

	auto rig = EmptyRig();
	rig.Triggers = { noneConfig, selectedConfig };
	const auto snapshot = BuildSnapshot(rig, { StationDescriptor("Station") }, 0u,
		{ "Keys", "Other" });
	ASSERT_TRUE(snapshot);
	ASSERT_EQ(2u, snapshot->InputDispatch.LiveMidi.size());
	ASSERT_EQ(1u, snapshot->InputDispatch.LiveMidi[0].Recipients.size());
	EXPECT_EQ("Station", snapshot->InputDispatch.LiveMidi[0].Recipients[0]->Name());
	EXPECT_TRUE(snapshot->InputDispatch.LiveMidi[1].Recipients.empty());
}

TEST_F(RigSnapshotTest, CaptureAndStationEditsReuseTriggerAndStageNewMidiDispatch)
{
	auto firstStation = RuntimeStation("First");
	auto secondStation = RuntimeStation("Second");
	auto initial = EmptyRig();
	auto trigger = TriggerDescriptor("record", "First");
	trigger.CaptureInputs.Audio = { 0u };
	trigger.CaptureInputs.Midi = { "Keys A" };
	initial.Triggers = { trigger };
	ConfigureMidiRegistry(initial, { "Keys A", "Keys B" });
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial,
		{ StationDescriptor("First"), StationDescriptor("Second") },
		{ firstStation, secondStation }, 2u, { "Keys A", "Keys B" },
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto accepted = coordinator.Accepted();
	ASSERT_TRUE(accepted);

	auto candidate = initial;
	candidate.Triggers[0].CaptureInputs.Audio.push_back(1u);
	candidate.Triggers[0].CaptureInputs.Midi = { "Keys B" };
	candidate.Triggers[0].StationTarget = "Second";
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(candidate));
	const auto staged = coordinator.Staged();
	ASSERT_TRUE(staged);
	EXPECT_EQ(accepted->Triggers[0].Instance, staged->Triggers[0].Instance);
	EXPECT_TRUE(staged->TriggerReplacementChecks.empty());
	ASSERT_EQ(1u, staged->TriggerRouteUpdates.size());
	EXPECT_EQ(0u, staged->TriggerRouteUpdates[0].CandidateIndex);
	EXPECT_EQ((std::vector<unsigned int>{ 0u, 1u }), staged->Triggers[0].InputChannels);
	EXPECT_EQ(secondStation, staged->Triggers[0].Receiver);
	ASSERT_EQ(2u, staged->InputDispatch.LiveMidi.size());
	EXPECT_TRUE(staged->InputDispatch.LiveMidi[0].Recipients.empty());
	ASSERT_EQ(1u, staged->InputDispatch.LiveMidi[1].Recipients.size());
	EXPECT_EQ(secondStation, staged->InputDispatch.LiveMidi[1].Recipients[0]);
}

TEST_F(RigSnapshotTest, ReorderPreservesStableTriggerInstancesAndDisplayOrder)
{
	auto initial = EmptyRig();
	initial.Triggers = {
		TriggerDescriptor("first", "Station"),
		TriggerDescriptor("second", "Station")
	};
	auto station = RuntimeStation("Station");
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto accepted = coordinator.Accepted();

	auto candidate = accepted->Rig;
	std::swap(candidate.Triggers[0], candidate.Triggers[1]);
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(candidate));
	const auto staged = coordinator.Staged();
	ASSERT_TRUE(staged);
	EXPECT_EQ("second-id", staged->Triggers[0].Id);
	EXPECT_EQ("first-id", staged->Triggers[1].Id);
	EXPECT_EQ(accepted->Triggers[1].Instance, staged->Triggers[0].Instance);
	EXPECT_EQ(accepted->Triggers[0].Instance, staged->Triggers[1].Instance);
	EXPECT_TRUE(staged->TriggerReplacementChecks.empty());
}

TEST_F(RigSnapshotTest, DeletionAndActivationReplacementAreClassifiedByStableId)
{
	auto initial = EmptyRig();
	initial.Triggers = {
		TriggerDescriptor("first", "Station"),
		TriggerDescriptor("second", "Station")
	};
	auto station = RuntimeStation("Station");
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto accepted = coordinator.Accepted();

	auto deletion = accepted->Rig;
	deletion.Triggers.erase(deletion.Triggers.begin());
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(deletion));
	auto staged = coordinator.Staged();
	ASSERT_TRUE(staged);
	ASSERT_EQ(1u, staged->Triggers.size());
	EXPECT_EQ(accepted->Triggers[1].Instance, staged->Triggers[0].Instance);
	ASSERT_EQ(1u, staged->TriggerReplacementChecks.size());
	EXPECT_EQ(accepted->Triggers[0].Instance, staged->TriggerReplacementChecks[0].AcceptedInstance);
	ASSERT_EQ(engine::RigCoordinator::EditResult::TransitionRejected,
		coordinator.CompleteTransition(staged->Revision, false, [](const io::RigFile&) { return true; }));

	auto replacement = accepted->Rig;
	replacement.Triggers[0].ControlBindings.Activate.push_back(KeyboardButton(65u));
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(replacement));
	staged = coordinator.Staged();
	ASSERT_TRUE(staged);
	EXPECT_NE(accepted->Triggers[0].Instance, staged->Triggers[0].Instance);
	EXPECT_EQ(accepted->Triggers[1].Instance, staged->Triggers[1].Instance);
	ASSERT_EQ(1u, staged->TriggerReplacementChecks.size());
	EXPECT_EQ(accepted->Triggers[0].Instance, staged->TriggerReplacementChecks[0].AcceptedInstance);
}

TEST_F(RigSnapshotTest, DuplicateTriggerIdsFailInitialAndCandidateValidation)
{
	auto duplicate = EmptyRig();
	duplicate.Triggers = {
		TriggerDescriptor("first", "Station"),
		TriggerDescriptor("second", "Station")
	};
	duplicate.Triggers[1].Id = duplicate.Triggers[0].Id;
	auto station = RuntimeStation("Station");
	engine::RigCoordinator invalid;
	EXPECT_FALSE(invalid.BuildInitial(duplicate, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	auto valid = EmptyRig();
	valid.Triggers = { TriggerDescriptor("first", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(valid, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	duplicate = coordinator.Accepted()->Rig;
	duplicate.Triggers.push_back(duplicate.Triggers[0]);
	EXPECT_EQ(engine::RigCoordinator::EditResult::ValidationFailed, coordinator.SubmitCandidate(duplicate));
}

TEST_F(RigSnapshotTest, HudCableRoutesContainOnlyResolvedGraphEdges)
{
	engine::RoutingGraph graph;
	io::RigFileRouting::TriggerResolution resolved;
	resolved.TriggerIndex = 0u;
	resolved.TriggerName = "resolved";
	resolved.StationIndex = 3u;
	resolved.Sources.push_back({ io::RigFileRouting::SourceKind::Adc, 1u, {}, true });
	io::RigFileRouting::TriggerResolution unbound;
	unbound.TriggerIndex = 1u;
	unbound.TriggerName = "unbound";
	unbound.Sources.push_back({ io::RigFileRouting::SourceKind::Midi, 0u, "Keys", false });
	graph.Triggers = { resolved, unbound };

	const auto routes = gui::GuiHud::BuildCableRoutes(graph);

	ASSERT_EQ(3u, routes.size());
	EXPECT_EQ(gui::GuiHud::CableRoute::Kind::Capture, routes[0].RouteKind);
	EXPECT_EQ(0u, routes[0].TriggerIndex);
	EXPECT_EQ(gui::GuiHud::CableRoute::Kind::Station, routes[1].RouteKind);
	EXPECT_EQ(3u, routes[1].StationIndex.value());
	EXPECT_EQ(gui::GuiHud::CableRoute::Kind::Capture, routes[2].RouteKind);
	EXPECT_EQ(1u, routes[2].TriggerIndex);
	EXPECT_FALSE(routes[2].Source->Available);
}

TEST(GuiHudLayout, RevealScrollOffsetClampsAndRevealsItems)
{
	EXPECT_EQ(0, gui::GuiHud::RevealScrollOffset(-30, 200, 500, 10, 110));
	EXPECT_EQ(80, gui::GuiHud::RevealScrollOffset(0, 200, 500, 180, 280));
	EXPECT_EQ(120, gui::GuiHud::RevealScrollOffset(160, 200, 500, 120, 220));
	EXPECT_EQ(300, gui::GuiHud::RevealScrollOffset(450, 200, 500, 450, 550));
	EXPECT_EQ(0, gui::GuiHud::RevealScrollOffset(30, 500, 200, 0, 100));
}

TEST_F(RigSnapshotTest, SnapshotOwnsCompleteTriggerListIndependentOfStationReset)
{
	auto station = RuntimeStation("Station");
	const auto descriptor = StationDescriptor("Station");
	auto initial = EmptyRig();
	initial.Triggers = { TriggerDescriptor("first", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { descriptor }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	const auto accepted = coordinator.Accepted();
	ASSERT_TRUE(accepted);
	ASSERT_EQ(1u, accepted->Triggers.size());
	const auto retainedTrigger = accepted->Triggers[0].Instance;
	station->Reset();
	EXPECT_EQ(retainedTrigger, accepted->Triggers[0].Instance);

	auto replacement = EmptyRig();
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.SubmitCandidate(replacement));
	const auto staged = coordinator.Staged();
	ASSERT_TRUE(staged);
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteTransition(staged->Revision, true,
			[](const io::RigFile&) { return true; }));
	const auto pending = coordinator.Pending();
	ASSERT_TRUE(pending);
	coordinator.ApplyPendingAtAudioBoundary();
	EXPECT_TRUE(pending->Triggers.empty());
}

TEST_F(RigSnapshotTest, RequiresAudioAcknowledgementBeforeInputAndBothBeforePromotion)
{
	auto station = RuntimeStation("Station");
	auto initial = EmptyRig();
	initial.Triggers = { TriggerDescriptor("first", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto acceptedRevision = coordinator.Accepted()->Revision;
	auto candidate = EmptyRig();
	candidate.Triggers = { TriggerDescriptor("replacement", "Station") };
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.SubmitCandidate(candidate));
	const auto pendingRevision = coordinator.Staged()->Revision;
	EXPECT_FALSE(coordinator.Pending());
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteTransition(pendingRevision, true,
			[](const io::RigFile&) { return true; }));

	EXPECT_FALSE(coordinator.AcknowledgeInput(pendingRevision));
	EXPECT_FALSE(coordinator.PromoteAcknowledged());
	EXPECT_EQ(acceptedRevision, coordinator.Accepted()->Revision);
	coordinator.ApplyPendingAtAudioBoundary();
	EXPECT_EQ(pendingRevision, coordinator.AudioAcknowledgement());
	EXPECT_FALSE(coordinator.PromoteAcknowledged());
	ASSERT_TRUE(coordinator.AcknowledgeInput(pendingRevision));
	ASSERT_TRUE(coordinator.PromoteAcknowledged());
	EXPECT_EQ(pendingRevision, coordinator.Accepted()->Revision);
	EXPECT_FALSE(coordinator.Pending());
}

TEST_F(RigSnapshotTest, RejectedCandidatesConsumeRevisionsAndDiskFailureCannotRejectRuntimeEdits)
{
	auto station = RuntimeStation("Station");
	auto initial = EmptyRig();
	initial.Triggers = { TriggerDescriptor("accepted", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto accepted = coordinator.Accepted();
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(initial));
	const auto rejectedRevision = coordinator.Staged()->Revision;
	EXPECT_EQ(engine::RigCoordinator::EditResult::TransitionRejected,
		coordinator.CompleteTransition(rejectedRevision, false,
			[](const io::RigFile&) { return true; }));
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(initial));
	const auto persistenceRevision = coordinator.Staged()->Revision;
	EXPECT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteTransition(persistenceRevision, true,
			[](const io::RigFile&) { ADD_FAILURE() << "Runtime edits must never write a rig"; return false; }));
	EXPECT_GT(persistenceRevision, rejectedRevision);
	EXPECT_EQ(accepted, coordinator.Accepted());
	ASSERT_TRUE(coordinator.Pending());
	coordinator.ApplyPendingAtAudioBoundary();
	ASSERT_TRUE(coordinator.AcknowledgeInput(persistenceRevision));
	ASSERT_TRUE(coordinator.PromoteAcknowledged());

	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(initial));
	const auto successfulRevision = coordinator.Staged()->Revision;
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteTransition(successfulRevision, true,
			[](const io::RigFile&) { return true; }));
	EXPECT_GT(successfulRevision, persistenceRevision);
	EXPECT_EQ(successfulRevision, coordinator.Pending()->Revision);
}

TEST_F(RigSnapshotTest, RapidSecondEditIsRejectedUntilFirstEditPromotes)
{
	auto station = RuntimeStation("Station");
	auto initial = EmptyRig();
	initial.Triggers = { TriggerDescriptor("accepted", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	auto first = initial;
	first.Triggers[0].Name = "first";
	auto second = initial;
	second.Triggers[0].Name = "second";
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(first));
	const auto firstRevision = coordinator.Staged()->Revision;
	EXPECT_EQ(engine::RigCoordinator::EditResult::EditsDisabled, coordinator.SubmitCandidate(second));
	EXPECT_EQ(firstRevision, coordinator.Staged()->Revision);
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteTransition(firstRevision, true, [](const io::RigFile&) { return true; }));
	coordinator.ApplyPendingAtAudioBoundary();
	ASSERT_TRUE(coordinator.AcknowledgeInput(firstRevision));
	ASSERT_TRUE(coordinator.PromoteAcknowledged());
	EXPECT_EQ("first", coordinator.Accepted()->Rig.Triggers[0].Name);
	EXPECT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(second));
}

TEST_F(RigSnapshotTest, ShutdownDuringTransitionOrPendingCannotPublishCandidate)
{
	auto makeCoordinator = [this]()
	{
		auto coordinator = std::make_unique<engine::RigCoordinator>();
		auto initial = EmptyRig();
		initial.Triggers = { TriggerDescriptor("accepted", "Station") };
		const auto station = RuntimeStation("Station");
		if (!coordinator->BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
			engine::TriggerParams(), [](const io::RigFile&) { return true; })) return coordinator;
		coordinator->SubmitCandidate(initial);
		return coordinator;
	};

	auto staged = makeCoordinator();
	const auto stagedRevision = staged->Staged()->Revision;
	staged->Shutdown();
	EXPECT_EQ(engine::RigCoordinator::EditResult::TransitionRejected,
		staged->CompleteTransition(stagedRevision, true, [](const io::RigFile&) { return true; }));
	EXPECT_FALSE(staged->Pending());

	auto pending = makeCoordinator();
	const auto pendingRevision = pending->Staged()->Revision;
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		pending->CompleteTransition(pendingRevision, true, [](const io::RigFile&) { return true; }));
	pending->Shutdown();
	pending->ReleaseAfterReadersStopped();
	EXPECT_FALSE(pending->Pending());
	EXPECT_FALSE(pending->Accepted());
}

TEST_F(RigSnapshotTest, ShutdownAndReleaseAreIdempotent)
{
	auto station = RuntimeStation("Station");
	auto rig = EmptyRig();
	rig.Triggers = { TriggerDescriptor("accepted", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(rig, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	coordinator.Shutdown();
	coordinator.Shutdown();
	EXPECT_FALSE(coordinator.EditsEnabled());
	EXPECT_EQ(engine::RigCoordinator::EditResult::EditsDisabled,
		coordinator.SubmitCandidate(rig));
	coordinator.ReleaseAfterReadersStopped();
	coordinator.ReleaseAfterReadersStopped();
	EXPECT_FALSE(coordinator.Accepted());
	EXPECT_FALSE(coordinator.Pending());
}

TEST_F(RigSnapshotTest, RuntimeTransitionsNeverPersistAndRejectionRestoresEdits)
{
	auto station = RuntimeStation("Station");
	auto rig = EmptyRig();
	rig.Triggers = { TriggerDescriptor("accepted", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(rig, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	unsigned int saves = 0u;
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(rig));
	const auto firstRevision = coordinator.Staged()->Revision;
	EXPECT_EQ(0u, saves);
	EXPECT_EQ(engine::RigCoordinator::EditResult::TransitionRejected,
		coordinator.CompleteTransition(firstRevision, false,
			[&saves](const io::RigFile&) { ++saves; return true; }));
	EXPECT_EQ(0u, saves);
	EXPECT_TRUE(coordinator.EditsEnabled());

	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(rig));
	const auto secondRevision = coordinator.Staged()->Revision;
	EXPECT_GT(secondRevision, firstRevision);
	EXPECT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteTransition(secondRevision, true,
			[&saves](const io::RigFile&) { ++saves; return true; }));
	EXPECT_EQ(0u, saves);
}

TEST_F(RigSnapshotTest, AudioBoundaryPublishesFreshTriggerTransition)
{
	auto rig = EmptyRig();
	rig.Triggers = { TriggerDescriptor("accepted", "Station") };
	const auto snapshot = BuildSnapshot(rig, { StationDescriptor("Station") }, 0u, {});
	ASSERT_TRUE(snapshot);
	ASSERT_EQ(1u, snapshot->Triggers.size());

	audio::AudioHost host(io::UserConfig{});
	host.PublishPendingRigSnapshot(snapshot);
	audio::RigAudioBoundaryTestAccess::ApplyPending(host);
	ASSERT_EQ(snapshot->Revision, host.AppliedRigRevision());

	auto sameTriggerCandidate = std::make_shared<engine::RigSnapshot>();
	sameTriggerCandidate->Revision = 41u;
	host.RequestRigTriggerTransition(41u, snapshot, sameTriggerCandidate);
	audio::RigAudioBoundaryTestAccess::PublishTransition(host);
	EXPECT_EQ(41u, host.TransitionReadyRigRevision());
	EXPECT_EQ(0u, host.RejectedRigRevision());

	base::Action action;
	ASSERT_TRUE(snapshot->Triggers[0].Instance->QueueExternalControlAction(true, true, action).IsEaten);
	auto replacement = std::make_shared<engine::RigSnapshot>();
	replacement->Revision = 42u;
	replacement->TriggerReplacementChecks = { { snapshot->Triggers[0].Instance } };
	host.RequestRigTriggerTransition(42u, snapshot, replacement);
	audio::RigAudioBoundaryTestAccess::PublishTransition(host);
	EXPECT_EQ(0u, host.TransitionReadyRigRevision());
	EXPECT_EQ(42u, host.RejectedRigRevision());
}

TEST_F(RigSnapshotTest, ReplacementKeepsLatestHistoryForTheSameTriggerId)
{
	auto station = RuntimeStation("Station");
	auto initial = EmptyRig();
	initial.Triggers = { TriggerDescriptor("original", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto original = coordinator.Accepted()->Triggers[0].Instance;
	auto edited = initial;
	edited.Triggers[0].Name = "renamed";
	edited.Triggers[0].ControlBindings.Activate.push_back(KeyboardButton(65u));
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(edited));
	const auto staged = coordinator.Staged();
	ASSERT_TRUE(staged);
	const auto replacement = staged->Triggers[0].Instance;
	ASSERT_NE(original, replacement);
	EXPECT_TRUE(replacement->GetTakes().empty());

	// History can change after staging, up to the accepted audio boundary.
	original->RestoreTakes({ { engine::TriggerTake::SOURCE_ADC, "source", "target", station } });
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteTransition(staged->Revision, true,
			[](const io::RigFile&) { return true; }));
	const auto history = replacement->GetTakes();
	ASSERT_EQ(1u, history.size());
	EXPECT_EQ("source", history[0].SourceTakeId);
	EXPECT_EQ("target", history[0].TargetTakeId);
	EXPECT_EQ(station, history[0].Receiver);
}

TEST_F(RigSnapshotTest, RetainedTriggerRecordsAcrossRealRoutePublicationAndDitchesLifo)
{
	auto stationA = RuntimeStation("A");
	auto stationB = RuntimeStation("B");
	const std::vector<io::JamFile::Station> stationFiles{
		StationDescriptor("A"), StationDescriptor("B") };
	auto descriptor = TriggerDescriptor("record", "A");
	descriptor.CaptureInputs.Audio = { 0u };
	auto initial = EmptyRig();
	initial.Triggers = { descriptor };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, stationFiles, { stationA, stationB },
		2u, {}, engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	audio::AudioHost host(io::UserConfig{});
	host.PublishPendingRigSnapshot(coordinator.Accepted());
	audio::RigAudioBoundaryTestAccess::ApplyPending(host);

	auto trigger = coordinator.Accepted()->Triggers[0].Instance;
	ASSERT_TRUE(trigger);
	base::Action action;
	io::UserConfig cfg{};
	cfg.Audio.NumChannelsIn = 2u;
	auto edge = [&](bool activate, bool down, std::uint64_t revision)
	{
		ASSERT_TRUE(trigger->QueueExternalControlAction(activate, down, action, revision).IsEaten);
		trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
		trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
		trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
	};
	auto record = [&](std::uint64_t revision)
	{
		edge(true, true, revision);
		edge(true, false, revision);
		trigger->OnTick(utils::Timer::GetTime(), 64u, cfg, std::nullopt, revision);
		edge(true, true, revision);
		edge(true, false, revision);
		stationA->CommitChanges();
		stationB->CommitChanges();
	};
	auto publishEdit = [&](io::RigFile candidate)
	{
		ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(candidate));
		const auto staged = coordinator.Staged();
		ASSERT_TRUE(staged);
		host.RequestRigTriggerTransition(staged->Revision, coordinator.Accepted(), staged);
		audio::RigAudioBoundaryTestAccess::PublishTransition(host);
		ASSERT_EQ(staged->Revision, host.TransitionReadyRigRevision());
		ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
			coordinator.CompleteTransition(staged->Revision, true,
				[](const io::RigFile&) { return true; }));
		host.ClearRigTriggerTransition();
		host.PublishPendingRigSnapshot(coordinator.Pending());
		audio::RigAudioBoundaryTestAccess::ApplyPending(host);
		ASSERT_TRUE(coordinator.ObserveAudioAcknowledgement(host.AppliedRigRevision()));
		ASSERT_TRUE(coordinator.AcknowledgeInput(host.AppliedRigRevision()));
		ASSERT_TRUE(coordinator.PromoteAcknowledged());
		EXPECT_EQ(trigger, coordinator.Accepted()->Triggers[0].Instance);
	};

	record(coordinator.Accepted()->Revision);
	ASSERT_EQ(1u, stationA->NumTakes());
	ASSERT_EQ(1u, stationA->GetLoopTakes()[0]->GetLoops().size());
	ASSERT_TRUE(stationA->GetLoopTakes()[0]->HasPendingTriggerCapture());
	EXPECT_TRUE(trigger->CanApplyCaptureRouting(false));
	EXPECT_FALSE(trigger->CanApplyCaptureRouting(true));
	{
		auto destructive = coordinator.Accepted()->Rig;
		destructive.Triggers[0].StationTarget = "B";
		ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(destructive));
		const auto rejected = coordinator.Staged();
		ASSERT_EQ(1u, rejected->TriggerRouteUpdates.size());
		EXPECT_TRUE(rejected->TriggerRouteUpdates.front().ReceiverChanged);
		host.RequestRigTriggerTransition(rejected->Revision, coordinator.Accepted(), rejected);
		audio::RigAudioBoundaryTestAccess::PublishTransition(host);
		EXPECT_EQ(rejected->Revision, host.RejectedRigRevision());
		EXPECT_EQ(0u, host.TransitionReadyRigRevision());
		EXPECT_EQ(engine::RigCoordinator::EditResult::TransitionRejected,
			coordinator.CompleteTransition(rejected->Revision, false, [](const io::RigFile&) { return true; }));
		host.ClearRigTriggerTransition();
		EXPECT_TRUE(stationA->GetLoopTakes()[0]->HasPendingTriggerCapture());
	}
	auto candidate = coordinator.Accepted()->Rig;
	candidate.Triggers[0].CaptureInputs.Audio = { 0u, 1u };
	publishEdit(candidate);
	record(coordinator.Accepted()->Revision);
	ASSERT_EQ(2u, stationA->NumTakes());
	ASSERT_EQ(2u, stationA->GetLoopTakes()[1]->GetLoops().size());
	// Moving the receiver remains destructive to an ending capture. Complete
	// actual tails before this separate edit; same-receiver input edits above
	// were deliberately published while the earlier tail remained pending.
	for (const auto& take : stationA->GetLoopTakeSnapshot()) take->EndRecording();
	trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
	trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, coordinator.Accepted()->Revision);
	EXPECT_TRUE(trigger->CanApplyCaptureRouting(true));
	candidate = coordinator.Accepted()->Rig;
	candidate.Triggers[0].CaptureInputs.Audio = { 0u };
	candidate.Triggers[0].StationTarget = "B";
	publishEdit(candidate);
	record(coordinator.Accepted()->Revision);
	ASSERT_EQ(1u, stationB->NumTakes());

	const auto history = trigger->GetTakes();
	ASSERT_EQ(3u, history.size());
	EXPECT_NE(history[0].TargetTakeId, history[1].TargetTakeId);
	EXPECT_NE(history[1].TargetTakeId, history[2].TargetTakeId);
	auto ditch = [&]()
	{
		const auto revision = coordinator.Accepted()->Revision;
		edge(false, true, revision);
		edge(false, false, revision);
		stationA->CommitChanges();
		stationB->CommitChanges();
	};
	ditch();
	EXPECT_EQ(0u, stationB->NumTakes());
	EXPECT_EQ(2u, stationA->NumTakes());
	ditch();
	EXPECT_EQ(1u, stationA->NumTakes());
	ditch();
	EXPECT_EQ(0u, stationA->NumTakes());
	EXPECT_TRUE(trigger->GetTakes().empty());
	ditch();
	EXPECT_EQ(0u, stationA->NumTakes());
	EXPECT_EQ(0u, stationB->NumTakes());
}

TEST_F(RigSnapshotTest, ReplacedTriggerDitchesFrozenHistoryFromItsOriginalReceiver)
{
    auto stationA = RuntimeStation("A");
    auto stationB = RuntimeStation("B");
    auto initial = EmptyRig();
    initial.Triggers = { TriggerDescriptor("original", "A") };
    engine::RigCoordinator coordinator;
    ASSERT_TRUE(coordinator.BuildInitial(initial,
        { StationDescriptor("A"), StationDescriptor("B") }, { stationA, stationB },
        1u, {}, engine::TriggerParams(), [](const io::RigFile&) { return true; }));
    auto original = coordinator.Accepted()->Triggers.front().Instance;
    auto candidate = initial;
    candidate.Triggers.front().Name = "replacement";
    candidate.Triggers.front().ControlBindings.Activate.push_back(KeyboardButton(65u));
    candidate.Triggers.front().StationTarget = "B";
    ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(candidate));
    auto staged = coordinator.Staged();
    auto replacement = staged->Triggers.front().Instance;
    ASSERT_NE(original, replacement);
    auto take = stationA->AddTake();
    take->AddLoop(0u, stationA->Name());
    stationA->CommitChanges();
    original->RestoreTakes({ { engine::TriggerTake::SOURCE_ADC, "", take->Id(), stationA } });
    ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
        coordinator.CompleteTransition(staged->Revision, true,
            [](const io::RigFile&) { return true; }));
    ASSERT_EQ(1u, replacement->GetTakes().size());
    EXPECT_EQ(stationA, replacement->GetTakes().front().Receiver);
    base::Action action;
    io::UserConfig cfg;
    for (const bool down : { true, false })
    {
        replacement->QueueExternalControlAction(false, down, action, staged->Revision);
        replacement->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, staged->Revision);
        replacement->ProcessStructuralActionsOnJob(cfg, std::nullopt);
        replacement->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, staged->Revision);
    }
    EXPECT_TRUE(stationA->GetLoopTakeSnapshot().empty());
    EXPECT_TRUE(stationB->GetLoopTakeSnapshot().empty());
    EXPECT_TRUE(replacement->GetTakes().empty());
}

struct PublishedAudioSnapshotLifetimeProbe
{
	PublishedAudioSnapshotLifetimeProbe(unsigned int initialValue,
		std::atomic<unsigned int>& destructionCount,
		std::atomic<bool>& destroyedOffWriter,
		std::thread::id writerThread) noexcept
		: value(initialValue), complement(~initialValue), destroyed(destructionCount),
		destroyedOffWriter(destroyedOffWriter), writerThread(writerThread) {}
	~PublishedAudioSnapshotLifetimeProbe()
	{
		if (std::this_thread::get_id() != writerThread)
			destroyedOffWriter.store(true, std::memory_order_relaxed);
		destroyed.fetch_add(1u, std::memory_order_relaxed);
	}
	unsigned int value;
	unsigned int complement;
	std::atomic<unsigned int>& destroyed;
	std::atomic<bool>& destroyedOffWriter;
	std::thread::id writerThread;
};

TEST(PublishedAudioSnapshot, NestedBorrowsKeepRetiredStorageOffAudioUntilCollected)
{
	std::atomic<unsigned int> destroyed{ 0u };
	std::atomic<bool> destroyedOffWriter{ false };
	base::PublishedAudioSnapshot<const PublishedAudioSnapshotLifetimeProbe> snapshot;
	const auto writerThread = std::this_thread::get_id();
	snapshot.store(std::make_shared<PublishedAudioSnapshotLifetimeProbe>(1u, destroyed,
		destroyedOffWriter, writerThread));
	auto outer = snapshot.borrow();
	auto inner = snapshot.borrow();
	ASSERT_TRUE(outer);
	ASSERT_TRUE(inner);
	EXPECT_EQ(1u, outer->value);
	EXPECT_EQ(1u, inner->value);

	snapshot.store(std::make_shared<PublishedAudioSnapshotLifetimeProbe>(2u, destroyed,
		destroyedOffWriter, writerThread));
	EXPECT_EQ(0u, destroyed.load(std::memory_order_relaxed));
	outer.reset();
	snapshot.collect();
	EXPECT_EQ(0u, destroyed.load(std::memory_order_relaxed));
	inner.reset();
	EXPECT_EQ(0u, destroyed.load(std::memory_order_relaxed));
	snapshot.collect();
	EXPECT_EQ(1u, destroyed.load(std::memory_order_relaxed));
	EXPECT_FALSE(destroyedOffWriter.load(std::memory_order_relaxed));
}

TEST(PublishedAudioSnapshot, RepeatedPublicationKeepsBorrowAliveUntilCollection)
{
	std::atomic<unsigned int> destroyed{ 0u };
	std::atomic<bool> destroyedOffWriter{ false };
	base::PublishedAudioSnapshot<const PublishedAudioSnapshotLifetimeProbe> snapshot;
	const auto writerThread = std::this_thread::get_id();
	auto published = std::make_shared<PublishedAudioSnapshotLifetimeProbe>(7u, destroyed,
		destroyedOffWriter, writerThread);
	snapshot.store(published);
	auto lease = snapshot.borrow();
	ASSERT_TRUE(lease);

	const auto ownersBeforeRepublishing = published.use_count();
	for (unsigned int publication = 0u; publication < 64u; ++publication)
		snapshot.store(published);
	EXPECT_EQ(ownersBeforeRepublishing, published.use_count());
	snapshot.store(nullptr);
	published.reset();
	snapshot.collect();
	EXPECT_EQ(0u, destroyed.load(std::memory_order_relaxed));
	EXPECT_EQ(7u, lease->value);

	lease.reset();
	snapshot.collect();
	EXPECT_EQ(1u, destroyed.load(std::memory_order_relaxed));
	EXPECT_FALSE(destroyedOffWriter.load(std::memory_order_relaxed));
}

TEST(PublishedAudioSnapshot, ReaderCanBorrowWhileWriterReplacesAndCollects)
{
	std::atomic<unsigned int> destroyed{ 0u };
	std::atomic<bool> destroyedOffWriter{ false };
	base::PublishedAudioSnapshot<const PublishedAudioSnapshotLifetimeProbe> snapshot;
	const auto writerThread = std::this_thread::get_id();
	snapshot.store(std::make_shared<PublishedAudioSnapshotLifetimeProbe>(1u, destroyed,
		destroyedOffWriter, writerThread));
	std::atomic<bool> running{ true };
	std::atomic<bool> invalidValue{ false };
	std::atomic<unsigned int> reads{ 0u };
	std::thread reader([&] {
		while (running.load(std::memory_order_relaxed))
		{
			auto lease = snapshot.borrow();
			if (lease)
			{
				if ((lease->value ^ lease->complement) != 0xffffffffu)
					invalidValue.store(true, std::memory_order_relaxed);
				reads.fetch_add(1u, std::memory_order_relaxed);
			}
		}
	});
	while (reads.load(std::memory_order_relaxed) == 0u)
		std::this_thread::yield();
	for (unsigned int value = 2u; value < 2002u; ++value)
	{
		snapshot.store(std::make_shared<PublishedAudioSnapshotLifetimeProbe>(value, destroyed,
			destroyedOffWriter, writerThread));
		if ((value & 7u) == 0u)
			snapshot.collect();
	}
	running.store(false, std::memory_order_relaxed);
	reader.join();
	snapshot.collect();
	EXPECT_GT(reads.load(std::memory_order_relaxed), 0u);
	EXPECT_FALSE(invalidValue.load(std::memory_order_relaxed));
	EXPECT_FALSE(destroyedOffWriter.load(std::memory_order_relaxed));
	EXPECT_EQ(2000u, destroyed.load(std::memory_order_relaxed));
}

TEST(PublishedAudioSnapshot, BoundedSlotsCanBeExhaustedAndReused)
{
	base::PublishedAudioSnapshot<const unsigned int> snapshot;
	auto emptyLease = snapshot.borrow();
	EXPECT_FALSE(emptyLease);
	EXPECT_TRUE(emptyLease.ReadSucceeded());
	snapshot.store(std::make_shared<const unsigned int>(7u));
	std::array<base::PublishedAudioSnapshot<const unsigned int>::Lease, 8u> leases;
	for (auto& lease : leases)
	{
		lease = snapshot.borrow();
		ASSERT_TRUE(lease);
	}
	auto exhaustedLease = snapshot.borrow();
	EXPECT_FALSE(exhaustedLease);
	EXPECT_FALSE(exhaustedLease.ReadSucceeded());
	snapshot.store(nullptr);
	auto emptyDuringExhaustion = snapshot.borrow();
	EXPECT_FALSE(emptyDuringExhaustion);
	EXPECT_TRUE(emptyDuringExhaustion.ReadSucceeded());
	leases[3].reset();
	snapshot.store(std::make_shared<const unsigned int>(7u));
	auto replacementLease = snapshot.borrow();
	ASSERT_TRUE(replacementLease);
	EXPECT_TRUE(replacementLease.ReadSucceeded());
	EXPECT_EQ(7u, *replacementLease);
}

TEST_F(RigSnapshotTest, HardwarePreferenceMergeRetainsTriggerIdentityAndUnrelatedEdits)
{
	auto rig = EmptyRig();
	rig.Triggers = {TriggerDescriptor("trigger", "Station")};
	rig.User.Loop.FadeSamps = 123u;
	rig.User.Audio.LatencyIn = 27u;
	auto station = RuntimeStation("Station");
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(rig, {StationDescriptor("Station")}, {station}, 0u, {}, engine::TriggerParams(), [](const auto&) {return true;}));
	const auto initial = coordinator.Accepted();
	auto audio = rig.User.Audio; audio.Name = "device"; audio.BufSize = 256u;
	ASSERT_TRUE(coordinator.MergeHardwarePreferences(audio, std::nullopt));
	const auto applied = coordinator.Accepted();
	EXPECT_EQ(applied->Rig.User.Loop.FadeSamps, 123u);
	EXPECT_EQ(applied->Rig.User.Audio.LatencyIn, 27u);
	EXPECT_EQ(applied->Triggers.front().Instance, initial->Triggers.front().Instance);
	std::vector<io::UserConfig::MidiSettings> desired{{"missing",true,"missing-input"}};
	ASSERT_TRUE(coordinator.MergeHardwarePreferences(std::nullopt, desired));
	EXPECT_EQ(coordinator.Accepted()->Rig.User.Audio.Name, "device");
	EXPECT_EQ(coordinator.Accepted()->Rig.User.Midi.Devices.front().Name, "missing");
	EXPECT_TRUE(coordinator.Accepted()->Rig.User.Midi.Devices.front().Enabled);
	EXPECT_EQ(initial->Rig.User.Audio.Name, rig.User.Audio.Name);
}

TEST_F(RigSnapshotTest, PrependingPreservesReceiverIdentityAndDoesNotCreateTriggers)
{
 auto a = RuntimeStation("A"), b = RuntimeStation("B"), added = RuntimeStation("New");
 auto rig = EmptyRig(); rig.Triggers = {TriggerDescriptor("first", "A"), TriggerDescriptor("second", "B")};
 engine::RigCoordinator coordinator;
 ASSERT_TRUE(coordinator.BuildInitial(rig, {StationDescriptor("A"), StationDescriptor("B")}, {a, b}, 0u, {}, {}, {}));
 const auto old = coordinator.Accepted();
 ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitInventory({added, a, b}));
 const auto candidate = coordinator.Staged();
 ASSERT_TRUE(candidate->MembershipChange);
 EXPECT_EQ(2u, candidate->Triggers.size());
 EXPECT_EQ(a, candidate->Triggers[0].BoundReceiver);
 EXPECT_EQ(b, candidate->Triggers[1].BoundReceiver);
 EXPECT_EQ(1u, candidate->Triggers[0].StationIndex);
 EXPECT_EQ(2u, candidate->Triggers[1].StationIndex);
 EXPECT_EQ(old->Triggers[0].Instance, candidate->Triggers[0].Instance);
 EXPECT_TRUE(candidate->TriggerRouteUpdates.empty());
 EXPECT_EQ("A", candidate->Rig.Triggers[0].StationTarget);
 ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.CompleteTransition(candidate->Revision, true, {}));
 coordinator.ApplyPendingAtAudioBoundary();
 ASSERT_TRUE(coordinator.AcknowledgeInput(candidate->Revision)); ASSERT_TRUE(coordinator.PromoteAcknowledged());
 EXPECT_EQ(3u, coordinator.Accepted()->Stations->size());
}

TEST_F(RigSnapshotTest, RemovalExplicitlyUnbindsAndNeverRedirectsToIndexOccupant)
{
 auto a = RuntimeStation("A"), b = RuntimeStation("B");
 auto rig = EmptyRig(); rig.Triggers = {TriggerDescriptor("first", "A"), TriggerDescriptor("second", "B")};
 engine::RigCoordinator coordinator;
 ASSERT_TRUE(coordinator.BuildInitial(rig, {StationDescriptor("A"), StationDescriptor("B")}, {a, b}, 0u, {}, {}, {}));
 const auto original = coordinator.Accepted();
 ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitInventory({b}, a));
 const auto candidate = coordinator.Staged();
 EXPECT_FALSE(candidate->Rig.Triggers[0].StationTarget);
 EXPECT_FALSE(candidate->Triggers[0].StationIndex);
 EXPECT_FALSE(candidate->Triggers[0].BoundReceiver);
 EXPECT_EQ(original->Triggers[0].Id, candidate->Triggers[0].Id);
 EXPECT_EQ(b, candidate->Triggers[1].BoundReceiver);
 ASSERT_EQ(1u, candidate->TriggerRouteUpdates.size());
 EXPECT_TRUE(candidate->TriggerRouteUpdates[0].ReceiverChanged);
 EXPECT_EQ(0u, candidate->TriggerRouteUpdates[0].CandidateIndex);
}

TEST_F(RigSnapshotTest, SameIndexWithNewStationIsAReceiverChange)
{
 auto a = RuntimeStation("A"), replacement = RuntimeStation("A");
 auto rig = EmptyRig(); rig.Triggers = {TriggerDescriptor("first", "A")};
 engine::RigCoordinator coordinator;
 ASSERT_TRUE(coordinator.BuildInitial(rig, {StationDescriptor("A")}, {a}, 0u, {}, {}, {}));
 // Same display name is not the original receiver identity.
 ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitInventory({replacement}, a));
 const auto candidate = coordinator.Staged();
 EXPECT_FALSE(candidate->Triggers[0].StationIndex);
 ASSERT_EQ(1u, candidate->TriggerRouteUpdates.size());
 EXPECT_TRUE(candidate->TriggerRouteUpdates[0].ReceiverChanged);
}

TEST_F(RigSnapshotTest, RemovalValidationUsesIdentityRevisionAndTakePresence)
{
 auto a = RuntimeStation("A"), b = RuntimeStation("B");
 engine::StationManagement management;
 management.Target = a; management.Revision = 7u;
 using Result = engine::StationManagement::Validation;
 EXPECT_EQ(Result::Ready, management.Validate({a, b}, 7u, false));
 EXPECT_EQ(Result::FinalStation, management.Validate({a}, 7u, true));
 EXPECT_EQ(Result::Stale, management.Validate({a, b}, 8u, true));
 EXPECT_EQ(Result::Stale, management.Validate({RuntimeStation("A"), b}, 7u, true));
 a->AddTake();
 EXPECT_EQ(Result::Confirm, management.Validate({a, b}, 7u, false));
 EXPECT_EQ(Result::Ready, management.Validate({a, b}, 7u, true));
 management.Cancel();
 EXPECT_EQ(Result::Stale, management.Validate({a, b}, 7u, true));
}

TEST_F(RigSnapshotTest, ReceiverRemovalDrainsQueuedStartBeforeRebinding)
{
 auto a = RuntimeStation("A");
 auto rig = EmptyRig(); rig.Triggers = {TriggerDescriptor("record", "A")};
 engine::RigCoordinator coordinator;
 ASSERT_TRUE(coordinator.BuildInitial(rig, {StationDescriptor("A")}, {a}, 1u, {}, {}, {}));
 auto trigger = coordinator.Accepted()->Triggers[0].Instance;
 io::UserConfig cfg{}; cfg.Audio.NumChannelsIn = 1u;
 base::Action action;
 const auto revision = coordinator.Accepted()->Revision;
 trigger->QueueExternalControlAction(true, true, action, revision);
 trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
 trigger->RequestReceiverRemoval(a.get());
 // A late UI edge carries the new generation, but the outgoing receiver remains unavailable.
 trigger->QueueExternalControlAction(true, false, action, revision);
 trigger->QueueExternalControlAction(true, true, action, revision);
 trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
 trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
 EXPECT_TRUE(trigger->ReceiverRemovalReady());
 trigger->CompleteReceiverRemovalOnJob();
 EXPECT_TRUE(trigger->GetTakes().empty());
 EXPECT_EQ(0u, a->NumTakes());
 EXPECT_FALSE(trigger->IsActivateInputDown());
 EXPECT_TRUE(trigger->CanApplyCaptureRouting(true));
}

TEST_F(RigSnapshotTest, ReceiverRemovalCancelsHeldRecordingAndReleasesHistory)
{
 auto a = RuntimeStation("A");
 auto rig = EmptyRig(); rig.Triggers = {TriggerDescriptor("record", "A")};
 engine::RigCoordinator coordinator;
 ASSERT_TRUE(coordinator.BuildInitial(rig, {StationDescriptor("A")}, {a}, 1u, {}, {}, {}));
 auto trigger = coordinator.Accepted()->Triggers[0].Instance;
 io::UserConfig cfg{}; cfg.Audio.NumChannelsIn = 1u;
 base::Action action;
 const auto revision = coordinator.Accepted()->Revision;
 trigger->QueueExternalControlAction(true, true, action, revision);
 trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
 trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
 trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
 ASSERT_EQ(1u, a->NumTakes());
 ASSERT_FALSE(trigger->GetTakes().empty());
 a->BeginMembershipRemoval(); trigger->RequestReceiverRemoval(a.get());
 trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
 ASSERT_TRUE(trigger->ReceiverRemovalReady());
 trigger->CompleteReceiverRemovalOnJob();
 EXPECT_TRUE(trigger->GetTakes().empty());
 EXPECT_FALSE(trigger->IsActivateInputDown());
 EXPECT_FALSE(a->GetLoopTakeSnapshot().front()->HasPendingTriggerCapture());
 EXPECT_TRUE(trigger->CanApplyCaptureRouting(true));
}

TEST(RigDefaultSaveTest, FilenameMustStayInRigDirectoryAndAvoidReservedNames)
{
 const std::filesystem::path directory = L"C:/Roaming/Jamma";
 EXPECT_TRUE(RigDefaultSave::ValidFilename(directory / L"My rig.rig", directory));
 for (const auto name : {L"defaults.rig", L"con.rig", L"LPT9.rig", L"profile.tmp.rig", L"wrong.json", L"../other.rig"})
  EXPECT_FALSE(RigDefaultSave::ValidFilename(directory / name, directory));
 EXPECT_FALSE(RigDefaultSave::ValidFilename(L"C:/other/rig.rig", directory));
}

TEST(RigDefaultSaveTest, WriteOrderingAndIndependentFailuresPreserveDefaults)
{
 io::RigFile rig{}; rig.Name = "Current rig";
 io::InitFile defaults{}; defaults.Rig = L"selected.rig"; defaults.Jam = L"performance.jam";
 defaults.RigLoadType = io::InitFile::LOAD_SPECIFIC; defaults.JamLoadType = io::InitFile::LOAD_LAST;
 defaults.RigOrigin = "user";
 std::vector<std::string> steps;
 auto result = RigDefaultSave::WriteCaptured(rig, defaults, L"isolated-defaults.json",
  [&](const auto&, const auto& captured) { steps.push_back("rig"); EXPECT_EQ("Current rig", captured.Name); return false; },
  [&](const auto&, const auto&) { steps.push_back("defaults"); return true; });
 EXPECT_FALSE(result->RigSaved); EXPECT_FALSE(result->DefaultsSaved);
 EXPECT_EQ(std::vector<std::string>{"rig"}, steps);
 steps.clear();
 result = RigDefaultSave::WriteCaptured(rig, defaults, L"isolated-defaults.json",
  [&](const auto&, const auto&) { steps.push_back("rig"); return true; },
  [&](const auto& path, const auto& json) {
   steps.push_back("defaults"); EXPECT_EQ(L"isolated-defaults.json", path);
   auto loaded = io::InitFile::FromStream(std::stringstream(json));
   EXPECT_TRUE(loaded); if (loaded) EXPECT_EQ(defaults.Jam, loaded->Jam);
   return false;
  });
 EXPECT_TRUE(result->RigSaved); EXPECT_FALSE(result->DefaultsSaved);
 EXPECT_EQ((std::vector<std::string>{"rig", "defaults"}), steps);
 result = RigDefaultSave::WriteCaptured(rig, defaults, L"isolated-defaults.json",
  [](const auto&, const auto&) { return true; }, [](const auto&, const auto&) { return true; });
 EXPECT_TRUE(result->RigSaved); EXPECT_TRUE(result->DefaultsSaved);
 EXPECT_EQ(defaults.Jam, result->Defaults.Jam);
 // A runtime topology/routing edit must not become a new startup default
 // simply because the performer chose different hardware.
 rig.User.Loop.FadeSamps = 123u;
 io::RigFile::Trigger trigger{}; trigger.Name = "saved-trigger"; trigger.StationTarget = "saved-station";
 rig.Triggers.push_back(trigger);
 auto live = rig; live.Name = "runtime"; live.Triggers.clear(); live.User.Loop.FadeSamps = 999u;
 live.User.Audio.Name = "new-device"; live.User.Midi.Devices = {{"new-midi", true}};
 const auto preferences = RigDefaultSave::WithHardwarePreferences(rig, live);
 EXPECT_EQ(rig.Name, preferences.Name);
 ASSERT_EQ(1u, preferences.Triggers.size());
 EXPECT_EQ(trigger.StationTarget, preferences.Triggers.front().StationTarget);
 EXPECT_EQ(123u, preferences.User.Loop.FadeSamps);
 EXPECT_EQ("new-device", preferences.User.Audio.Name);
 ASSERT_EQ(1u, preferences.User.Midi.Devices.size());
 EXPECT_EQ("new-midi", preferences.User.Midi.Devices.front().Name);
}

TEST_F(RigSnapshotTest, DisabledFinalBinConsumesPressAndRelease)
{
 auto station = RuntimeStation("Only");
 gui::GuiStationControls controls;
 const auto view = glm::scale(glm::mat4(1.0f), glm::vec3(0.001f, 0.001f, 0.001f));
 utils::Size2d viewport{800u, 600u};
 controls.Update({station}, view, viewport, true, true);
 auto anchor = station->TopCapModelPosition(); anchor.Y += 65.0f;
 const auto point = gui::CableInteraction::ProjectAnchor(view * glm::vec4(anchor.X, anchor.Y, anchor.Z, 1.0f), viewport);
 ASSERT_TRUE(point);
 actions::TouchAction action; action.Position = {static_cast<int>(point->x), static_cast<int>(point->y)}; action.Index = 0;
 action.State = actions::TouchAction::TOUCH_DOWN;
 unsigned activations = 0u;
 EXPECT_TRUE(controls.Touch(action, [&](auto) { ++activations; }));
 action.State = actions::TouchAction::TOUCH_UP;
 EXPECT_TRUE(controls.Touch(action, [&](auto) { ++activations; }));
 EXPECT_EQ(0u, activations);
}

TEST_F(RigSnapshotTest, ReleasingCtrlCancelsBinAndConsumesItsRelease)
{
 auto station = RuntimeStation("A"), b = RuntimeStation("B");
 gui::GuiStationControls controls;
 const auto view = glm::scale(glm::mat4(1.0f), glm::vec3(0.001f, 0.001f, 0.001f));
 utils::Size2d viewport{800u, 600u};
 controls.Update({station, b}, view, viewport, true, true);
 auto anchor = station->TopCapModelPosition(); anchor.Y += 65.0f;
 const auto point = gui::CableInteraction::ProjectAnchor(view * glm::vec4(anchor.X, anchor.Y, anchor.Z, 1.0f), viewport);
 ASSERT_TRUE(point);
 actions::TouchAction action; action.Position = {static_cast<int>(point->x), static_cast<int>(point->y)}; action.Index = 0;
 unsigned activations = 0u;
 action.State = actions::TouchAction::TOUCH_DOWN;
 EXPECT_TRUE(controls.Touch(action, [&](auto) { ++activations; }));
 controls.CancelBinPress();
 controls.Update({station, b}, view, viewport, false, true);
 action.State = actions::TouchAction::TOUCH_UP;
 EXPECT_TRUE(controls.Touch(action, [&](auto) { ++activations; }));
 EXPECT_EQ(0u, activations);
}

TEST_F(RigSnapshotTest, DetachingDeletedMasterPreservesTransportAndGrid)
{
 auto station = RuntimeStation("Master");
 auto take = station->AddTake();
 engine::LoopParams params; params.Size = {80u, 80u};
 audio::WireMixBehaviourParams wire; wire.Channels = {0u};
 auto loop = std::make_shared<engine::Loop>(params, engine::Loop::GetMixerParams(params.Size, wire));
 loop->Record();
 std::vector<float> samples(constants::MaxLoopFadeSamps + 48000u, 0.1f);
 base::AudioWriteRequest request; request.samples = samples.data(); request.numSamps = static_cast<unsigned>(samples.size());
 request.stride = 1u; request.fadeCurrent = 0.0f; request.fadeNew = 1.0f; request.source = base::Audible::AUDIOSOURCE_ADC;
 loop->OnBlockWrite(request, 0); loop->EndWrite(request.numSamps, true);
 loop->Play(constants::MaxLoopFadeSamps, 48000u, false);
 take->AddLoop(loop); take->CommitChanges(); station->CommitChanges();
 engine::Quantiser quantiser;
 auto clock = std::make_shared<utils::Timer>(); quantiser.SetClock(clock);
 ASSERT_TRUE(quantiser.TrySetMasterFromHover(loop, base::DEPTH_LOOP, {station}, 48000u, io::UserConfig{}, true));
 ASSERT_TRUE(clock->InitialiseAbsoluteSamplePos(48000u * 7u + 111u));
 const auto phase = clock->AbsoluteSamplePos(); const auto length = clock->SeedSourceLength();
 const auto grid = quantiser.ActiveGrid();
 const auto pins = loop.use_count();
 quantiser.DetachStationMaster(station);
 EXPECT_EQ(pins - 1, loop.use_count());
 EXPECT_EQ(phase, clock->AbsoluteSamplePos()); EXPECT_EQ(length, clock->SeedSourceLength());
 EXPECT_EQ(grid.DivisionCount, quantiser.ActiveGrid().DivisionCount);
 EXPECT_EQ(grid.Source, quantiser.ActiveGrid().Source);
}

TEST_F(RigSnapshotTest, PresentationMoveSharesTopCapPositionAndReportsItsSettlingFrame)
{
 auto station = RuntimeStation("Animated");
 station->SetModelPosition({550.0f, -20.0f, 0.0f});
 const auto finalCap = station->TopCapModelPosition();
 station->BeginPresentationMove(-50.0f);
 EXPECT_FLOAT_EQ(550.0f, station->ModelPosition().X);
 EXPECT_FLOAT_EQ(-50.0f, station->PresentationModelPosition().X);
 EXPECT_FLOAT_EQ(finalCap.X - 600.0f, station->TopCapModelPosition().X);
 EXPECT_TRUE(station->AdvancePresentationMove(-1.0f));
 EXPECT_FLOAT_EQ(-50.0f, station->PresentationModelPosition().X);
 float previous = -50.0f;
 for (unsigned int step = 0u; step < 5u; ++step)
 {
  EXPECT_TRUE(station->AdvancePresentationMove(10.0f)); // Long frames are bounded.
  const float current = station->PresentationModelPosition().X;
  EXPECT_GE(current, previous); EXPECT_LE(current, 550.0f); previous = current;
 }
 EXPECT_FALSE(station->IsPresentationMoving());
 EXPECT_FLOAT_EQ(550.0f, station->PresentationModelPosition().X);
 EXPECT_FALSE(station->AdvancePresentationMove(0.05f));
}

TEST_F(RigSnapshotTest, PredelayEditRequiresTriggerReplacementAndAudioAcceptance)
{
	auto rig = EmptyRig();
	rig.Triggers = { TriggerDescriptor("capture", std::nullopt) };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(rig, {}, {}, 0u, {}, engine::TriggerParams(), {}));
	const auto previous = coordinator.Accepted()->Triggers.front().Instance;
	rig.Triggers.front().PreDelayMs = -0.125;
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(rig));
	const auto staged = coordinator.Staged();
	ASSERT_TRUE(staged);
	ASSERT_EQ(1u, staged->TriggerReplacementChecks.size());
	EXPECT_EQ(previous, staged->TriggerReplacementChecks.front().AcceptedInstance);
	EXPECT_NE(previous, staged->Triggers.front().Instance);
	EXPECT_DOUBLE_EQ(-0.125, staged->Rig.Triggers.front().PreDelayMs);
	EXPECT_EQ(engine::RigCoordinator::EditResult::TransitionRejected,
		coordinator.CompleteTransition(staged->Revision, false, {}));
	EXPECT_EQ(previous, coordinator.Accepted()->Triggers.front().Instance);
	EXPECT_DOUBLE_EQ(0.0, coordinator.Accepted()->Rig.Triggers.front().PreDelayMs);
}
