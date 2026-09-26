#include "engine/RigTriggerIngressGate.h"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using engine::RigTriggerIngressGate;

#if defined(JAMMA_STANDALONE_GATE_TEST)
int main(int argc, char** argv)
{
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
#endif

TEST(RigTriggerIngressGate, StartsClosedAndRejectsReservedRevisions)
{
	RigTriggerIngressGate gate;

	EXPECT_EQ(RigTriggerIngressGate::NoRevision, gate.OpenRevision());
	EXPECT_FALSE(gate.TryAcceptUi(1u));
	EXPECT_FALSE(gate.TryAcceptJob(1u));
	EXPECT_FALSE(gate.Open(RigTriggerIngressGate::NoRevision));
	EXPECT_FALSE(gate.Open(RigTriggerIngressGate::CloseForeverRevision));
}

TEST(RigTriggerIngressGate, OpenAcceptsOnlyThePublishedRevision)
{
	RigTriggerIngressGate gate;

	ASSERT_TRUE(gate.Open(7u));
	EXPECT_TRUE(gate.TryAcceptUi(7u));
	EXPECT_TRUE(gate.TryAcceptJob(7u));
	EXPECT_FALSE(gate.TryAcceptUi(6u));
	EXPECT_FALSE(gate.TryAcceptJob(8u));
	EXPECT_EQ(RigTriggerIngressGate::NoRevision, gate.ObserveAndAcknowledgeClose());
}

TEST(RigTriggerIngressGate, UiCloseAndJobObservationFormTwoProducerBarrier)
{
	RigTriggerIngressGate gate;
	ASSERT_TRUE(gate.Open(11u));

	ASSERT_TRUE(gate.RequestCloseFromUi(11u));
	EXPECT_FALSE(gate.TryAcceptUi(11u));
	EXPECT_FALSE(gate.TryAcceptJob(11u));
	EXPECT_TRUE(gate.UiAcknowledged(11u));
	EXPECT_FALSE(gate.JobAcknowledged(11u));
	EXPECT_FALSE(gate.ReadyForAudioQuiescence(11u));

	EXPECT_EQ(11u, gate.ObserveAndAcknowledgeClose());
	EXPECT_TRUE(gate.JobAcknowledged(11u));
	EXPECT_TRUE(gate.ReadyForAudioQuiescence(11u));
	EXPECT_FALSE(gate.TryAcceptJob(11u));
}

TEST(RigTriggerIngressGate, WrongRevisionCannotCloseCurrentIngress)
{
	RigTriggerIngressGate gate;
	ASSERT_TRUE(gate.Open(13u));

	EXPECT_FALSE(gate.RequestCloseFromUi(12u));
	EXPECT_TRUE(gate.TryAcceptUi(13u));
	EXPECT_TRUE(gate.TryAcceptJob(13u));
	EXPECT_FALSE(gate.UiAcknowledged(12u));
	EXPECT_EQ(RigTriggerIngressGate::NoRevision, gate.RequestedClosedRevision());
}

TEST(RigTriggerIngressGate, ReopenClearsOldBarrierAndRejectsOldRevision)
{
	RigTriggerIngressGate gate;
	ASSERT_TRUE(gate.Open(17u));
	ASSERT_TRUE(gate.RequestCloseFromUi(17u));
	ASSERT_EQ(17u, gate.ObserveAndAcknowledgeClose());
	ASSERT_TRUE(gate.ReadyForAudioQuiescence(17u));

	ASSERT_TRUE(gate.Reopen(18u));
	EXPECT_TRUE(gate.TryAcceptUi(18u));
	EXPECT_TRUE(gate.TryAcceptJob(18u));
	EXPECT_FALSE(gate.TryAcceptUi(17u));
	EXPECT_FALSE(gate.UiAcknowledged(17u));
	EXPECT_FALSE(gate.JobAcknowledged(17u));
	EXPECT_FALSE(gate.ReadyForAudioQuiescence(17u));
}

TEST(RigTriggerIngressGate, RepeatedUiCloseIsIdempotentBeforeJobAcknowledgement)
{
	RigTriggerIngressGate gate;
	ASSERT_TRUE(gate.Open(23u));

	EXPECT_TRUE(gate.RequestCloseFromUi(23u));
	const auto firstToken = gate.RequestedCloseToken();
	EXPECT_TRUE(gate.RequestCloseFromUi(23u));
	EXPECT_EQ(firstToken, gate.RequestedCloseToken());
	EXPECT_EQ(23u, gate.ObserveAndAcknowledgeClose());
	EXPECT_TRUE(gate.ReadyForAudioQuiescence(23u));
}

TEST(RigTriggerIngressGate, DelayedOldAcknowledgementCannotSatisfyNewCloseOfSameRevision)
{
	RigTriggerIngressGate gate;
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
	EXPECT_FALSE(gate.ReadyForAudioQuiescence(27u));

	ASSERT_TRUE(gate.AcknowledgeObservedCloseFromJob(newCloseToken));
	EXPECT_TRUE(gate.JobAcknowledged(27u));
	EXPECT_TRUE(gate.ReadyForAudioQuiescence(27u));
}

TEST(RigTriggerIngressGate, CloseForeverRequiresFreshJobAcknowledgementAndCannotReopen)
{
	RigTriggerIngressGate gate;
	ASSERT_TRUE(gate.Open(29u));
	ASSERT_TRUE(gate.RequestCloseFromUi(29u));
	ASSERT_EQ(29u, gate.ObserveAndAcknowledgeClose());
	ASSERT_TRUE(gate.ReadyForAudioQuiescence(29u));

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

	EXPECT_EQ(RigTriggerIngressGate::CloseForeverRevision,
		gate.ObserveAndAcknowledgeClose());
	EXPECT_TRUE(gate.JobAcknowledgedCloseForever());
	EXPECT_TRUE(gate.ReadyForShutdown());
	EXPECT_FALSE(gate.Open(30u));
	EXPECT_FALSE(gate.Reopen(29u));
	EXPECT_FALSE(gate.RequestCloseFromUi(29u));
}

TEST(RigTriggerIngressGate, ConcurrentOpenCannotErasePermanentClose)
{
	for (auto iteration = 0u; iteration < 200u; ++iteration)
	{
		RigTriggerIngressGate gate;
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
		EXPECT_EQ(RigTriggerIngressGate::NoRevision, gate.OpenRevision());
		EXPECT_EQ(RigTriggerIngressGate::CloseForeverRevision,
			gate.RequestedClosedRevision());
		EXPECT_NE(0u, gate.RequestedCloseToken());
		EXPECT_EQ(RigTriggerIngressGate::CloseForeverRevision,
			gate.ObserveAndAcknowledgeClose());
		EXPECT_TRUE(gate.ReadyForShutdown());
	}
}
