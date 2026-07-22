#include "gtest/gtest.h"
#include "ninjam/NinjamTimingCoordinator.h"
#include "io/UserConfig.h"

using ninjam::NinjamTiming;
using ninjam::NinjamTimingCoordinator;
using utils::Timer;

namespace
{
	NinjamTiming MakeTiming(unsigned int length, unsigned int position)
	{
		NinjamTiming timing;
		timing.IsConnected = true;
		timing.IsValid = true;
		timing.DeviceSampleRate = 48000u;
		timing.SourceSampleRate = 44100u;
		timing.IntervalLengthSamps = length;
		timing.IntervalPositionSamps = position;
		return timing;
	}

	void Connect(NinjamTimingCoordinator& coordinator, bool prompt, bool push,
		const std::optional<timing::QuantisationTiming>& local = std::nullopt)
	{
		ninjam::NinjamTempoJoinOptions options;
		options.PromptBeforeApplyingRemoteTempo = prompt;
		options.PushLocalTempoOnJoin = push;
		coordinator.Connect(options, local);
	}
}

TEST(NinjamTimingCoordinator, FirstGenerationInvalidatesOldCorrectionsWithoutEmittingPhaseMovement)
{
	Timer clock;
	clock.SetQuantisation(100u, Timer::QUANTISE_MULTIPLE);
	clock.SetSeedSourceLength(1000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);

	NinjamTiming timing;
	timing.IsConnected = true;
	timing.IsValid = true;
	timing.DeviceSampleRate = 48000u;
	timing.IntervalLengthSamps = 1000u;
	timing.IntervalPositionSamps = 300u;
	const auto update = coordinator.Observe(timing, std::nullopt, true, io::UserConfig{}, clock);

	EXPECT_TRUE(update.InvalidatePendingCorrections);
	EXPECT_FALSE(update.PhaseCorrection.has_value());
	EXPECT_EQ(1u, coordinator.Diagnostics().GenerationChanges);
}

TEST(NinjamTimingCoordinator, AcceptedWrapProducesOneGenerationTaggedCorrection)
{
	Timer clock;
	clock.SetQuantisation(100u, Timer::QUANTISE_MULTIPLE);
	clock.SetSeedSourceLength(1000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, false);

	NinjamTiming timing;
	timing.IsConnected = true;
	timing.IsValid = true;
	timing.DeviceSampleRate = 48000u;
	timing.IntervalLengthSamps = 1000u;
	timing.IntervalPositionSamps = 900u;
	coordinator.Observe(timing, std::nullopt, false, io::UserConfig{}, clock);
	timing.IntervalPositionSamps = 10u;
	const auto update = coordinator.Observe(timing, std::nullopt, false, io::UserConfig{}, clock);

	ASSERT_TRUE(update.PhaseCorrection.has_value());
	EXPECT_NE(0, update.PhaseCorrection->DeltaSamps);
	EXPECT_NE(0u, update.PhaseCorrection->Generation);
	EXPECT_EQ(1u, coordinator.Diagnostics().PhaseEventsQueued);
}

TEST(NinjamTimingCoordinator, AutoAcceptsRemoteTempoWithoutLocalContent)
{
	Timer clock;
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	const auto update = coordinator.Observe(MakeTiming(384000u, 100u), std::nullopt, false, io::UserConfig{}, clock);
	ASSERT_TRUE(update.ClockSettings.has_value());
	EXPECT_FALSE(update.PromptForTempoChange);
	EXPECT_EQ(384000ul, update.ClockSettings->SeedLengthSamps);
}

TEST(NinjamTimingCoordinator, PromptAcceptRejectAndChangedProposal)
{
	Timer clock;
	clock.SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
	clock.SetSeedSourceLength(384000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	auto update = coordinator.Observe(MakeTiming(480000u, 100u), std::nullopt, true, io::UserConfig{}, clock);
	EXPECT_TRUE(update.PromptForTempoChange);
	EXPECT_TRUE(coordinator.PendingTempoChange().has_value());
	EXPECT_FALSE(coordinator.ResolveTempoChange(false, std::nullopt, clock).ClockSettings.has_value());
	update = coordinator.Observe(MakeTiming(480000u, 200u), std::nullopt, true, io::UserConfig{}, clock);
	EXPECT_FALSE(update.PromptForTempoChange);
	update = coordinator.Observe(MakeTiming(576000u, 100u), std::nullopt, true, io::UserConfig{}, clock);
	EXPECT_TRUE(update.PromptForTempoChange);
	auto accepted = coordinator.ResolveTempoChange(true, std::nullopt, clock);
	EXPECT_TRUE(accepted.ClockSettings.has_value());
}

TEST(NinjamTimingCoordinator, LocalRequestWaitsForWrapAndAcknowledges)
{
	Timer clock;
	timing::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, true, local);
	coordinator.Observe(MakeTiming(384000u, 300000u), local, true, io::UserConfig{}, clock);
	auto update = coordinator.Observe(MakeTiming(384000u, 1000u), local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(update.TempoRequest.has_value());
	EXPECT_FLOAT_EQ(120.0f, update.TempoRequest->Bpm);
}

TEST(NinjamTimingCoordinator, DisconnectClearsPromptRequestAndCorrections)
{
	Timer clock;
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, true, timing::QuantisationTiming{ 24000u, 384000u, 16u, 120.0f, 16u });
	coordinator.Observe(MakeTiming(480000u, 100u), std::nullopt, true, io::UserConfig{}, clock);
	ASSERT_TRUE(coordinator.PendingTempoChange().has_value());
	coordinator.Disconnect();
	EXPECT_FALSE(coordinator.IsConnected());
	EXPECT_FALSE(coordinator.PendingTempoChange().has_value());
	EXPECT_FALSE(coordinator.Observe(MakeTiming(480000u, 10u), std::nullopt, true, io::UserConfig{}, clock).PhaseCorrection.has_value());
}

TEST(NinjamTimingCoordinator, LongRunningConvertedTimingSimulationStaysGenerationSafe)
{
	Timer clock;
	clock.SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
	clock.SetSeedSourceLength(384000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, false);
	const unsigned int lengths[] = { 384000u, 768000u, 192000u, 123457u };
	long long maxDelta = 0;
	for (unsigned int generation = 0u; generation < 4u; ++generation)
	{
		const auto length = lengths[generation];
		for (unsigned int interval = 0u; interval < 250u; ++interval)
		{
			for (unsigned int step = 0u; step < 8u; ++step)
			{
				const auto jitter = static_cast<int>((interval + step) % 5u) - 2;
				auto position = static_cast<unsigned int>((static_cast<unsigned long long>(step) * length / 8u + length + jitter) % length);
				auto update = coordinator.Observe(MakeTiming(length, position), std::nullopt, false, io::UserConfig{}, clock);
				if (update.PhaseCorrection.has_value())
					maxDelta = std::max(maxDelta, std::llabs(update.PhaseCorrection->DeltaSamps));
			}
			coordinator.Observe(MakeTiming(length, length - 10u), std::nullopt, false, io::UserConfig{}, clock);
			coordinator.Observe(MakeTiming(length, 10u), std::nullopt, false, io::UserConfig{}, clock);
		}
		if (generation == 1u)
		{
			coordinator.Disconnect();
			EXPECT_FALSE(coordinator.Observe(MakeTiming(length, 0u), std::nullopt, false, io::UserConfig{}, clock).PhaseCorrection.has_value());
			Connect(coordinator, false, false);
		}
	}
	EXPECT_LE(maxDelta, static_cast<long long>(constants::DefaultBufferSizeSamps * 2u));
	EXPECT_GT(coordinator.Diagnostics().GenerationChanges, 0u);
}