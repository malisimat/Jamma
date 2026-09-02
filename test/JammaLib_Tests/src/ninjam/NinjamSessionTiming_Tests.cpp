#include "gtest/gtest.h"

#include "ninjam/NinjamSession.h"
#include "ninjam/NinjamTimingCoordinator.h"

TEST(NinjamSessionTiming, PhysicalLossRetryCreatesOneNoSyncAndFreshEpoch)
{
	ninjam::NinjamSessionTimingStatus status;
	ninjam::NinjamTimingCoordinator coordinator;
	ninjam::NinjamTempoJoinOptions options;

	status = ninjam::NinjamSession::AdvanceTimingStatus(status, false);
	EXPECT_FALSE(status.Changed);
	EXPECT_EQ(0u, status.SessionEpoch);

	status = ninjam::NinjamSession::AdvanceTimingStatus(status, true);
	ASSERT_TRUE(status.Changed);
	ASSERT_TRUE(status.IsAvailable);
	ASSERT_EQ(1u, status.SessionEpoch);
	const auto connected = coordinator.ObserveSessionStatus(status, options, std::nullopt);
	EXPECT_FALSE(connected.InvalidatePendingCorrections);
	EXPECT_TRUE(coordinator.IsConnected());
	EXPECT_EQ(1u, coordinator.SessionEpoch());

	status = ninjam::NinjamSession::AdvanceTimingStatus(status, false);
	ASSERT_TRUE(status.Changed);
	ASSERT_FALSE(status.IsAvailable);
	const auto loss = coordinator.ObserveSessionStatus(status, options, std::nullopt);
	EXPECT_TRUE(loss.InvalidatePendingCorrections);
	EXPECT_EQ(ninjam::NinjamNoSyncReason::PhysicalLoss, loss.NoSyncReason);
	EXPECT_FALSE(coordinator.IsConnected());

	const auto repeatedLoss = coordinator.ObserveSessionStatus(status, options, std::nullopt);
	EXPECT_FALSE(repeatedLoss.InvalidatePendingCorrections);
	EXPECT_EQ(ninjam::NinjamNoSyncReason::None, repeatedLoss.NoSyncReason);

	status = ninjam::NinjamSession::AdvanceTimingStatus(status, true);
	ASSERT_TRUE(status.Changed);
	ASSERT_TRUE(status.IsAvailable);
	ASSERT_EQ(2u, status.SessionEpoch);
	const auto retried = coordinator.ObserveSessionStatus(status, options, std::nullopt);
	EXPECT_FALSE(retried.InvalidatePendingCorrections);
	EXPECT_TRUE(coordinator.IsConnected());
	EXPECT_EQ(2u, coordinator.SessionEpoch());
}
