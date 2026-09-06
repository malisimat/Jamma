#include "gtest/gtest.h"
#include "ninjam/NinjamTimingTracker.h"

using ninjam::NinjamTimingEventType;
using ninjam::NinjamTimingObservation;
using ninjam::NinjamTimingTracker;

namespace
{
	NinjamTimingObservation Observation(unsigned int length, unsigned int position)
	{
		return { length, position, position };
	}
}

TEST(NinjamTimingTracker, EmitsGenerationWrapAndJoinEvents)
{
	NinjamTimingTracker tracker;
	tracker.Connect();
	auto generation = tracker.Observe(Observation(1000u, 300u));
	ASSERT_TRUE(generation.has_value());
	EXPECT_EQ(NinjamTimingEventType::GenerationChanged, generation->Type);
	tracker.BeginJoinAlignment(700ul);
	EXPECT_FALSE(tracker.Observe(Observation(1000u, 900u)).has_value());
	auto join = tracker.Observe(Observation(1000u, 40u));
	ASSERT_TRUE(join.has_value());
	EXPECT_EQ(NinjamTimingEventType::Join, join->Type);
	EXPECT_EQ(-400, join->RemoteMasterPhaseCorrectionSamps);
	EXPECT_EQ(1ul, join->RemoteWrapCount);
}

TEST(NinjamTimingTracker, RejectsBackwardJumpsAndDuplicates)
{
	NinjamTimingTracker tracker;
	tracker.Connect();
	tracker.Observe(Observation(1000u, 700u));
	EXPECT_FALSE(tracker.Observe(Observation(1000u, 400u)).has_value());
	EXPECT_FALSE(tracker.Observe(Observation(1000u, 700u)).has_value());
	const auto diagnostics = tracker.Diagnostics();
	EXPECT_EQ(1u, diagnostics.ObservationsRejected);
	EXPECT_EQ(1u, diagnostics.DuplicateObservations);
}

TEST(NinjamTimingTracker, DisconnectClearsConnectedStateAndEvents)
{
	NinjamTimingTracker tracker;
	tracker.Connect();
	tracker.Observe(Observation(1000u, 900u));
	tracker.Disconnect();
	EXPECT_FALSE(tracker.IsConnected());
	EXPECT_FALSE(tracker.Observe(Observation(1000u, 10u)).has_value());
}
