#include "gtest/gtest.h"
#include "ninjam/NinjamTimingCoordinator.h"
#include "io/UserConfig.h"

using ninjam::NinjamTiming;
using ninjam::NinjamTimingCoordinator;
using ninjam::NinjamTimingUpdate;
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

	NinjamTiming MakeTimingTempo(unsigned int length, unsigned int position, float bpm, unsigned int bpi)
	{
		auto timing = MakeTiming(length, position);
		timing.Bpm = bpm;
		timing.Bpi = bpi;
		return timing;
	}

	void Connect(NinjamTimingCoordinator& coordinator, bool prompt, bool push,
		const std::optional<engine::QuantisationTiming>& local = std::nullopt)
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

TEST(NinjamTimingCoordinator, AcceptedPromptUsesLatestAnchoredObservation)
{
	Timer clock;
	clock.SetSeedSourceLength(384000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	auto first = MakeTiming(480000u, 100u);
	first.AudioBlockStartSample = 1000u;
	EXPECT_TRUE(coordinator.Observe(first, std::nullopt, true, io::UserConfig{}, clock).PromptForTempoChange);

	auto latest = MakeTiming(480000u, 200u);
	latest.AudioBlockStartSample = 5000u;
	EXPECT_FALSE(coordinator.Observe(latest, std::nullopt, true, io::UserConfig{}, clock).PromptForTempoChange);
	const auto accepted = coordinator.ResolveTempoChange(true, std::nullopt, clock);
	ASSERT_TRUE(accepted.ClockSettings.has_value());
	EXPECT_EQ(200u, accepted.ClockSettings->PhaseSamps);
	EXPECT_EQ(5000u, accepted.ClockSettings->AudioBlockStartSample);
}

TEST(NinjamTimingCoordinator, RejectedDifferentTempoDoesNotEmitWrapCorrection)
{
	Timer clock;
	clock.SetSeedSourceLength(384000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	coordinator.Observe(MakeTiming(480000u, 400000u), std::nullopt, true, io::UserConfig{}, clock);
	const auto rejected = coordinator.ResolveTempoChange(false, std::nullopt, clock);
	EXPECT_TRUE(rejected.InvalidatePendingCorrections);
	const auto update = coordinator.Observe(MakeTiming(480000u, 1000u), std::nullopt, true,
		io::UserConfig{}, clock);
	EXPECT_FALSE(update.PhaseCorrection.has_value());
}

TEST(NinjamTimingCoordinator, FixedOneBpmAcknowledgementIncludesBothEdges)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator lower;
	Connect(lower, false, true, local);
	lower.Observe(MakeTimingTempo(480000u, 400000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	lower.Observe(MakeTimingTempo(480000u, 1000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	lower.NotifyTempoRequestSent(true);
	lower.Observe(MakeTimingTempo(384000u, 2000u, 119.0f, 16u), local, true, io::UserConfig{}, clock);
	EXPECT_EQ(ninjam::TempoRequestState::Acknowledged, lower.RequestState());

	NinjamTimingCoordinator upper;
	Connect(upper, false, true, local);
	upper.Observe(MakeTimingTempo(480000u, 400000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	upper.Observe(MakeTimingTempo(480000u, 1000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	upper.NotifyTempoRequestSent(true);
	upper.Observe(MakeTimingTempo(384000u, 2000u, 121.0f, 16u), local, true, io::UserConfig{}, clock);
	EXPECT_EQ(ninjam::TempoRequestState::Acknowledged, upper.RequestState());
}

TEST(NinjamTimingCoordinator, ClassifiesContinuousAndBlockSyncAtFixedOneBpmBoundary)
{
	const engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::ContinuousSync,
		NinjamTimingCoordinator::SelectLocalFollowPolicy(local, 120.0f));
	EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::ContinuousSync,
		NinjamTimingCoordinator::SelectLocalFollowPolicy(local, 120.99f));
	EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::ContinuousSync,
		NinjamTimingCoordinator::SelectLocalFollowPolicy(local, 121.0f));
	EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::BlockSync,
		NinjamTimingCoordinator::SelectLocalFollowPolicy(local, 118.99f));
	EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::BlockSync,
		NinjamTimingCoordinator::SelectLocalFollowPolicy(local, 121.01f));
	EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::ContinuousSync,
		NinjamTimingCoordinator::SelectLocalFollowPolicy(std::nullopt, 90.0f));
}

TEST(NinjamTimingCoordinator, LocalRequestWaitsForWrapAndAcknowledges)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, true, local);
	coordinator.Observe(MakeTiming(384000u, 300000u), local, true, io::UserConfig{}, clock);
	auto update = coordinator.Observe(MakeTiming(384000u, 1000u), local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(update.TempoRequest.has_value());
	EXPECT_FLOAT_EQ(120.0f, update.TempoRequest->Bpm);
}

TEST(NinjamTimingCoordinator, DefaultJoinOptionsPushValidLocalTempo)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	coordinator.Connect(ninjam::NinjamTempoJoinOptions{}, local);

	coordinator.Observe(MakeTimingTempo(480000u, 400000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	const auto update = coordinator.Observe(MakeTimingTempo(480000u, 1000u, 90.0f, 8u),
		local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(update.TempoRequest.has_value());
	EXPECT_FLOAT_EQ(local.Bpm, update.TempoRequest->Bpm);
	EXPECT_EQ(local.Bpi, update.TempoRequest->Bpi);
}

TEST(NinjamTimingCoordinator, DisconnectClearsPromptRequestAndCorrections)
{
	Timer clock;
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, true, engine::QuantisationTiming{ 24000u, 384000u, 16u, 120.0f, 16u });
	coordinator.Observe(MakeTiming(480000u, 100u), std::nullopt, true, io::UserConfig{}, clock);
	ASSERT_FALSE(coordinator.PendingTempoChange().has_value());
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

// ── Phase 3: join alignment accepts full documented range (§2.2) ─────────────

namespace
{
	// Drives the coordinator through a generation change plus a frozen join delta
	// of the requested magnitude, then triggers the wrap that emits the Join.
	// The join delta = circularDifference(localOffset, remoteAnchorPos). The anchor
	// sits in the final quarter so the following backward move is a valid wrap, and
	// localOffset is chosen so the frozen delta equals `delta`.
	NinjamTimingUpdate DriveJoin(unsigned int length, long long delta)
	{
		const auto anchorPos = length - (length / 8u);
		const auto localOffset = static_cast<unsigned int>(
			(static_cast<long long>(anchorPos) - delta + length) % length);
		Timer clock;
		clock.SetSeedSourceLength(length);
		clock.Tick(localOffset, 0u);
		NinjamTimingCoordinator coordinator;
		Connect(coordinator, false, false);

		// Generation change: records remote anchor and freezes the join delta.
		coordinator.Observe(MakeTiming(length, anchorPos), std::nullopt, false, io::UserConfig{}, clock);
		// A backward move from the final quarter into the first quarter is the wrap
		// that releases the pending Join event.
		return coordinator.Observe(MakeTiming(length, 1000u), std::nullopt, false, io::UserConfig{}, clock);
	}
}

TEST(NinjamTimingCoordinator, QuarterIntervalJoinIsAccepted)
{
	// 96000-sample interval at 48 kHz; a quarter-interval (24000) join is far above
	// the old two-buffer cap yet must be accepted through the join path.
	const auto update = DriveJoin(96000u, 24000);
	ASSERT_TRUE(update.PhaseCorrection.has_value());
	EXPECT_TRUE(update.PhaseCorrection->IsJoin);
	EXPECT_EQ(24000, update.PhaseCorrection->DeltaSamps);
}

TEST(NinjamTimingCoordinator, HalfIntervalJoinIsAccepted)
{
	// The half-interval tie (48000) is the largest legitimate join delta and must
	// still be accepted (safety limit is seedLength/2).
	const auto update = DriveJoin(96000u, 48000);
	ASSERT_TRUE(update.PhaseCorrection.has_value());
	EXPECT_TRUE(update.PhaseCorrection->IsJoin);
	EXPECT_EQ(48000, update.PhaseCorrection->DeltaSamps);
}

// ── Phase 5: tempo request state machine (§2.6/§3.5) ─────────────────────────

namespace
{
	// Cycles one full interval: a forward move into the final quarter followed by
	// a backward wrap into the first quarter. Returns the wrap observation update.
	NinjamTimingUpdate CycleWrap(NinjamTimingCoordinator& coordinator, Timer& clock,
		unsigned int length, unsigned int wrapPos, float bpm, unsigned int bpi)
	{
		coordinator.Observe(MakeTimingTempo(length, length - (length / 8u), bpm, bpi),
			std::nullopt, true, io::UserConfig{}, clock);
		return coordinator.Observe(MakeTimingTempo(length, wrapPos, bpm, bpi),
			std::nullopt, true, io::UserConfig{}, clock);
	}
}

TEST(NinjamTimingCoordinator, TempoRequestAcknowledgedByMatchingTimingWithoutGenerationChange)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, true, local);

	// Gen change auto-accepts (no prompt); request stays queued until a wrap.
	coordinator.Observe(MakeTiming(384000u, 300000u), local, true, io::UserConfig{}, clock);
	auto request = coordinator.Observe(MakeTiming(384000u, 1000u), local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(request.TempoRequest.has_value());
	coordinator.NotifyTempoRequestSent(true);
	EXPECT_EQ(ninjam::TempoRequestState::SentAwaitingOutcome, coordinator.RequestState());

	// The server applies the tempo but the rounded device interval is unchanged, so
	// no generation-change event occurs. A matching fresh observation must still
	// acknowledge the request (§2.6).
	auto ack = CycleWrap(coordinator, clock, 384000u, 2000u, 120.0f, 16u);
	EXPECT_FALSE(ack.TempoRequest.has_value());
	EXPECT_EQ(ninjam::TempoRequestState::Acknowledged, coordinator.RequestState());
	EXPECT_EQ(1u, coordinator.Diagnostics().TempoAcknowledged);
}

TEST(NinjamTimingCoordinator, FreshMatchingGenerationAcknowledgesAndAppliesRequestedTempo)
{
	Timer clock;
	clock.SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
	clock.SetSeedSourceLength(384000ul);
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, true, local);

	coordinator.Observe(MakeTimingTempo(256000u, 220000u, 90.0f, 8u), local, true,
		io::UserConfig{}, clock);
	auto request = coordinator.Observe(MakeTimingTempo(256000u, 1000u, 90.0f, 8u), local, true,
		io::UserConfig{}, clock);
	ASSERT_TRUE(request.TempoRequest.has_value());
	coordinator.NotifyTempoRequestSent(true);

	const auto confirmed = coordinator.Observe(MakeTimingTempo(384000u, 100u, 120.0f, 16u),
		local, true, io::UserConfig{}, clock);
	EXPECT_EQ(ninjam::TempoRequestState::Acknowledged, coordinator.RequestState());
	EXPECT_EQ(1u, coordinator.Diagnostics().TempoAcknowledged);
	ASSERT_TRUE(confirmed.ClockSettings.has_value());
	EXPECT_EQ(384000ul, confirmed.ClockSettings->SeedLengthSamps);
}

TEST(NinjamTimingCoordinator, NearLocalServerTempoAcknowledgesAfterSuccessfulSend)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, true, local);

	coordinator.Observe(MakeTimingTempo(480000u, 400000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	auto request = coordinator.Observe(MakeTimingTempo(480000u, 1000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(request.TempoRequest.has_value());
	coordinator.NotifyTempoRequestSent(true);

	const auto acknowledged = coordinator.Observe(MakeTimingTempo(384000u, 2000u, 120.75f, 16u),
		local, true, io::UserConfig{}, clock);
	EXPECT_EQ(ninjam::TempoRequestState::Acknowledged, coordinator.RequestState());
	EXPECT_TRUE(acknowledged.ClockSettings.has_value());
	EXPECT_FALSE(acknowledged.PromptForTempoChange);
}

TEST(NinjamTimingCoordinator, DistantServerTempoDoesNotAcknowledgeRequest)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, true, local);

	coordinator.Observe(MakeTimingTempo(480000u, 400000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	auto request = coordinator.Observe(MakeTimingTempo(480000u, 1000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(request.TempoRequest.has_value());
	coordinator.NotifyTempoRequestSent(true);

	const auto update = coordinator.Observe(MakeTimingTempo(384000u, 2000u, 121.01f, 16u),
		local, true, io::UserConfig{}, clock);
	EXPECT_EQ(ninjam::TempoRequestState::SentAwaitingOutcome, coordinator.RequestState());
	EXPECT_FALSE(update.ClockSettings.has_value());
	EXPECT_FALSE(update.PromptForTempoChange);
}

TEST(NinjamTimingCoordinator, MatchingObservationBeforeSendDoesNotAcknowledgeRequest)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, true, local);

	// This matching observation occurs before the first request is emitted.
	coordinator.Observe(MakeTimingTempo(384000u, 300000u, 120.0f, 16u), local, true,
		io::UserConfig{}, clock);
	auto request = coordinator.Observe(MakeTimingTempo(384000u, 1000u, 120.0f, 16u), local, true,
		io::UserConfig{}, clock);
	ASSERT_TRUE(request.TempoRequest.has_value());
	coordinator.NotifyTempoRequestSent(true);
	EXPECT_EQ(ninjam::TempoRequestState::SentAwaitingOutcome, coordinator.RequestState());
	EXPECT_EQ(0u, coordinator.Diagnostics().TempoAcknowledged);

	// A later matching observation is the first legitimate acknowledgement.
	CycleWrap(coordinator, clock, 384000u, 2000u, 120.0f, 16u);
	EXPECT_EQ(ninjam::TempoRequestState::Acknowledged, coordinator.RequestState());
	EXPECT_EQ(1u, coordinator.Diagnostics().TempoAcknowledged);
}

TEST(NinjamTimingCoordinator, TempoRequestSendFailureRequeuesForRetry)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, true, local);

	const auto oldServer = coordinator.Observe(MakeTimingTempo(384000u, 300000u, 90.0f, 8u),
		local, true, io::UserConfig{}, clock);
	EXPECT_FALSE(oldServer.PromptForTempoChange);
	EXPECT_FALSE(coordinator.PendingTempoChange().has_value());
	auto request = coordinator.Observe(MakeTiming(384000u, 1000u), local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(request.TempoRequest.has_value());

	// A failed network send returns the request to the queue.
	coordinator.NotifyTempoRequestSent(false);
	EXPECT_EQ(ninjam::TempoRequestState::Queued, coordinator.RequestState());

	// The next interval boundary re-sends it (server has not applied the tempo, so
	// the observed tempo does not match yet).
	auto resend = CycleWrap(coordinator, clock, 384000u, 2000u, 90.0f, 8u);
	EXPECT_TRUE(resend.TempoRequest.has_value());
	EXPECT_EQ(ninjam::TempoRequestState::SentAwaitingOutcome, coordinator.RequestState());
}

TEST(NinjamTimingCoordinator, FailedSendCannotBeAcknowledgedByInterveningObservation)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, true, local);
	coordinator.Observe(MakeTimingTempo(384000u, 300000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	auto request = coordinator.Observe(MakeTimingTempo(384000u, 1000u, 90.0f, 8u), local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(request.TempoRequest.has_value());
	coordinator.NotifyTempoRequestSent(false);
	CycleWrap(coordinator, clock, 384000u, 2000u, 120.0f, 16u);
	EXPECT_EQ(ninjam::TempoRequestState::SentAwaitingOutcome, coordinator.RequestState());
	EXPECT_EQ(0u, coordinator.Diagnostics().TempoAcknowledged);
}

TEST(NinjamTimingCoordinator, TempoRequestExpiresAfterConfiguredRetries)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	ninjam::NinjamTempoJoinOptions options;
	options.PromptBeforeApplyingRemoteTempo = true;
	options.PushLocalTempoOnJoin = true;
	options.MaxTempoRequestRetries = 2u;
	NinjamTimingCoordinator coordinator;
	coordinator.Connect(options, local);

	coordinator.Observe(MakeTiming(384000u, 300000u), local, true, io::UserConfig{}, clock);
	auto request = coordinator.Observe(MakeTiming(384000u, 1000u), local, true, io::UserConfig{}, clock);
	ASSERT_TRUE(request.TempoRequest.has_value());

	// Never match the requested tempo (server never applies it). Cycle wraps until
	// retries are exhausted and the request expires.
	bool sawPrompt = false;
	for (unsigned int i = 0u; i < 6u; ++i)
		sawPrompt = CycleWrap(coordinator, clock, 384000u, 2000u + i, 90.0f, 8u).PromptForTempoChange
			|| sawPrompt;

	EXPECT_EQ(ninjam::TempoRequestState::Expired, coordinator.RequestState());
	EXPECT_TRUE(sawPrompt);
	EXPECT_TRUE(coordinator.PendingTempoChange().has_value());
}

TEST(NinjamTimingCoordinator, TempoRequestDeadlinePromptsOnceWithLatestServerTiming)
{
	Timer clock;
	engine::QuantisationTiming local{ 24000u, 384000u, 16u, 120.0f, 16u };
	ninjam::NinjamTempoJoinOptions options;
	options.PushLocalTempoOnJoin = true;
	options.PromptBeforeApplyingRemoteTempo = true;
	options.TempoRequestDeadline = std::chrono::seconds(1);
	NinjamTimingCoordinator coordinator;
	coordinator.Connect(options, local);
	const auto start = std::chrono::steady_clock::time_point{};

	coordinator.Observe(MakeTimingTempo(480000u, 400000u, 90.0f, 8u), local, true,
		io::UserConfig{}, clock, start);
	auto request = coordinator.Observe(MakeTimingTempo(480000u, 1000u, 90.0f, 8u), local, true,
		io::UserConfig{}, clock, start);
	ASSERT_TRUE(request.TempoRequest.has_value());
	coordinator.NotifyTempoRequestSent(true, start);

	const auto early = coordinator.Observe(MakeTimingTempo(480000u, 2000u, 90.0f, 8u), local, true,
		io::UserConfig{}, clock, start + std::chrono::milliseconds(999));
	EXPECT_FALSE(early.PromptForTempoChange);
	const auto expired = coordinator.Observe(MakeTimingTempo(480000u, 3000u, 90.0f, 8u), local, true,
		io::UserConfig{}, clock, start + std::chrono::seconds(1));
	EXPECT_EQ(ninjam::TempoRequestState::Expired, coordinator.RequestState());
	EXPECT_TRUE(expired.PromptForTempoChange);
	ASSERT_TRUE(coordinator.PendingTempoChange().has_value());
	EXPECT_EQ(480000u, coordinator.PendingTempoChange()->IntervalLengthSamps);
	EXPECT_FALSE(coordinator.Observe(MakeTimingTempo(480000u, 4000u, 90.0f, 8u), local, true,
		io::UserConfig{}, clock, start + std::chrono::seconds(2)).PromptForTempoChange);
}
