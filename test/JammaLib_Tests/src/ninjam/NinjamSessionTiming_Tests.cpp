#include "gtest/gtest.h"

#include "engine/StationRemote.h"
#include "io/UserConfig.h"
#include "ninjam/NinjamController.h"
#include "ninjam/NinjamNetworkService.h"
#include "ninjam/NinjamSession.h"
#include "ninjam/NinjamTimingCoordinator.h"

TEST(NinjamSessionTiming, PhysicalLossRetryCreatesOneNoSyncAndFreshEpoch)
{
	ninjam::NinjamController controller;
	auto* session = controller.Session();
	ASSERT_NE(nullptr, session);
	ninjam::NinjamTimingCoordinator coordinator;
	ninjam::NinjamTempoJoinOptions options;

	auto status = session->ObservePhysicalAvailability(true);
	ASSERT_TRUE(status.Changed);
	ASSERT_TRUE(status.IsAvailable);
	ASSERT_EQ(1u, status.SessionEpoch);
	const auto connected = coordinator.ObserveSessionStatus(status, options, std::nullopt);
	ASSERT_TRUE(connected.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::NoSync, connected.DesiredTransport->Intent);
	EXPECT_TRUE(coordinator.IsConnected());
	EXPECT_EQ(1u, coordinator.SessionEpoch());

	ninjam::NinjamSessionPumpResult cached;
	cached.TimingStatus = status;
	cached.Snapshot = ninjam::NinjamRemoteSnapshot{};
	cached.Snapshot->Users.push_back({ "stale-user" });
	controller.ApplySessionPumpResult(cached);

	io::JamFile::NinjamConfig persisted;
	persisted.Host = "127.0.0.1:1";
	persisted.User = "persisted-lifecycle";
	persisted.WorkDir = ".";
	controller.LoadConfig(persisted);

	// Even if the replacement reports available immediately, Start's production
	// edge must publish unavailable first and retain the old epoch.
	status = session->ObservePhysicalAvailability(true);
	ASSERT_TRUE(status.Changed);
	ASSERT_FALSE(status.IsAvailable);
	ASSERT_EQ(1u, status.SessionEpoch);
	ninjam::NinjamSessionPumpResult lossResult;
	lossResult.TimingStatus = status;
	controller.ApplySessionPumpResult(lossResult);
	EXPECT_FALSE(controller.TakePendingSnapshot().has_value());

	const auto loss = coordinator.ObserveSessionStatus(status, options, std::nullopt);
	ASSERT_TRUE(loss.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::NoSync, loss.DesiredTransport->Intent);
	EXPECT_EQ(ninjam::NinjamNoSyncReason::PhysicalLoss, loss.NoSyncReason);
	EXPECT_FALSE(coordinator.IsConnected());

	const auto repeatedLoss = coordinator.ObserveSessionStatus(status, options, std::nullopt);
	EXPECT_FALSE(repeatedLoss.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamNoSyncReason::None, repeatedLoss.NoSyncReason);

	status = session->ObservePhysicalAvailability(true);
	ASSERT_TRUE(status.Changed);
	ASSERT_TRUE(status.IsAvailable);
	ASSERT_EQ(2u, status.SessionEpoch);
	const auto retried = coordinator.ObserveSessionStatus(status, options, std::nullopt);
	ASSERT_TRUE(retried.DesiredTransport.has_value());
	EXPECT_EQ(2u, retried.DesiredTransport->SessionEpoch);
	EXPECT_TRUE(coordinator.IsConnected());
	EXPECT_EQ(2u, coordinator.SessionEpoch());

	engine::StationParams stationParams;
	stationParams.Name = "stale-user";
	stationParams.Size = { 200, 280 };
	audio::MergeMixBehaviourParams merge;
	auto mixerParams = engine::Station::GetMixerParams(stationParams.Size, merge);
	auto remote = std::make_shared<engine::StationRemote>(stationParams, mixerParams);
	remote->SetRemoteUserName("stale-user");
	std::vector<std::shared_ptr<engine::Station>> stations{ remote };
	ninjam::NinjamNetworkService service;
	EXPECT_TRUE(service.UpdateRemoteStationsFromSnapshot({}, stations));
	EXPECT_TRUE(stations.empty());
}

TEST(NinjamNetworkServiceTiming, CachedObservationDeadlineNoSyncIsIdempotentAndRecoversFresh)
{
	ninjam::NinjamNetworkService service;
	ninjam::NinjamTempoJoinOptions options;
	options.PushLocalTempoOnJoin = false;
	options.PromptBeforeApplyingRemoteTempo = false;
	options.TempoRequestDeadline = std::chrono::seconds(1);
	service.SetTempoJoinOptions(options);
	service.PrepareTempoSyncOnConnect(std::nullopt);

	ninjam::NinjamSessionTimingStatus available;
	available.IsAvailable = true;
	available.Changed = true;
	available.SessionEpoch = 1u;
	service.ObserveSessionStatus(available, std::nullopt);

	utils::Timer clock;
	clock.SetSeedSourceLength(480000ul);
	ninjam::NinjamTiming timing;
	timing.IsConnected = true;
	timing.IsValid = true;
	timing.DeviceSampleRate = 48000u;
	timing.SourceSampleRate = 48000u;
	timing.IntervalLengthSamps = 480000u;
	timing.IntervalPositionSamps = 1000u;
	timing.Bpm = 96.0f;
	timing.Bpi = 16u;
	timing.HasDeviceAudioSampleAtObservation = true;
	timing.DeviceAudioSampleAtObservation = 10000u;
	timing.HasLocalTransport = true;
	timing.LocalTransport.MasterLengthSamps = 480000u;
	timing.LocalTransport.MasterPhaseSamps = 1000u;
	timing.LocalTransport.AbsoluteSamplePos = 1000u;
	const auto start = std::chrono::steady_clock::time_point{};

	service.ObserveTiming(timing, std::nullopt, false, io::UserConfig{}, clock, start);
	const auto deadline = service.ObserveTiming(timing, std::nullopt, false,
		io::UserConfig{}, clock, start + std::chrono::seconds(1));
	ASSERT_TRUE(deadline.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::NoSync, deadline.DesiredTransport->Intent);
	EXPECT_EQ(ninjam::NinjamNoSyncReason::ObservationDeadline, deadline.NoSyncReason);
	const auto repeated = service.ObserveTiming(timing, std::nullopt, false,
		io::UserConfig{}, clock, start + std::chrono::seconds(2));
	EXPECT_FALSE(repeated.DesiredTransport.has_value());

	timing.DeviceAudioSampleAtObservation += 256u;
	timing.IntervalPositionSamps += 256u;
	timing.LocalTransport.MasterPhaseSamps += 256u;
	timing.LocalTransport.AbsoluteSamplePos += 256u;
	const auto recovered = service.ObserveTiming(timing, std::nullopt, false,
		io::UserConfig{}, clock, start + std::chrono::seconds(2));
	EXPECT_TRUE(recovered.DesiredTransport.has_value());
	EXPECT_TRUE(service.HasConnectedTiming());
}

TEST(NinjamControllerConnection, ManualJoinWithoutConfigStartsGuestAttempt)
{
	ninjam::NinjamController controller;
	testing::internal::CaptureStdout();
	controller.Connect("127.0.0.1:1");
	const auto output = testing::internal::GetCapturedStdout();
	ASSERT_TRUE(controller.Config().has_value());
	EXPECT_EQ("jamma_guest", controller.Config()->User);
	EXPECT_NE(std::string::npos, output.find("Connect attempt 1 to 127.0.0.1:1 as anonymous:jamma_guest"));
	auto connection = controller.AcquireConnectionUse();
	ASSERT_TRUE(connection);
	EXPECT_EQ(ninjam::NinjamConnection::ConnectionState::Connecting, connection->State());
}

TEST(NinjamControllerConnection, ManualJoinPreservesConfiguredIdentity)
{
	ninjam::NinjamController controller;
	io::JamFile::NinjamConfig config;
	config.User = "saved-user";
	config.Pass = "saved-password";
	config.WorkDir = ".";
	controller.LoadConfig(config);
	controller.Connect("127.0.0.1:1");
	ASSERT_TRUE(controller.Config().has_value());
	EXPECT_EQ(config.User, controller.Config()->User);
	EXPECT_EQ(config.Pass, controller.Config()->Pass);
	EXPECT_EQ(config.WorkDir, controller.Config()->WorkDir);
	EXPECT_TRUE(controller.AcquireConnectionUse());
}

TEST(NinjamControllerConnection, PasswordWithoutUsernameReportsFailure)
{
	ninjam::NinjamController controller;
	io::JamFile::NinjamConfig config;
	config.Pass = "saved-password";
	controller.LoadConfig(config);
	testing::internal::CaptureStdout();
	controller.Connect("127.0.0.1:1");
	const auto output = testing::internal::GetCapturedStdout();
	EXPECT_NE(std::string::npos, output.find("Connection failed: Username not configured"));
	EXPECT_FALSE(controller.AcquireConnectionUse());
	EXPECT_TRUE(controller.Config()->User.empty());
}

TEST(NinjamSessionConnection, MissingHostReportsFailure)
{
	ninjam::NinjamSession session;
	io::JamFile::NinjamConfig config;
	config.User = "test-user";
	testing::internal::CaptureStdout();
	session.Start(config);
	const auto output = testing::internal::GetCapturedStdout();
	EXPECT_NE(std::string::npos, output.find("Connection failed: Host not configured"));
	EXPECT_FALSE(ninjam::NinjamConnectionUse(session));
}

TEST(NinjamSessionConnection, UnavailableWorkDirectoryReportsStartupFailure)
{
	ninjam::NinjamSession session;
	io::JamFile::NinjamConfig config;
	config.Host = "127.0.0.1:1";
	config.User = "test-user";
	// A source file cannot be used as a directory; no network attempt is needed.
	config.WorkDir = __FILE__;
	testing::internal::CaptureStdout();
	session.Start(config);
	const auto output = testing::internal::GetCapturedStdout();
	EXPECT_NE(std::string::npos, output.find("Connection failed: NINJAM work directory is unavailable"));
	ninjam::NinjamConnectionUse connection(session);
	ASSERT_TRUE(connection);
	EXPECT_EQ(ninjam::NinjamConnection::ConnectionState::Failed, connection->State());
}

TEST(NinjamSessionTiming, FormatRateChangeInvalidatesOldEpochOnceWithoutChangingConnection)
{
	ninjam::NinjamSession session;
	session.SetAudioFormat(48000u,64u,2u,2u,0u,0u);
	auto initial = session.ObservePhysicalAvailability(true);
	ASSERT_TRUE(initial.IsAvailable);
	session.SetAudioFormat(48000u,128u,2u,2u,0u,0u);
	EXPECT_FALSE(session.ObservePhysicalAvailability(true).Changed);
	session.SetAudioFormat(44100u,128u,2u,2u,0u,0u);
	const auto invalidated = session.ObservePhysicalAvailability(true);
	EXPECT_TRUE(invalidated.Changed); EXPECT_FALSE(invalidated.IsAvailable);
	const auto fresh = session.ObservePhysicalAvailability(true);
	EXPECT_TRUE(fresh.Changed); EXPECT_TRUE(fresh.IsAvailable);
	EXPECT_EQ(fresh.SessionEpoch, initial.SessionEpoch + 1u);
	EXPECT_FALSE(session.ObservePhysicalAvailability(true).Changed);
}
