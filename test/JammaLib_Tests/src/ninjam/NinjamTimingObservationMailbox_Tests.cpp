#include "gtest/gtest.h"
#include "./ninjam/NinjamTimingObservationMailbox.h"

#include <atomic>
#include <thread>

TEST(NinjamTimingObservationMailbox, ReadsPublishedObservationAsOneValue)
{
	ninjam::NinjamTimingObservationMailbox mailbox;
	ninjam::NinjamTiming timing;
	timing.IsConnected = true;
	timing.IsValid = true;
	timing.IntervalLengthSamps = 192000u;
	timing.IntervalPositionSamps = 48000u;
	timing.DeviceSampleRate = 48000u;
	timing.SourceSampleRate = 44100u;
	timing.Bpm = 120.0f;
	timing.Bpi = 8u;
	timing.Generation = 3u;
	timing.RemoteWrapCount = 7ul;
	timing.ObservationSequence = 11u;
	timing.LocalBlockStartSample = 13u;

	mailbox.Publish(timing);
	const auto observed = mailbox.ReadLatest();

	ASSERT_TRUE(observed.has_value());
	EXPECT_TRUE(observed->IsConnected);
	EXPECT_TRUE(observed->IsValid);
	EXPECT_EQ(192000u, observed->IntervalLengthSamps);
	EXPECT_EQ(48000u, observed->IntervalPositionSamps);
	EXPECT_EQ(48000u, observed->DeviceSampleRate);
	EXPECT_EQ(44100u, observed->SourceSampleRate);
	EXPECT_EQ(120.0f, observed->Bpm);
	EXPECT_EQ(8u, observed->Bpi);
	EXPECT_EQ(3u, observed->Generation);
	EXPECT_EQ(7ul, observed->RemoteWrapCount);
	EXPECT_EQ(11u, observed->ObservationSequence);
	EXPECT_EQ(13u, observed->LocalBlockStartSample);
}

TEST(NinjamTimingObservationMailbox, ReplacesThePreviousCompleteObservation)
{
	ninjam::NinjamTimingObservationMailbox mailbox;
	ninjam::NinjamTiming first;
	first.IsConnected = true;
	first.IsValid = true;
	first.IntervalLengthSamps = 192000u;
	first.ObservationSequence = 1u;
	mailbox.Publish(first);

	ninjam::NinjamTiming second;
	second.IsConnected = true;
	second.IsValid = true;
	second.IntervalLengthSamps = 240000u;
	second.IntervalPositionSamps = 42u;
	second.ObservationSequence = 2u;
	mailbox.Publish(second);

	const auto observed = mailbox.ReadLatest();
	ASSERT_TRUE(observed.has_value());
	EXPECT_EQ(240000u, observed->IntervalLengthSamps);
	EXPECT_EQ(42u, observed->IntervalPositionSamps);
	EXPECT_EQ(2u, observed->ObservationSequence);
}

TEST(NinjamTimingObservationMailbox, ConcurrentReadsAreCompleteOrDeferred)
{
	ninjam::NinjamTimingObservationMailbox mailbox;
	std::atomic_bool writerFinished{ false };

	std::thread writer([&mailbox, &writerFinished]()
		{
			for (std::uint64_t sequence = 1u; sequence <= 200000u; ++sequence)
			{
				ninjam::NinjamTiming timing;
				timing.IsConnected = true;
				timing.IsValid = true;
				timing.IntervalLengthSamps = static_cast<unsigned int>(sequence);
				timing.IntervalPositionSamps = static_cast<unsigned int>(sequence + 1u);
				timing.DeviceSampleRate = static_cast<unsigned int>(sequence + 2u);
				timing.SourceSampleRate = static_cast<unsigned int>(sequence + 3u);
				timing.Bpm = static_cast<float>(sequence + 4u);
				timing.Bpi = static_cast<unsigned int>(sequence + 5u);
				timing.Generation = sequence + 6u;
				timing.RemoteWrapCount = static_cast<unsigned long>(sequence + 7u);
				timing.ObservationSequence = sequence;
				timing.LocalBlockStartSample = sequence + 8u;
				mailbox.Publish(timing);
			}
			writerFinished.store(true, std::memory_order_release);
		});

	while (!writerFinished.load(std::memory_order_acquire))
	{
		const auto observed = mailbox.ReadLatest();
		if (!observed.has_value())
			continue;

		const auto sequence = observed->ObservationSequence;
		EXPECT_TRUE(observed->IsConnected);
		EXPECT_TRUE(observed->IsValid);
		EXPECT_EQ(sequence, observed->IntervalLengthSamps);
		EXPECT_EQ(sequence + 1u, observed->IntervalPositionSamps);
		EXPECT_EQ(sequence + 2u, observed->DeviceSampleRate);
		EXPECT_EQ(sequence + 3u, observed->SourceSampleRate);
		EXPECT_EQ(static_cast<float>(sequence + 4u), observed->Bpm);
		EXPECT_EQ(sequence + 5u, observed->Bpi);
		EXPECT_EQ(sequence + 6u, observed->Generation);
		EXPECT_EQ(sequence + 7u, observed->RemoteWrapCount);
		EXPECT_EQ(sequence + 8u, observed->LocalBlockStartSample);
	}

	writer.join();
}