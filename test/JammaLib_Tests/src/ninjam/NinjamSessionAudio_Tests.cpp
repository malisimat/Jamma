#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "ninjam/NinjamSession.h"

TEST(NinjamSessionAudio, StopCannotInvalidateBorrowDuringSynchronousConsume)
{
	ninjam::NinjamSession session;
	session.SetAudioFormat(48000u, 256u, 0u, 4u);

	io::JamFile::NinjamConfig config;
	config.Host = "127.0.0.1:1";
	config.User = "lifetime-test";
	config.WorkDir = ".";
	session.Start(config);

	std::atomic_bool stopEntered{ false };
	std::atomic_bool stopReturned{ false };
	std::thread stopThread;
	bool unpublishedBeforeConsume = false;

	{
		ninjam::NinjamConnectionUse connectionUse(session);
		ASSERT_TRUE(connectionUse);

		stopThread = std::thread([&]() {
			stopEntered.store(true, std::memory_order_release);
			session.Stop();
			stopReturned.store(true, std::memory_order_release);
		});

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (!stopEntered.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
				continue;
			}

			ninjam::NinjamConnectionUse laterUse(session);
			if (!laterUse)
			{
				unpublishedBeforeConsume = true;
				break;
			}
			std::this_thread::yield();
		}

		EXPECT_TRUE(unpublishedBeforeConsume);
		EXPECT_FALSE(stopReturned.load(std::memory_order_acquire));

		const float sentinel = 0.0f;
		const float* left = &sentinel;
		const float* right = &sentinel;
		auto frameCount = 1u;
		EXPECT_FALSE(connectionUse->ConsumeStereoPair(0u, left, right, frameCount));
		EXPECT_EQ(nullptr, left);
		EXPECT_EQ(nullptr, right);
		EXPECT_EQ(0u, frameCount);
		EXPECT_FALSE(stopReturned.load(std::memory_order_acquire));
	}

	stopThread.join();
	EXPECT_TRUE(stopReturned.load(std::memory_order_acquire));

	// No permitted test hook exists between the private first connection load and
	// user-count increment. Put Stop at the public acquisition entry, exercise
	// both scheduler leads repeatedly, and require every returned use to hold
	// retirement until its synchronous consume and guarded scope are complete.
	constexpr auto entryOverlapEpochs = 64u;
	auto successfulEntryOverlaps = 0u;
	for (auto epoch = 0u; epoch < entryOverlapEpochs; ++epoch)
	{
		session.Start(config);

		std::atomic_uint ready{ 0u };
		std::atomic_bool begin{ false };
		std::atomic_bool acquireEntering{ false };
		std::atomic_bool acquireFinished{ false };
		std::atomic_bool useAcquired{ false };
		std::atomic_bool consumeClearedOutputs{ false };
		std::atomic_bool releaseUse{ false };
		std::atomic_bool overlapStopReturned{ false };
		std::atomic_bool retirementMissedUse{ false };

		std::thread acquireThread([&]() {
			ready.fetch_add(1u, std::memory_order_release);
			while (!begin.load(std::memory_order_acquire))
				std::this_thread::yield();

			acquireEntering.store(true, std::memory_order_release);
			if ((epoch % 4u) == 1u || (epoch % 4u) == 3u)
				std::this_thread::yield();

			ninjam::NinjamConnectionUse overlappingUse(session);
			if (!overlappingUse)
			{
				acquireFinished.store(true, std::memory_order_release);
				return;
			}

			useAcquired.store(true, std::memory_order_release);
			const float sentinel = 0.0f;
			const float* overlapLeft = &sentinel;
			const float* overlapRight = &sentinel;
			auto overlapFrames = 1u;
			const auto consumed = overlappingUse->ConsumeStereoPair(
				0u, overlapLeft, overlapRight, overlapFrames);
			consumeClearedOutputs.store(!consumed
				&& overlapLeft == nullptr
				&& overlapRight == nullptr
				&& overlapFrames == 0u,
				std::memory_order_release);
			acquireFinished.store(true, std::memory_order_release);

			while (!releaseUse.load(std::memory_order_acquire))
			{
				if (overlapStopReturned.load(std::memory_order_acquire))
					retirementMissedUse.store(true, std::memory_order_release);
				std::this_thread::yield();
			}
		});

		std::thread overlapStopThread([&]() {
			ready.fetch_add(1u, std::memory_order_release);
			while (!begin.load(std::memory_order_acquire)
				|| !acquireEntering.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			if ((epoch % 4u) == 2u || (epoch % 4u) == 3u)
				std::this_thread::yield();

			session.Stop();
			overlapStopReturned.store(true, std::memory_order_release);
		});

		const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (ready.load(std::memory_order_acquire) != 2u
			&& std::chrono::steady_clock::now() < readyDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(2u, ready.load(std::memory_order_acquire));
		begin.store(true, std::memory_order_release);

		const auto acquireDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!acquireFinished.load(std::memory_order_acquire)
			&& std::chrono::steady_clock::now() < acquireDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_TRUE(acquireFinished.load(std::memory_order_acquire));

		if (useAcquired.load(std::memory_order_acquire))
		{
			++successfulEntryOverlaps;
			EXPECT_TRUE(consumeClearedOutputs.load(std::memory_order_acquire));
			EXPECT_FALSE(overlapStopReturned.load(std::memory_order_acquire));
		}

		releaseUse.store(true, std::memory_order_release);
		acquireThread.join();
		overlapStopThread.join();

		EXPECT_TRUE(overlapStopReturned.load(std::memory_order_acquire));
		EXPECT_FALSE(retirementMissedUse.load(std::memory_order_acquire));
	}

	EXPECT_GT(successfulEntryOverlaps, 0u);
}
