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
}
