///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

// TDD tests for vst::Vst3ControllerEditQueue — the fixed-capacity SPSC ring
// buffer used to forward UI-thread performEdit() values to the audio thread.
// Pure, VST3-SDK-independent logic, so these tests always run.

#include "gtest/gtest.h"
#include "vst/Vst3ControllerEditQueue.h"

using vst::Vst3ControllerEditQueue;

// -----------------------------------------------------------------------
// Suite: Vst3ControllerEditQueueDefault
// -----------------------------------------------------------------------

TEST(Vst3ControllerEditQueueDefault, PopFailsWhenEmpty)
{
	Vst3ControllerEditQueue queue;
	Vst3ControllerEditQueue::Edit edit;
	EXPECT_FALSE(queue.Pop(edit));
}

// -----------------------------------------------------------------------
// Suite: Vst3ControllerEditQueuePushPop
// -----------------------------------------------------------------------

TEST(Vst3ControllerEditQueuePushPop, SingleRoundTrip)
{
	Vst3ControllerEditQueue queue;
	ASSERT_TRUE(queue.Push(42u, 0.75));

	Vst3ControllerEditQueue::Edit edit;
	ASSERT_TRUE(queue.Pop(edit));
	EXPECT_EQ(edit.ParamId, 42u);
	EXPECT_DOUBLE_EQ(edit.Value, 0.75);

	EXPECT_FALSE(queue.Pop(edit));
}

TEST(Vst3ControllerEditQueuePushPop, PreservesFifoOrder)
{
	Vst3ControllerEditQueue queue;
	ASSERT_TRUE(queue.Push(1u, 0.1));
	ASSERT_TRUE(queue.Push(2u, 0.2));
	ASSERT_TRUE(queue.Push(3u, 0.3));

	Vst3ControllerEditQueue::Edit edit;

	ASSERT_TRUE(queue.Pop(edit));
	EXPECT_EQ(edit.ParamId, 1u);

	ASSERT_TRUE(queue.Pop(edit));
	EXPECT_EQ(edit.ParamId, 2u);

	ASSERT_TRUE(queue.Pop(edit));
	EXPECT_EQ(edit.ParamId, 3u);

	EXPECT_FALSE(queue.Pop(edit));
}

TEST(Vst3ControllerEditQueuePushPop, ArbitraryParamIdRoundTrips)
{
	Vst3ControllerEditQueue queue;
	ASSERT_TRUE(queue.Push(0xDEADBEEFu, -1.5));

	Vst3ControllerEditQueue::Edit edit;
	ASSERT_TRUE(queue.Pop(edit));
	EXPECT_EQ(edit.ParamId, 0xDEADBEEFu);
	EXPECT_DOUBLE_EQ(edit.Value, -1.5);
}

// -----------------------------------------------------------------------
// Suite: Vst3ControllerEditQueueCapacity
// -----------------------------------------------------------------------

TEST(Vst3ControllerEditQueueCapacity, FillingToCapacitySucceeds)
{
	Vst3ControllerEditQueue queue;
	for (std::size_t i = 0; i < Vst3ControllerEditQueue::Capacity; ++i)
		EXPECT_TRUE(queue.Push(static_cast<std::uint32_t>(i), static_cast<double>(i)));
}

TEST(Vst3ControllerEditQueueCapacity, PushBeyondCapacityDropsAndReturnsFalse)
{
	Vst3ControllerEditQueue queue;
	for (std::size_t i = 0; i < Vst3ControllerEditQueue::Capacity; ++i)
		ASSERT_TRUE(queue.Push(static_cast<std::uint32_t>(i), static_cast<double>(i)));

	// Queue is full: the next push must be dropped, not overwrite an unread
	// slot or corrupt ordering.
	EXPECT_FALSE(queue.Push(9999u, 9999.0));

	// The oldest entry (index 0) must still be the first one popped.
	Vst3ControllerEditQueue::Edit edit;
	ASSERT_TRUE(queue.Pop(edit));
	EXPECT_EQ(edit.ParamId, 0u);
}

TEST(Vst3ControllerEditQueueCapacity, WrapAroundAfterManyCyclesPreservesOrder)
{
	Vst3ControllerEditQueue queue;

	// Push/pop repeatedly well past the capacity so the internal indices
	// wrap around multiple times, verifying the mask-based indexing.
	std::uint32_t nextPush = 0u;
	std::uint32_t nextExpectedPop = 0u;

	for (int cycle = 0; cycle < 10; ++cycle)
	{
		for (int i = 0; i < 100; ++i)
		{
			ASSERT_TRUE(queue.Push(nextPush, static_cast<double>(nextPush)));
			++nextPush;
		}

		for (int i = 0; i < 100; ++i)
		{
			Vst3ControllerEditQueue::Edit edit;
			ASSERT_TRUE(queue.Pop(edit));
			EXPECT_EQ(edit.ParamId, nextExpectedPop);
			++nextExpectedPop;
		}
	}

	Vst3ControllerEditQueue::Edit edit;
	EXPECT_FALSE(queue.Pop(edit));
}

// -----------------------------------------------------------------------
// Suite: Vst3ControllerEditQueueClear
// -----------------------------------------------------------------------

TEST(Vst3ControllerEditQueueClear, DropsAllPendingEntries)
{
	Vst3ControllerEditQueue queue;
	ASSERT_TRUE(queue.Push(1u, 1.0));
	ASSERT_TRUE(queue.Push(2u, 2.0));

	queue.Clear();

	Vst3ControllerEditQueue::Edit edit;
	EXPECT_FALSE(queue.Pop(edit));
}

TEST(Vst3ControllerEditQueueClear, QueueUsableAfterClear)
{
	Vst3ControllerEditQueue queue;
	ASSERT_TRUE(queue.Push(1u, 1.0));
	queue.Clear();

	ASSERT_TRUE(queue.Push(7u, 7.0));
	Vst3ControllerEditQueue::Edit edit;
	ASSERT_TRUE(queue.Pop(edit));
	EXPECT_EQ(edit.ParamId, 7u);
}
