#define __WINDOWS_ASIO__
#include "audio/AsioStream.h"
#include "audio/AudioHost.h"
#include "engine/SessionControl.h"
#include "rtaudio/include/asio.h"
#include <gtest/gtest.h>
#include <thread>

namespace audio
{
	// Real owned adapter and upstream metadata, with no opened device/buffers.
	// Only the native stop effect is injected; constructors initialize Windows COM
	// but never enumerate/probe/open hardware. All fake state is test-owned.
	class AsioStreamTestAccess
	{
	public:
		struct Calls { unsigned int Stops = 0u, Closes = 0u, Errors = 0u; long Result = ASE_OK; };
		class FakeBackend : public AsioStream::Backend
		{
		public:
			explicit FakeBackend(Calls& calls) : Backend(&StopDriver), _calls(calls)
			{
				_current = &calls;
				stream_.state = STREAM_RUNNING;
				stream_.mode = DUPLEX;
				stream_.callbackInfo.errorCallback = reinterpret_cast<void*>(&OnError);
			}
			~FakeBackend() override { stream_.state = STREAM_CLOSED; _current = nullptr; }
			void closeStream() override { ++_calls.Closes; Backend::closeStream(); }
			void startStream() override { stream_.state = STREAM_RUNNING; if (NotifyOnStart) stopStream(); }
			void PrepareStopped() { stream_.state = STREAM_STOPPED; }
			void DrainCompleted() { stream_.state = STREAM_STOPPING; }
			bool NotifyOnStart = false;
		private:
			static long StopDriver() { ++_current->Stops; return _current->Result; }
			static void OnError(RtAudioError::Type, const std::string&) { ++_current->Errors; }
			static inline Calls* _current = nullptr;
			Calls& _calls;
		};
		static std::unique_ptr<AudioDevice> Device(std::unique_ptr<FakeBackend> backend,
			unsigned int sampleRate = 48000u, unsigned int blockSize = 128u)
		{
			AudioStreamParams params{"Primary ASIO device", sampleRate, 2u, blockSize, 0u, 0u, 2u, 2u};
			return std::make_unique<AudioDevice>(params, std::unique_ptr<AsioStream>(new AsioStream(std::move(backend))));
		}
		static void Install(AudioHost& host, std::unique_ptr<AudioDevice> device)
		{
			host._publishedStreamParams.store(std::make_shared<const AudioStreamParams>(device->GetAudioStreamParams()));
			host._audioDevice = std::move(device);
		}
		static void Heartbeat(AudioHost& host) { host._audioCallbackHeartbeat.fetch_add(1u); }
		static bool EnterCallback(AudioHost& host) { return host.TryEnterCallback(); }
		static void LeaveCallback(AudioHost& host) { host.LeaveCallback(); }
		static bool StaleAuthorizedReopen(AudioHost& host)
		{
			auto expected = AudioHost::_CallbackAdmissionClosed;
			return host._callbackAdmission.compare_exchange_strong(expected, 0u);
		}
		static bool LifetimeUncertain(const AudioHost& host) { return host._backendLifetimeUncertain; }
		static void Inventory(engine::SessionControl& control) { control._deviceInventory.Devices = {{0u, "Primary ASIO device", true}}; }
	};
}

using Access = audio::AsioStreamTestAccess;

TEST(AsioStreamRecovery, NormalPauseAndCloseNeedNoDrainCallbacksOrDuplicateStop)
{
	Access::Calls calls;
	auto device = Access::Device(std::make_unique<Access::FakeBackend>(calls));
	EXPECT_TRUE(device->Pause()); // No audio callbacks exist to drain.
	EXPECT_EQ(calls.Stops, 1u);
	EXPECT_FALSE(device->IsRunning());
	EXPECT_TRUE(device->TryStop());
	EXPECT_EQ(calls.Stops, 1u);
	EXPECT_FALSE(device->IsOpen());
}

TEST(AsioStreamRecovery, FailedNativeStopStaysActiveWithoutRecursiveAbortOrDisposal)
{
	Access::Calls calls;
	auto backend = std::make_unique<Access::FakeBackend>(calls);
	auto* fake = backend.get();
	auto device = Access::Device(std::move(backend));
	calls.Result = ASE_HWMalfunction;
	EXPECT_FALSE(device->TryStop());
	EXPECT_EQ(calls.Stops, 1u);
	EXPECT_EQ(calls.Errors, 1u);
	EXPECT_EQ(calls.Closes, 0u);
	EXPECT_TRUE(device->IsRunning());
	fake->DrainCompleted();
	fake->closeStream(); // Exercise the owned close guard for upstream STOPPING.
	EXPECT_TRUE(device->IsOpen());
	EXPECT_TRUE(device->IsRunning());
	EXPECT_EQ(calls.Stops, 2u);
	EXPECT_EQ(calls.Errors, 2u);
	calls.Result = ASE_OK;
	EXPECT_TRUE(device->TryStop());
	EXPECT_FALSE(device->IsOpen());
}

TEST(AsioStreamRecovery, DriverThreadStopOnlyPublishesAndWindowStopIsConfirmedBeforeClose)
{
	Access::Calls calls;
	auto backend = std::make_unique<Access::FakeBackend>(calls);
	auto* fake = backend.get();
	auto device = Access::Device(std::move(backend));
	std::thread driver([fake] { fake->stopStream(); });
	driver.join();
	EXPECT_TRUE(device->ResetRequested());
	EXPECT_TRUE(device->IsRunning());
	EXPECT_EQ(calls.Stops, 0u);
	EXPECT_EQ(calls.Errors, 0u);
	EXPECT_TRUE(device->TryStop());
	EXPECT_EQ(calls.Stops, 1u);
	EXPECT_FALSE(device->IsOpen());
}

TEST(AsioStreamRecovery, SynchronousOwnerThreadDriverNotificationAlsoOnlyPublishes)
{
	Access::Calls calls;
	auto backend = std::make_unique<Access::FakeBackend>(calls);
	auto* fake = backend.get();
	auto device = Access::Device(std::move(backend));
	RtApi* notificationTarget = fake;
	notificationTarget->stopStream(); // Upstream sampleRateChanged virtual dispatch.
	EXPECT_TRUE(device->ResetRequested());
	EXPECT_TRUE(device->IsRunning());
	EXPECT_EQ(calls.Stops, 0u);
	EXPECT_TRUE(device->TryStop());
	EXPECT_EQ(calls.Stops, 1u);
}

TEST(AsioStreamRecovery, FirstStartClearsPreparedNotificationButPreservesStartAndResumeNotifications)
{
	for (const bool notifyDuringStart : {false, true})
	{
		SCOPED_TRACE(notifyDuringStart);
		Access::Calls calls;
		auto backend = std::make_unique<Access::FakeBackend>(calls);
		auto* fake = backend.get();
		fake->PrepareStopped();
		fake->NotifyOnStart = notifyDuringStart;
		auto device = Access::Device(std::move(backend));
		fake->stopStream(); // Notification during buffer preparation.
		ASSERT_TRUE(device->ResetRequested());
		EXPECT_TRUE(device->Start());
		EXPECT_EQ(device->ResetRequested(), notifyDuringStart);
		EXPECT_TRUE(device->Pause());
		fake->stopStream(); // A later reset must survive Resume.
		EXPECT_TRUE(device->Resume());
		EXPECT_TRUE(device->ResetRequested());
		EXPECT_EQ(calls.Stops, 1u);
	}
}

TEST(AsioStreamRecovery, ChangeHintNeedsStalledHeartbeatAndIgnoresHealthyOrGatedCallbacks)
{
	Access::Calls calls;
	audio::AudioHost host(io::UserConfig{});
	Access::Install(host, Access::Device(std::make_unique<Access::FakeBackend>(calls)));
	const auto now = std::chrono::steady_clock::now();
	EXPECT_FALSE(host.ObserveBackendReset(true, now));
	Access::Heartbeat(host);
	EXPECT_FALSE(host.ObserveBackendReset(false, now + std::chrono::milliseconds(400)));
	Access::Heartbeat(host);
	EXPECT_FALSE(host.ObserveBackendReset(false, now + std::chrono::milliseconds(800)));
	Access::Heartbeat(host);
	EXPECT_FALSE(host.ObserveBackendReset(false, now + std::chrono::seconds(3)));
	EXPECT_FALSE(host.ObserveBackendReset(false, now + std::chrono::seconds(4))); // Hint expired.
	EXPECT_FALSE(host.ObserveBackendReset(true, now + std::chrono::seconds(5)));
	host.CloseCallbackAdmission();
	EXPECT_FALSE(host.ObserveBackendReset(false, now + std::chrono::milliseconds(5600)));
	ASSERT_TRUE(host.ReopenCallbackAdmission());
	EXPECT_FALSE(host.ObserveBackendReset(true, now + std::chrono::milliseconds(5700)));
	EXPECT_TRUE(host.ObserveBackendReset(true, now + std::chrono::milliseconds(6200))); // Repeated hint cannot postpone a real stall.
	EXPECT_EQ(calls.Stops, 0u); // Stall is observation, never a teardown barrier.
	std::string error;
	EXPECT_TRUE(host.SuspendResetStream(error));
	EXPECT_FALSE(host.HasOpenStream());
}

TEST(AsioStreamRecovery, FailedResetStopRetainsBackendAndClearsPublishedAvailability)
{
	Access::Calls calls;
	audio::AudioHost host(io::UserConfig{});
	auto backend = std::make_unique<Access::FakeBackend>(calls);
	auto* fake = backend.get();
	Access::Install(host, Access::Device(std::move(backend)));
	std::thread driver([fake] { fake->stopStream(); });
	driver.join();
	ASSERT_TRUE(host.ObserveBackendReset());
	calls.Result = ASE_HWMalfunction;
	std::string error;
	EXPECT_FALSE(host.SuspendResetStream(error));
	EXPECT_FALSE(error.empty());
	EXPECT_EQ(calls.Stops, 1u);
	EXPECT_EQ(calls.Closes, 0u);
	EXPECT_TRUE(host.HasOpenStream());
	EXPECT_EQ(host.GetStreamParams().SampleRate, 0u);
	EXPECT_TRUE(Access::LifetimeUncertain(host));
	EXPECT_FALSE(host.ReopenCallbackAdmission());
	calls.Result = ASE_OK; // Test-owned cleanup after a newly confirmed barrier.
	EXPECT_TRUE(host.SuspendResetStream(error));
	EXPECT_FALSE(host.HasOpenStream());
}

TEST(AsioStreamRecovery, LowRateLargeBlocksGetAtLeastThreeBlockIntervalsBeforeStall)
{
	Access::Calls calls;
	audio::AudioHost host(io::UserConfig{});
	Access::Install(host, Access::Device(std::make_unique<Access::FakeBackend>(calls), 4000u, 4096u));
	const auto now = std::chrono::steady_clock::now();
	EXPECT_FALSE(host.ObserveBackendReset(true, now));
	EXPECT_FALSE(host.ObserveBackendReset(false, now + std::chrono::seconds(2)));
	EXPECT_FALSE(host.ObserveBackendReset(false, now + std::chrono::milliseconds(3071)));
	EXPECT_TRUE(host.ObserveBackendReset(false, now + std::chrono::milliseconds(3072)));
	std::string error;
	EXPECT_TRUE(host.SuspendResetStream(error));
}

TEST(AsioStreamRecovery, WindowLossWaitsForInputJobBoundaryPreservesPreferencesAndCanRecover)
{
	for (const bool rigEditPending : {false, true})
	{
	SCOPED_TRACE(rigEditPending);
	Access::Calls calls;
	io::UserConfig config{};
	config.Audio.Name = "Saved preferred device";
	config.Audio.BufSize = 128u;
	config.Audio.NumBuffers = 2u;
	config.Audio.NumChannelsIn = config.Audio.NumChannelsOut = 2u;
	audio::AudioHost host(config);
	engine::SessionControl control;
	io::IoInputSubsystem input(config, io::LoggingConfig{});
	engine::RigCoordinator rig;
	std::mutex sceneMutex;
	const std::vector<std::shared_ptr<engine::Station>> stations;
	io::RigFile rigFile;
	rigFile.Name = "test";
	rigFile.User = config;
	ASSERT_TRUE(rig.BuildInitial(rigFile, {}, stations, 2u, {}, {}, {}));
	input.PublishRigInputDispatch(rig.Accepted());
	ASSERT_TRUE(input.OpenRigTriggerInput(rig.Accepted()->Revision));
	control.Bind(host, input, rig, config, 1u);
	control.InitializeCandidates(config.Audio, {});
	if (rigEditPending) ASSERT_EQ(rig.SubmitCandidate(rigFile), engine::RigCoordinator::EditResult::Pending);
	Access::Inventory(control);
	auto backend = std::make_unique<Access::FakeBackend>(calls);
	auto* fake = backend.get();
	Access::Install(host, Access::Device(std::move(backend)));
	unsigned int publications = 0u;
	engine::SessionControl::Effects effects;
	effects.CheckEligibility = [] { return engine::SessionControl::Eligibility{true, false, false}; };
	effects.ApplyRuntimeAvailabilityLocked = [&publications](const auto& availability) {
		++publications; EXPECT_EQ(availability.Inputs, 0u); EXPECT_TRUE(availability.ApplyWhileQuiescent);
	};
	std::thread driver([fake] { fake->stopStream(); });
	driver.join();
	ASSERT_TRUE(Access::EnterCallback(host));
	control.PumpOnWindow({sceneMutex, stations}, effects, false);
	EXPECT_EQ(calls.Stops, 0u);
	EXPECT_TRUE(control.AdmissionPending());
	input.AcknowledgeRigTriggerInputCloseFromJob();
	control.PumpOnWindow({sceneMutex, stations}, effects, false);
	EXPECT_EQ(calls.Stops, 0u);
	EXPECT_TRUE(control.AdmissionPending());
	EXPECT_FALSE(control.HoldsWriter()); // Waiting for a reader returns to the UI.
	Access::LeaveCallback(host);
	EXPECT_TRUE(Access::StaleAuthorizedReopen(host)); // Job had read permission before reset publication.
	control.PumpOnWindow({sceneMutex, stations}, effects, false);
	EXPECT_EQ(calls.Stops, 1u);
	EXPECT_EQ(publications, rigEditPending ? 0u : 1u);
	if (rigEditPending)
	{
		// Source loss and driver close must not wait for a staged edit that itself
		// needs the missing callback. Runtime publication waits for its own boundary.
		ASSERT_TRUE(rig.Staged());
		rig.CompleteTransition(rig.Staged()->Revision, false, {});
		control.PumpOnWindow({sceneMutex, stations}, effects, false);
		EXPECT_EQ(publications, 0u);
		input.AcknowledgeRigTriggerInputCloseFromJob();
		control.PumpOnWindow({sceneMutex, stations}, effects, false);
		EXPECT_EQ(publications, 1u);
	}
	EXPECT_FALSE(host.HasOpenStream());
	EXPECT_EQ(host.GetStreamParams().NumInputChannels, 0u);
	EXPECT_EQ(control.Current(), engine::SessionControl::None);
	EXPECT_FALSE(control.HoldsWriter());
	EXPECT_FALSE(control.DeviceInventory().Devices.front().Probed);
	EXPECT_NE(control.TakePresentation().DeviceStatus.find("unavailable"), std::string::npos);
	EXPECT_EQ(config.Audio.Name, "Saved preferred device");
	audio::AudioHost::QuiescenceState state;
	std::string error;
	EXPECT_TRUE(host.BeginQuiescence(state, error)); // Manual Rescan is no longer blocked by reset.
	EXPECT_FALSE(state.WasRunning);
	control.PumpOnWindow({sceneMutex, stations}, effects, false);
	EXPECT_EQ(calls.Stops, 1u); // No periodic recovery or discovery.
	}
}
