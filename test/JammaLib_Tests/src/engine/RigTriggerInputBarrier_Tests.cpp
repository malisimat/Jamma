#include "engine/RigTriggerInputBarrier.h"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using engine::RigTriggerInputBarrier;

#if defined(JAMMA_STANDALONE_GATE_TEST)
int main(int argc, char** argv)
{
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
#endif

TEST(RigTriggerInputBarrier, StartsBlockedAndRejectsReservedRevisions)
{
	RigTriggerInputBarrier gate;

	EXPECT_EQ(RigTriggerInputBarrier::NoRevision, gate.OpenRevision());
	EXPECT_FALSE(gate.TryAcceptUi(1u));
	EXPECT_FALSE(gate.TryAcceptJob(1u));
	EXPECT_FALSE(gate.Open(RigTriggerInputBarrier::NoRevision));
	EXPECT_FALSE(gate.Open(RigTriggerInputBarrier::CloseForeverRevision));
}

TEST(RigTriggerInputBarrier, OpenAcceptsOnlyThePublishedRevision)
{
	RigTriggerInputBarrier gate;

	ASSERT_TRUE(gate.Open(7u));
	EXPECT_TRUE(gate.TryAcceptUi(7u));
	EXPECT_TRUE(gate.TryAcceptJob(7u));
	EXPECT_FALSE(gate.TryAcceptUi(6u));
	EXPECT_FALSE(gate.TryAcceptJob(8u));
	EXPECT_EQ(RigTriggerInputBarrier::NoRevision, gate.ObserveAndAcknowledgeClose());
}

TEST(RigTriggerInputBarrier, UiCloseAndJobObservationFormTwoProducerBarrier)
{
	RigTriggerInputBarrier gate;
	ASSERT_TRUE(gate.Open(11u));

	ASSERT_TRUE(gate.RequestCloseFromUi(11u));
	EXPECT_FALSE(gate.TryAcceptUi(11u));
	EXPECT_FALSE(gate.TryAcceptJob(11u));
	EXPECT_TRUE(gate.UiAcknowledged(11u));
	EXPECT_FALSE(gate.JobAcknowledged(11u));
	EXPECT_FALSE(gate.ReadyForAudioBoundary(11u));

	EXPECT_EQ(11u, gate.ObserveAndAcknowledgeClose());
	EXPECT_TRUE(gate.JobAcknowledged(11u));
	EXPECT_TRUE(gate.ReadyForAudioBoundary(11u));
	EXPECT_FALSE(gate.TryAcceptJob(11u));
}

TEST(RigTriggerInputBarrier, WrongRevisionCannotCloseCurrentInput)
{
	RigTriggerInputBarrier gate;
	ASSERT_TRUE(gate.Open(13u));

	EXPECT_FALSE(gate.RequestCloseFromUi(12u));
	EXPECT_TRUE(gate.TryAcceptUi(13u));
	EXPECT_TRUE(gate.TryAcceptJob(13u));
	EXPECT_FALSE(gate.UiAcknowledged(12u));
	EXPECT_EQ(RigTriggerInputBarrier::NoRevision, gate.RequestedClosedRevision());
}

TEST(RigTriggerInputBarrier, ReopenClearsOldBarrierAndRejectsOldRevision)
{
	RigTriggerInputBarrier gate;
	ASSERT_TRUE(gate.Open(17u));
	ASSERT_TRUE(gate.RequestCloseFromUi(17u));
	ASSERT_EQ(17u, gate.ObserveAndAcknowledgeClose());
	ASSERT_TRUE(gate.ReadyForAudioBoundary(17u));

	ASSERT_TRUE(gate.Reopen(18u));
	EXPECT_TRUE(gate.TryAcceptUi(18u));
	EXPECT_TRUE(gate.TryAcceptJob(18u));
	EXPECT_FALSE(gate.TryAcceptUi(17u));
	EXPECT_FALSE(gate.UiAcknowledged(17u));
	EXPECT_FALSE(gate.JobAcknowledged(17u));
	EXPECT_FALSE(gate.ReadyForAudioBoundary(17u));
}

TEST(RigTriggerInputBarrier, RepeatedUiCloseIsIdempotentBeforeJobAcknowledgement)
{
	RigTriggerInputBarrier gate;
	ASSERT_TRUE(gate.Open(23u));

	EXPECT_TRUE(gate.RequestCloseFromUi(23u));
	const auto firstToken = gate.RequestedCloseToken();
	EXPECT_TRUE(gate.RequestCloseFromUi(23u));
	EXPECT_EQ(firstToken, gate.RequestedCloseToken());
	EXPECT_EQ(23u, gate.ObserveAndAcknowledgeClose());
	EXPECT_TRUE(gate.ReadyForAudioBoundary(23u));
}

TEST(RigTriggerInputBarrier, DelayedOldAcknowledgementCannotSatisfyNewCloseOfSameRevision)
{
	RigTriggerInputBarrier gate;
	ASSERT_TRUE(gate.Open(27u));
	ASSERT_TRUE(gate.RequestCloseFromUi(27u));
	const auto oldCloseToken = gate.ObserveCloseTokenFromJob();
	ASSERT_NE(0u, oldCloseToken);

	// Rejection reopens the accepted revision. A later edit then closes that
	// same revision with a distinct epoch while the old job acknowledgement is
	// still delayed.
	ASSERT_TRUE(gate.Reopen(27u));
	ASSERT_TRUE(gate.RequestCloseFromUi(27u));
	const auto newCloseToken = gate.RequestedCloseToken();
	ASSERT_NE(oldCloseToken, newCloseToken);

	ASSERT_TRUE(gate.AcknowledgeObservedCloseFromJob(oldCloseToken));
	EXPECT_FALSE(gate.JobAcknowledged(27u));
	EXPECT_FALSE(gate.ReadyForAudioBoundary(27u));

	ASSERT_TRUE(gate.AcknowledgeObservedCloseFromJob(newCloseToken));
	EXPECT_TRUE(gate.JobAcknowledged(27u));
	EXPECT_TRUE(gate.ReadyForAudioBoundary(27u));
}

TEST(RigTriggerInputBarrier, CloseForeverRequiresFreshJobAcknowledgementAndCannotReopen)
{
	RigTriggerInputBarrier gate;
	ASSERT_TRUE(gate.Open(29u));
	ASSERT_TRUE(gate.RequestCloseFromUi(29u));
	ASSERT_EQ(29u, gate.ObserveAndAcknowledgeClose());
	ASSERT_TRUE(gate.ReadyForAudioBoundary(29u));

	gate.CloseForever();
	const auto permanentCloseToken = gate.RequestedCloseToken();
	gate.CloseForever();
	EXPECT_EQ(permanentCloseToken, gate.RequestedCloseToken());
	EXPECT_TRUE(gate.IsClosedForever());
	EXPECT_TRUE(gate.UiAcknowledgedCloseForever());
	EXPECT_FALSE(gate.JobAcknowledgedCloseForever());
	EXPECT_FALSE(gate.ReadyForShutdown());
	EXPECT_FALSE(gate.TryAcceptUi(29u));
	EXPECT_FALSE(gate.TryAcceptJob(29u));

	EXPECT_EQ(RigTriggerInputBarrier::CloseForeverRevision,
		gate.ObserveAndAcknowledgeClose());
	EXPECT_TRUE(gate.JobAcknowledgedCloseForever());
	EXPECT_TRUE(gate.ReadyForShutdown());
	EXPECT_FALSE(gate.Open(30u));
	EXPECT_FALSE(gate.Reopen(29u));
	EXPECT_FALSE(gate.RequestCloseFromUi(29u));
}

TEST(RigTriggerInputBarrier, ConcurrentOpenCannotErasePermanentClose)
{
	for (auto iteration = 0u; iteration < 200u; ++iteration)
	{
		RigTriggerInputBarrier gate;
		ASSERT_TRUE(gate.Open(31u));
		std::atomic<bool> start{ false };
		std::thread opener([&]()
			{
				while (!start.load(std::memory_order_acquire)) {}
				gate.Open(32u);
			});
		start.store(true, std::memory_order_release);
		gate.CloseForever();
		opener.join();

		EXPECT_TRUE(gate.IsClosedForever());
		EXPECT_EQ(RigTriggerInputBarrier::NoRevision, gate.OpenRevision());
		EXPECT_EQ(RigTriggerInputBarrier::CloseForeverRevision,
			gate.RequestedClosedRevision());
		EXPECT_NE(0u, gate.RequestedCloseToken());
		EXPECT_EQ(RigTriggerInputBarrier::CloseForeverRevision,
			gate.ObserveAndAcknowledgeClose());
		EXPECT_TRUE(gate.ReadyForShutdown());
	}
}
