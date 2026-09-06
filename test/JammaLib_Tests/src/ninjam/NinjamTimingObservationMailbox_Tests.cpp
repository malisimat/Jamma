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
	timing.HasLocalTransport = true;
	timing.LocalTransport.MasterLengthSamps = 12u;
	timing.LocalTransport.MasterPhaseSamps = 13u;
	timing.LocalTransport.LoopCount = 14u;
	timing.LocalTransport.AbsoluteSamplePos = 15u;
	timing.LocalTransport.SceneSamplePos = 16u;
	timing.HasDeviceAudioSampleAtObservation = true;
	timing.LocalMasterAbsoluteSampleAtObservation = 13u;
	timing.DeviceAudioSampleAtObservation = 17u;
	timing.ObservationAgeSamps = 18u;

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
	EXPECT_TRUE(observed->HasLocalTransport);
	EXPECT_EQ(12u, observed->LocalTransport.MasterLengthSamps);
	EXPECT_EQ(13u, observed->LocalTransport.MasterPhaseSamps);
	EXPECT_EQ(14u, observed->LocalTransport.LoopCount);
	EXPECT_EQ(15u, observed->LocalTransport.AbsoluteSamplePos);
	EXPECT_EQ(16u, observed->LocalTransport.SceneSamplePos);
	EXPECT_TRUE(observed->HasDeviceAudioSampleAtObservation);
	EXPECT_EQ(13u, observed->LocalMasterAbsoluteSampleAtObservation);
	EXPECT_EQ(17u, observed->DeviceAudioSampleAtObservation);
	EXPECT_EQ(18u, observed->ObservationAgeSamps);
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

TEST(NinjamTimingObservationMailbox, ConcurrentReadProvesOverlapAndCoherence)
{
	ninjam::NinjamTimingObservationMailbox mailbox;
	std::atomic_bool readerReady{ false };
	std::atomic_bool writerStarted{ false };
	std::atomic_bool writerFinished{ false };
	std::atomic_uint successfulOverlappingReads{ 0u };
	constexpr auto requiredOverlappingReads = 64u;
	constexpr std::uint64_t finalSequence = 400000u;

	const auto makeTiming = [](std::uint64_t sequence)
		{
			ninjam::NinjamTiming timing;
			timing.IsConnected = (sequence & 1u) != 0u;
			timing.IsValid = (sequence & 2u) != 0u;
			timing.IntervalLengthSamps = static_cast<unsigned int>(1000u + sequence);
			timing.IntervalPositionSamps = static_cast<unsigned int>(2000u + sequence);
			timing.DeviceSampleRate = static_cast<unsigned int>(3000u + sequence);
			timing.SourceSampleRate = static_cast<unsigned int>(4000u + sequence);
			timing.Bpm = static_cast<float>(5000u + sequence);
			timing.Bpi = static_cast<unsigned int>(6000u + sequence);
			timing.Generation = 7000u + sequence;
			timing.RemoteWrapCount = static_cast<unsigned long>(8000u + sequence);
			timing.ObservationSequence = sequence;
			timing.HasLocalTransport = (sequence & 4u) != 0u;
			timing.LocalTransport.MasterLengthSamps = 9000u + sequence;
			timing.LocalTransport.MasterPhaseSamps = 10000u + sequence;
			timing.LocalTransport.LoopCount = 11000u + sequence;
			timing.LocalTransport.AbsoluteSamplePos = 12000u + sequence;
			timing.LocalTransport.SceneSamplePos = 13000u + sequence;
			timing.HasDeviceAudioSampleAtObservation = (sequence & 8u) != 0u;
			timing.LocalMasterAbsoluteSampleAtObservation = 9000u + sequence;
			timing.DeviceAudioSampleAtObservation = 14000u + sequence;
			timing.ObservationAgeSamps = 15000u + sequence;
			return timing;
		};

	std::thread writer([&]()
		{
			while (!readerReady.load(std::memory_order_acquire))
				std::this_thread::yield();

			writerStarted.store(true, std::memory_order_release);
			std::uint64_t sequence = 1u;
			while (successfulOverlappingReads.load(std::memory_order_acquire)
				< requiredOverlappingReads)
			{
				mailbox.Publish(makeTiming(sequence));
				++sequence;
				std::this_thread::yield();
			}

			mailbox.Publish(makeTiming(finalSequence));
			writerFinished.store(true, std::memory_order_release);
		});

	readerReady.store(true, std::memory_order_release);
	while (!writerStarted.load(std::memory_order_acquire))
		std::this_thread::yield();

	unsigned int readAttemptsDuringWrite = 0u;
	while (!writerFinished.load(std::memory_order_acquire))
	{
		++readAttemptsDuringWrite;
		const auto observed = mailbox.ReadLatest();
		if (!observed.has_value())
			continue;

		const auto sequence = observed->ObservationSequence;
		EXPECT_EQ((sequence & 1u) != 0u, observed->IsConnected) << "sequence=" << sequence;
		EXPECT_EQ((sequence & 2u) != 0u, observed->IsValid) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 1000u, observed->IntervalLengthSamps) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 2000u, observed->IntervalPositionSamps) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 3000u, observed->DeviceSampleRate) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 4000u, observed->SourceSampleRate) << "sequence=" << sequence;
		EXPECT_EQ(static_cast<float>(sequence + 5000u), observed->Bpm) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 6000u, observed->Bpi) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 7000u, observed->Generation) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 8000u, observed->RemoteWrapCount) << "sequence=" << sequence;
		EXPECT_EQ((sequence & 4u) != 0u, observed->HasLocalTransport) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 9000u, observed->LocalTransport.MasterLengthSamps) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 10000u, observed->LocalTransport.MasterPhaseSamps) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 11000u, observed->LocalTransport.LoopCount) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 12000u, observed->LocalTransport.AbsoluteSamplePos) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 13000u, observed->LocalTransport.SceneSamplePos) << "sequence=" << sequence;
		EXPECT_EQ((sequence & 8u) != 0u, observed->HasDeviceAudioSampleAtObservation) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 9000u, observed->LocalMasterAbsoluteSampleAtObservation) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 14000u, observed->DeviceAudioSampleAtObservation) << "sequence=" << sequence;
		EXPECT_EQ(sequence + 15000u, observed->ObservationAgeSamps) << "sequence=" << sequence;
		successfulOverlappingReads.fetch_add(1u, std::memory_order_release);
	}

	writer.join();
	EXPECT_GT(readAttemptsDuringWrite, 0u);
	EXPECT_GE(successfulOverlappingReads.load(std::memory_order_acquire),
		requiredOverlappingReads);

	const auto finalObservation = mailbox.ReadLatest();
	ASSERT_TRUE(finalObservation.has_value());
	EXPECT_EQ(finalSequence, finalObservation->ObservationSequence);
	EXPECT_EQ(finalSequence + 1000u, finalObservation->IntervalLengthSamps);
	EXPECT_EQ(finalSequence + 7000u, finalObservation->Generation);
	EXPECT_EQ((finalSequence & 4u) != 0u, finalObservation->HasLocalTransport);
	EXPECT_EQ(finalSequence + 12000u, finalObservation->LocalTransport.AbsoluteSamplePos);
	EXPECT_EQ((finalSequence & 8u) != 0u, finalObservation->HasDeviceAudioSampleAtObservation);
	EXPECT_EQ(finalSequence + 9000u, finalObservation->LocalMasterAbsoluteSampleAtObservation);
	EXPECT_EQ(finalSequence + 14000u, finalObservation->DeviceAudioSampleAtObservation);
	EXPECT_EQ(finalSequence + 15000u, finalObservation->ObservationAgeSamps);
}
