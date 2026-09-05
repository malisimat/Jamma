#include "gtest/gtest.h"
#include "ninjam/NinjamNetworkService.h"
#include "ninjam/NinjamTimingCoordinator.h"
#include "ninjam/NinjamSession.h"
#include "io/UserConfig.h"

#include <limits>

using ninjam::NinjamTiming;
using ninjam::NinjamTimingCoordinator;
using ninjam::NinjamTimingUpdate;
using utils::Timer;

namespace
{
	NinjamTiming MakeTiming(unsigned int length, unsigned int position,
		unsigned int localPhase = 0u, unsigned int localLength = 0u)
	{
		NinjamTiming timing;
		timing.IsConnected = true;
		timing.IsValid = true;
		timing.DeviceSampleRate = 48000u;
		timing.SourceSampleRate = 44100u;
		timing.IntervalLengthSamps = length;
		timing.IntervalPositionSamps = position;
		timing.Bpm = 120.0f;
		timing.Bpi = 16u;
		timing.HasAudioBlockStartSample = true;
		timing.AudioBlockStartSample = 0u;
		timing.HasLocalTransport = true;
		timing.LocalTransport.MasterLengthSamps = localLength == 0u ? length : localLength;
		timing.LocalTransport.MasterPhaseSamps = localPhase;
		timing.LocalTransport.AbsoluteSamplePos = position;
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

	auto timing = MakeTiming(1000u, 300u);
	const auto update = coordinator.Observe(timing, std::nullopt, true, io::UserConfig{}, clock);

	ASSERT_TRUE(update.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::NoSync, update.DesiredTransport->Intent);
	EXPECT_EQ(1u, coordinator.Diagnostics().GenerationChanges);
}

TEST(NinjamTimingCoordinator, AcceptedWrapProducesOneGenerationTaggedCorrection)
{
	Timer clock;
	clock.SetQuantisation(100u, Timer::QUANTISE_MULTIPLE);
	clock.SetSeedSourceLength(1000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, false);

	auto timing = MakeTiming(1000u, 900u);
	coordinator.Observe(timing, std::nullopt, false, io::UserConfig{}, clock);
	timing.IntervalPositionSamps = 10u;
	const auto update = coordinator.Observe(timing, std::nullopt, false, io::UserConfig{}, clock);

	ASSERT_TRUE(update.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::JoinAlignment, update.DesiredTransport->Intent);
	EXPECT_NE(0u, update.DesiredTransport->Generation);
	EXPECT_EQ(1u, coordinator.Diagnostics().PhaseEventsQueued);
}

TEST(NinjamTimingCoordinator, AutoAcceptsRemoteTempoWithoutLocalContent)
{
	Timer clock;
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	const auto update = coordinator.Observe(MakeTiming(384000u, 100u), std::nullopt, false, io::UserConfig{}, clock);
	ASSERT_TRUE(update.DesiredTransport.has_value());
	EXPECT_FALSE(update.PromptForTempoChange);
	EXPECT_EQ(384000ul, update.DesiredTransport->IntervalLengthSamps);
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
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::NoSync,
		coordinator.ResolveTempoChange(false, std::nullopt, clock).DesiredTransport->Intent);
	update = coordinator.Observe(MakeTiming(480000u, 200u), std::nullopt, true, io::UserConfig{}, clock);
	EXPECT_FALSE(update.PromptForTempoChange);
	update = coordinator.Observe(MakeTiming(576000u, 100u), std::nullopt, true, io::UserConfig{}, clock);
	EXPECT_TRUE(update.PromptForTempoChange);
	auto accepted = coordinator.ResolveTempoChange(true, std::nullopt, clock);
	EXPECT_TRUE(accepted.DesiredTransport.has_value());
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
	ASSERT_TRUE(accepted.DesiredTransport.has_value());
	EXPECT_EQ(200u, accepted.DesiredTransport->RemotePhaseSamps);
	EXPECT_EQ(5000u, accepted.DesiredTransport->ObservationSample);
}

TEST(NinjamTimingCoordinator, RejectedDifferentTempoDoesNotEmitWrapCorrection)
{
	Timer clock;
	clock.SetSeedSourceLength(384000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	coordinator.Observe(MakeTiming(480000u, 400000u, 0u, 384000u),
		std::nullopt, true, io::UserConfig{}, clock);
	const auto rejected = coordinator.ResolveTempoChange(false, std::nullopt, clock);
	ASSERT_TRUE(rejected.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::NoSync, rejected.DesiredTransport->Intent);
	const auto update = coordinator.Observe(MakeTiming(480000u, 1000u, 0u, 384000u), std::nullopt, true,
		io::UserConfig{}, clock);
	EXPECT_FALSE(update.DesiredTransport.has_value());
}

TEST(NinjamTimingDiagnostics, DisabledIsZeroWorkAndEnabledIsBounded)
{
	ninjam::NinjamTimingDiagnostics diagnostics;
	diagnostics.SetCaptureEnabled(false);
	diagnostics.Capture(ninjam::NinjamTimingDiagnosticReason::DesiredApplyLag,
		1u, 3u, 1u, 7u, 2, 1u);

	EXPECT_EQ(0u, diagnostics.CapturedEventCount);
	EXPECT_EQ(0u, diagnostics.EventOverflowCount);
	EXPECT_EQ(0u, diagnostics.EventSequence);

	diagnostics.SetCaptureEnabled(true);
	const auto eventTotal = ninjam::NinjamTimingDiagnostics::EventCapacity + 3u;
	for (auto eventIndex = 0u; eventIndex < eventTotal; ++eventIndex)
	{
		const auto epoch = eventIndex < 2u ? 1u : 2u;
		const auto reason = (eventIndex & 1u) == 0u
			? ninjam::NinjamTimingDiagnosticReason::DesiredApplyLag
			: ninjam::NinjamTimingDiagnosticReason::DesiredApplyCaughtUp;
		diagnostics.Capture(reason, epoch, eventIndex + 1u, eventIndex,
			eventIndex + 10u, static_cast<long long>(eventIndex + 1u), eventIndex);
	}

	EXPECT_EQ(ninjam::NinjamTimingDiagnostics::EventCapacity,
		diagnostics.CapturedEventCount);
	EXPECT_EQ(3u, diagnostics.EventOverflowCount);
	EXPECT_EQ(eventTotal, diagnostics.EventSequence);
	EXPECT_EQ(eventTotal, diagnostics.LatestEvent.Sequence);
	EXPECT_EQ(2u, diagnostics.LatestEvent.SessionEpoch);
	EXPECT_EQ(eventTotal, diagnostics.LatestEvent.DesiredVersion);
	EXPECT_EQ(eventTotal - 1u, diagnostics.LatestEvent.AppliedVersion);
	ASSERT_EQ(ninjam::NinjamTimingDiagnostics::EventCapacity,
		diagnostics.Events.size());
	EXPECT_EQ(1u, diagnostics.Events.front().Sequence);
	EXPECT_EQ(ninjam::NinjamTimingDiagnostics::EventCapacity,
		diagnostics.Events.back().Sequence);
}

TEST(NinjamTimingDiagnostics, EveryCoordinatorRejectionHasOneBoundedReason)
{
	struct RejectionCase
	{
		ninjam::NinjamTimingDiagnosticReason Reason;
		NinjamTiming Timing;
	};

	auto disconnected = MakeTiming(1000u, 100u);
	disconnected.IsConnected = false;
	auto zeroInterval = MakeTiming(0u, 0u);
	auto invalid = MakeTiming(1000u, 100u);
	invalid.IsValid = false;
	auto invalidRate = MakeTiming(1000u, 100u);
	invalidRate.IsValid = false;
	invalidRate.SourceSampleRate = 0u;
	auto invalidTempo = MakeTiming(1000u, 100u);
	invalidTempo.IsValid = false;
	invalidTempo.Bpm = (std::numeric_limits<float>::quiet_NaN)();
	auto invalidBpi = MakeTiming(1000u, 100u);
	invalidBpi.IsValid = false;
	invalidBpi.Bpi = 0u;
	auto missingAudioBoundary = MakeTiming(1000u, 100u);
	missingAudioBoundary.HasAudioBlockStartSample = false;
	auto missingLocalTransport = MakeTiming(1000u, 100u);
	missingLocalTransport.HasLocalTransport = false;
	auto invalidGrain = MakeTiming(1u, 0u);
	invalidGrain.Bpi = 32u;

	const RejectionCase cases[] = {
		{ ninjam::NinjamTimingDiagnosticReason::ObservationDisconnected, disconnected },
		{ ninjam::NinjamTimingDiagnosticReason::ObservationZeroInterval, zeroInterval },
		{ ninjam::NinjamTimingDiagnosticReason::ObservationInvalid, invalid },
		{ ninjam::NinjamTimingDiagnosticReason::ObservationInvalidSampleRate, invalidRate },
		{ ninjam::NinjamTimingDiagnosticReason::ObservationInvalidTempo, invalidTempo },
		{ ninjam::NinjamTimingDiagnosticReason::ObservationInvalidBpi, invalidBpi },
		{ ninjam::NinjamTimingDiagnosticReason::ObservationMissingAudioBoundary, missingAudioBoundary },
		{ ninjam::NinjamTimingDiagnosticReason::ObservationMissingLocalTransport, missingLocalTransport },
		{ ninjam::NinjamTimingDiagnosticReason::ObservationInvalidGrain, invalidGrain },
	};

	for (const auto& rejectionCase : cases)
	{
		Timer clock;
		NinjamTimingCoordinator coordinator;
		Connect(coordinator, false, false);
		coordinator.SetDiagnosticsCaptureEnabled(true);
		const auto update = coordinator.Observe(rejectionCase.Timing, std::nullopt,
			false, io::UserConfig{}, clock);
		EXPECT_FALSE(update.DesiredTransport.has_value());
		const auto diagnostics = coordinator.Diagnostics();
		EXPECT_EQ(1u, diagnostics.Count(rejectionCase.Reason));
		EXPECT_EQ(rejectionCase.Reason, diagnostics.LatestEvent.Reason);
	}

	Timer trackerClock;
	trackerClock.SetSeedSourceLength(1000ul);
	NinjamTimingCoordinator trackerCoordinator;
	Connect(trackerCoordinator, false, false);
	trackerCoordinator.SetDiagnosticsCaptureEnabled(true);
	trackerCoordinator.Observe(MakeTiming(1000u, 800u), std::nullopt,
		false, io::UserConfig{}, trackerClock);
	trackerCoordinator.Observe(MakeTiming(1000u, 700u), std::nullopt,
		false, io::UserConfig{}, trackerClock);
	EXPECT_EQ(1u, trackerCoordinator.Diagnostics().Count(
		ninjam::NinjamTimingDiagnosticReason::TrackerImplausibleBackward));

	Timer safetyClock;
	safetyClock.SetSeedSourceLength(10000ul);
	NinjamTimingCoordinator safetyCoordinator;
	Connect(safetyCoordinator, false, false);
	safetyCoordinator.SetDiagnosticsCaptureEnabled(true);
	safetyCoordinator.Observe(MakeTiming(10000u, 9000u), std::nullopt,
		false, io::UserConfig{}, safetyClock);
	safetyCoordinator.Observe(MakeTiming(10000u, 10u), std::nullopt,
		false, io::UserConfig{}, safetyClock);
	safetyCoordinator.Observe(MakeTiming(10000u, 9000u), std::nullopt,
		false, io::UserConfig{}, safetyClock);
	const auto safetyRejected = safetyCoordinator.Observe(
		MakeTiming(10000u, 1000u, 5000u), std::nullopt,
		false, io::UserConfig{}, safetyClock);
	EXPECT_FALSE(safetyRejected.DesiredTransport.has_value());
	const auto safetyDiagnostics = safetyCoordinator.Diagnostics();
	EXPECT_EQ(1u, safetyDiagnostics.Count(
		ninjam::NinjamTimingDiagnosticReason::SafetyLimitExceeded));
	EXPECT_EQ(-4000, safetyDiagnostics.LatestEvent.ValueSamps);
	EXPECT_EQ(1024u, safetyDiagnostics.LatestEvent.LimitSamps);
}

TEST(NinjamTimingDiagnostics, DesiredAppliedCorrelationIsDeduplicatedAcrossTwoSessions)
{
	Timer clock;
	NinjamTimingCoordinator coordinator;
	coordinator.SetDiagnosticsCaptureEnabled(true);
	ninjam::NinjamTempoJoinOptions options;
	options.PushLocalTempoOnJoin = false;
	options.PromptBeforeApplyingRemoteTempo = false;

	ninjam::NinjamSessionTimingStatus available;
	available.IsAvailable = true;
	available.Changed = true;
	available.SessionEpoch = 1u;
	const auto firstSession = coordinator.ObserveSessionStatus(available, options, std::nullopt);
	ASSERT_TRUE(firstSession.DesiredTransport.has_value());
	const auto firstDesired = firstSession.DesiredTransport.value();
	coordinator.ObserveAppliedTimingReceipt(std::nullopt);
	coordinator.ObserveAppliedTimingReceipt(std::nullopt);
	ninjam::NinjamDesiredTimingReceipt firstReceipt;
	firstReceipt.SessionEpoch = firstDesired.SessionEpoch;
	firstReceipt.Version = firstDesired.Version;
	firstReceipt.Generation = firstDesired.Generation;
	coordinator.ObserveAppliedTimingReceipt(firstReceipt);
	coordinator.ObserveAppliedTimingReceipt(firstReceipt);

	available.SessionEpoch = 2u;
	const auto secondSession = coordinator.ObserveSessionStatus(available, options, std::nullopt);
	ASSERT_TRUE(secondSession.DesiredTransport.has_value());
	const auto secondDesired = secondSession.DesiredTransport.value();
	coordinator.ObserveAppliedTimingReceipt(firstReceipt);
	ninjam::NinjamDesiredTimingReceipt secondReceipt;
	secondReceipt.SessionEpoch = secondDesired.SessionEpoch;
	secondReceipt.Version = secondDesired.Version;
	secondReceipt.Generation = secondDesired.Generation;
	const auto diagnostics = coordinator.ObserveAppliedTimingReceipt(secondReceipt);

	EXPECT_EQ(2u, diagnostics.Count(ninjam::NinjamTimingDiagnosticReason::DesiredApplyLag));
	EXPECT_EQ(2u, diagnostics.Count(ninjam::NinjamTimingDiagnosticReason::DesiredApplyCaughtUp));
	bool foundEpochOneLag = false;
	bool foundEpochTwoLag = false;
	bool foundStaleEpochOneReceiptAgainstEpochTwo = false;
	for (auto eventIndex = 0u; eventIndex < diagnostics.CapturedEventCount; ++eventIndex)
	{
		const auto& event = diagnostics.Events[eventIndex];
		if (event.Reason != ninjam::NinjamTimingDiagnosticReason::DesiredApplyLag)
			continue;
		foundEpochOneLag = foundEpochOneLag || event.SessionEpoch == 1u;
		foundEpochTwoLag = foundEpochTwoLag || event.SessionEpoch == 2u;
		foundStaleEpochOneReceiptAgainstEpochTwo = foundStaleEpochOneReceiptAgainstEpochTwo
			|| (event.SessionEpoch == 2u && event.AppliedSessionEpoch == 1u);
	}
	EXPECT_TRUE(foundEpochOneLag);
	EXPECT_TRUE(foundEpochTwoLag);
	EXPECT_TRUE(foundStaleEpochOneReceiptAgainstEpochTwo);
	EXPECT_EQ(2u, diagnostics.LatestEvent.SessionEpoch);
	EXPECT_EQ(2u, diagnostics.LatestEvent.AppliedSessionEpoch);
	EXPECT_EQ(secondDesired.Version, diagnostics.LatestEvent.DesiredVersion);
	EXPECT_EQ(secondDesired.Version, diagnostics.LatestEvent.AppliedVersion);
}

TEST(NinjamTimingDiagnostics, CaptureConfigurationDoesNotChangeDesiredPhase)
{
	const auto run = [](bool captureEnabled)
	{
		Timer clock;
		clock.SetSeedSourceLength(1000ul);
		NinjamTimingCoordinator coordinator;
		Connect(coordinator, false, false);
		coordinator.SetDiagnosticsCaptureEnabled(captureEnabled);
		auto timing = MakeTiming(1000u, 275u, 125u);
		timing.AudioBlockStartSample = 4000u;
		return coordinator.Observe(timing, std::nullopt, false,
			io::UserConfig{}, clock).DesiredTransport;
	};

	const auto disabled = run(false);
	const auto enabled = run(true);
	ASSERT_TRUE(disabled.has_value());
	ASSERT_TRUE(enabled.has_value());
	EXPECT_EQ(disabled->Version, enabled->Version);
	EXPECT_EQ(disabled->SessionEpoch, enabled->SessionEpoch);
	EXPECT_EQ(disabled->Generation, enabled->Generation);
	EXPECT_EQ(disabled->Intent, enabled->Intent);
	EXPECT_EQ(disabled->LocalFollowPolicy, enabled->LocalFollowPolicy);
	EXPECT_EQ(disabled->IntervalLengthSamps, enabled->IntervalLengthSamps);
	EXPECT_EQ(disabled->RemotePhaseSamps, enabled->RemotePhaseSamps);
	EXPECT_EQ(disabled->ObservationSample, enabled->ObservationSample);
}

TEST(NinjamTimingDiagnostics, NetworkServiceSerializesConfigurationAndAppliedReceiptForwarding)
{
	ninjam::NinjamNetworkService service;
	service.SetTimingDiagnosticsEnabled(true);
	const auto connected = service.PrepareTempoSyncOnConnect(std::nullopt);
	ASSERT_TRUE(connected.DesiredTransport.has_value());

	auto diagnostics = service.ObserveAppliedTimingReceipt(std::nullopt);
	EXPECT_EQ(1u, diagnostics.Count(ninjam::NinjamTimingDiagnosticReason::DesiredApplyLag));

	ninjam::NinjamDesiredTimingReceipt receipt;
	receipt.SessionEpoch = connected.DesiredTransport->SessionEpoch;
	receipt.Version = connected.DesiredTransport->Version;
	receipt.Generation = connected.DesiredTransport->Generation;
	diagnostics = service.ObserveAppliedTimingReceipt(receipt);
	EXPECT_EQ(1u, diagnostics.Count(ninjam::NinjamTimingDiagnosticReason::DesiredApplyCaughtUp));

	service.SetTimingDiagnosticsEnabled(false);
	const auto sequenceBeforeDisabledForward = diagnostics.EventSequence;
	diagnostics = service.ObserveAppliedTimingReceipt(std::nullopt);
	EXPECT_EQ(sequenceBeforeDisabledForward, diagnostics.EventSequence);
}

TEST(NinjamTimingCoordinator, AcceptedRemoteGridRetainsAuthoritativeBpi)
{
	Timer clock;
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, false);
	const auto update = coordinator.Observe(MakeTimingTempo(78985u, 123u, 120.0f, 4u),
		std::nullopt, false, io::UserConfig{}, clock);
	ASSERT_TRUE(update.DesiredTransport.has_value());
	EXPECT_EQ(78985ul, update.DesiredTransport->IntervalLengthSamps);
	EXPECT_EQ(4u, update.DesiredTransport->BeatsPerInterval);
	EXPECT_EQ(19746u, update.DesiredTransport->GrainSamps);
	EXPECT_EQ(123u, update.DesiredTransport->RemotePhaseSamps);
}

TEST(NinjamTimingCoordinator, MissingBpiCannotCreateOrMutateRemoteAuthority)
{
	Timer clock;
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	auto missingBpi = MakeTimingTempo(480000u, 100u, 90.0f, 0u);
	missingBpi.AudioBlockStartSample = 1000u;

	const auto rejected = coordinator.Observe(missingBpi, std::nullopt, true,
		io::UserConfig{}, clock);
	EXPECT_FALSE(rejected.DesiredTransport.has_value());
	EXPECT_FALSE(rejected.RemoteGrid.has_value());
	EXPECT_FALSE(rejected.TempoRequest.has_value());
	EXPECT_FALSE(rejected.PromptForTempoChange);
	EXPECT_FALSE(coordinator.PendingTempoChange().has_value());
	EXPECT_EQ(0u, coordinator.Diagnostics().CommandsEmitted);
	EXPECT_EQ(0u, coordinator.Diagnostics().TempoProposals);
	EXPECT_EQ(0u, coordinator.Diagnostics().ObservationsAccepted);

	auto complete = MakeTimingTempo(480000u, 200u, 90.0f, 8u);
	complete.AudioBlockStartSample = 2000u;
	const auto accepted = coordinator.Observe(complete, std::nullopt, true,
		io::UserConfig{}, clock);
	EXPECT_TRUE(accepted.PromptForTempoChange);
	ASSERT_TRUE(coordinator.PendingTempoChange().has_value());
	EXPECT_EQ(8u, coordinator.PendingTempoChange()->Bpi);
}

TEST(NinjamTimingCoordinator, ProposalIdentityRefreshesObservationButReplacesEveryMaterialField)
{
	struct IdentityCase
	{
		const char* Name;
		unsigned int IntervalLengthSamps;
		unsigned int DeviceSampleRate;
		float Bpm;
		unsigned int Bpi;
	};
	const IdentityCase cases[] = {
		{ "interval", 480008u, 48000u, 90.0f, 8u },
		{ "rate", 480000u, 44100u, 90.0f, 8u },
		{ "bpi-and-derived-grain", 480000u, 48000u, 90.0f, 6u },
		{ "bpm-outside-tolerance", 480000u, 48000u, 90.011f, 8u },
	};

	for (const auto& identityCase : cases)
	{
		SCOPED_TRACE(identityCase.Name);
		Timer clock;
		NinjamTimingCoordinator coordinator;
		Connect(coordinator, true, false);
		auto original = MakeTimingTempo(480000u, 100u, 90.0f, 8u);
		original.DeviceSampleRate = 48000u;
		original.AudioBlockStartSample = 1000u;
		ASSERT_TRUE(coordinator.Observe(original, std::nullopt, true,
			io::UserConfig{}, clock).PromptForTempoChange);

		auto changed = MakeTimingTempo(identityCase.IntervalLengthSamps, 200u,
			identityCase.Bpm, identityCase.Bpi);
		changed.DeviceSampleRate = identityCase.DeviceSampleRate;
		changed.AudioBlockStartSample = 2000u;
		const auto replacement = coordinator.Observe(changed, std::nullopt, true,
			io::UserConfig{}, clock);
		EXPECT_TRUE(replacement.PromptForTempoChange);
		ASSERT_TRUE(coordinator.PendingTempoChange().has_value());
		const auto pending = coordinator.PendingTempoChange().value();
		EXPECT_EQ(identityCase.IntervalLengthSamps, pending.IntervalLengthSamps);
		EXPECT_EQ(identityCase.DeviceSampleRate, pending.SourceSampleRate);
		EXPECT_FLOAT_EQ(identityCase.Bpm, pending.Bpm);
		EXPECT_EQ(identityCase.Bpi, pending.Bpi);
		EXPECT_EQ((static_cast<std::uint64_t>(identityCase.IntervalLengthSamps)
			+ identityCase.Bpi / 2u) / identityCase.Bpi, pending.GrainSamps);
		EXPECT_EQ(200u, pending.IntervalPositionSamps);
		EXPECT_EQ(2000u, pending.AudioBlockStartSample);
	}

	Timer clock;
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	auto original = MakeTimingTempo(480000u, 100u, 90.0f, 8u);
	original.DeviceSampleRate = 48000u;
	original.AudioBlockStartSample = 1000u;
	ASSERT_TRUE(coordinator.Observe(original, std::nullopt, true,
		io::UserConfig{}, clock).PromptForTempoChange);
	auto phaseOnly = MakeTimingTempo(480000u, 321u, 90.009f, 8u);
	phaseOnly.DeviceSampleRate = 48000u;
	phaseOnly.AudioBlockStartSample = 4321u;
	EXPECT_FALSE(coordinator.Observe(phaseOnly, std::nullopt, true,
		io::UserConfig{}, clock).PromptForTempoChange);
	ASSERT_TRUE(coordinator.PendingTempoChange().has_value());
	EXPECT_FLOAT_EQ(90.0f, coordinator.PendingTempoChange()->Bpm);
	EXPECT_EQ(321u, coordinator.PendingTempoChange()->IntervalPositionSamps);
	EXPECT_EQ(4321u, coordinator.PendingTempoChange()->AudioBlockStartSample);
}

TEST(NinjamTimingCoordinator, ChangedIdentityReplacesIgnoredProposalWithoutIntervalChange)
{
	Timer clock;
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, true, false);
	auto original = MakeTimingTempo(480000u, 100u, 90.0f, 8u);
	original.DeviceSampleRate = 48000u;
	ASSERT_TRUE(coordinator.Observe(original, std::nullopt, true,
		io::UserConfig{}, clock).PromptForTempoChange);
	ASSERT_TRUE(coordinator.ResolveTempoChange(false, std::nullopt, clock).DesiredTransport.has_value());

	auto phaseOnly = MakeTimingTempo(480000u, 200u, 90.009f, 8u);
	phaseOnly.DeviceSampleRate = 48000u;
	phaseOnly.AudioBlockStartSample = 2000u;
	EXPECT_FALSE(coordinator.Observe(phaseOnly, std::nullopt, true,
		io::UserConfig{}, clock).PromptForTempoChange);
	EXPECT_FALSE(coordinator.PendingTempoChange().has_value());

	auto changedBpi = MakeTimingTempo(480000u, 300u, 90.0f, 6u);
	changedBpi.DeviceSampleRate = 48000u;
	changedBpi.AudioBlockStartSample = 3000u;
	const auto replacement = coordinator.Observe(changedBpi, std::nullopt, true,
		io::UserConfig{}, clock);
	EXPECT_TRUE(replacement.PromptForTempoChange);
	ASSERT_TRUE(coordinator.PendingTempoChange().has_value());
	EXPECT_EQ(6u, coordinator.PendingTempoChange()->Bpi);
	EXPECT_EQ(80000u, coordinator.PendingTempoChange()->GrainSamps);
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
	const auto postDisconnect = coordinator.Observe(MakeTiming(480000u, 10u),
		std::nullopt, true, io::UserConfig{}, clock);
	EXPECT_TRUE(postDisconnect.DesiredTransport.has_value());
	EXPECT_NE(ninjam::NinjamDesiredTimingIntent::PhaseDiscipline,
		postDisconnect.DesiredTransport->Intent);
}

TEST(NinjamTimingCoordinator, LongRunningConvertedTimingSimulationStaysGenerationSafe)
{
	Timer clock;
	clock.SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
	clock.SetSeedSourceLength(384000ul);
	NinjamTimingCoordinator coordinator;
	Connect(coordinator, false, false);
	const unsigned int lengths[] = { 384000u, 768000u, 192000u, 123457u };
	unsigned long maxRemotePhase = 0ul;
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
				if (update.DesiredTransport.has_value())
					maxRemotePhase = std::max(maxRemotePhase,
						static_cast<unsigned long>(update.DesiredTransport->RemotePhaseSamps));
			}
			coordinator.Observe(MakeTiming(length, length - 10u), std::nullopt, false, io::UserConfig{}, clock);
			coordinator.Observe(MakeTiming(length, 10u), std::nullopt, false, io::UserConfig{}, clock);
		}
		if (generation == 1u)
		{
			coordinator.Disconnect();
			const auto freshDesired = coordinator.Observe(MakeTiming(length, 0u),
				std::nullopt, false, io::UserConfig{}, clock);
			EXPECT_TRUE(freshDesired.DesiredTransport.has_value());
			EXPECT_NE(ninjam::NinjamDesiredTimingIntent::PhaseDiscipline,
				freshDesired.DesiredTransport->Intent);
			Connect(coordinator, false, false);
		}
	}
	EXPECT_LT(maxRemotePhase, 768000ul);
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
		coordinator.Observe(MakeTiming(length, anchorPos, localOffset),
			std::nullopt, false, io::UserConfig{}, clock);
		// A backward move from the final quarter into the first quarter is the wrap
		// that releases the pending Join event.
		return coordinator.Observe(MakeTiming(length, 1000u, localOffset),
			std::nullopt, false, io::UserConfig{}, clock);
	}
}

TEST(NinjamTimingCoordinator, QuarterIntervalJoinIsAccepted)
{
	// 96000-sample interval at 48 kHz; a quarter-interval (24000) join is far above
	// the old two-buffer cap yet must be accepted through the join path.
	const auto update = DriveJoin(96000u, 24000);
	ASSERT_TRUE(update.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::JoinAlignment, update.DesiredTransport->Intent);
	EXPECT_EQ(1000u, update.DesiredTransport->RemotePhaseSamps);
}

TEST(NinjamTimingCoordinator, HalfIntervalJoinIsAccepted)
{
	// The half-interval tie (48000) is the largest legitimate join delta and must
	// still be accepted (safety limit is seedLength/2).
	const auto update = DriveJoin(96000u, 48000);
	ASSERT_TRUE(update.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::JoinAlignment, update.DesiredTransport->Intent);
	EXPECT_EQ(1000u, update.DesiredTransport->RemotePhaseSamps);
}

TEST(NinjamTimingCoordinator, DelayedInitialJoinUsesObservationLocalPhaseNotLiveTimer)
{
	const auto runScenario = [](unsigned int delayBlocks)
		{
			Timer clock;
			clock.SetSeedSourceLength(1000ul);
			clock.Tick(900u, 0u);
			NinjamTimingCoordinator coordinator;
			Connect(coordinator, false, false);

			coordinator.Observe(MakeTiming(1000u, 875u, 250u),
				std::nullopt, false, io::UserConfig{}, clock);
			for (unsigned int block = 0u; block < delayBlocks; ++block)
				clock.Tick(256u, 0u);
			return coordinator.Observe(MakeTiming(1000u, 100u, 250u),
				std::nullopt, false, io::UserConfig{}, clock);
		};

	const auto immediate = runScenario(0u);
	const auto delayed = runScenario(7u);
	ASSERT_TRUE(immediate.DesiredTransport.has_value());
	ASSERT_TRUE(delayed.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::JoinAlignment, immediate.DesiredTransport->Intent);
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::JoinAlignment, delayed.DesiredTransport->Intent);
	EXPECT_EQ(immediate.DesiredTransport->RemotePhaseSamps,
		delayed.DesiredTransport->RemotePhaseSamps);
	EXPECT_EQ(immediate.DesiredTransport->ObservationSample,
		delayed.DesiredTransport->ObservationSample);
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
	ASSERT_TRUE(confirmed.DesiredTransport.has_value());
	EXPECT_EQ(384000ul, confirmed.DesiredTransport->IntervalLengthSamps);
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
	EXPECT_TRUE(acknowledged.DesiredTransport.has_value());
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
	EXPECT_FALSE(update.DesiredTransport.has_value());
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

TEST(NinjamTimingCoordinator, NoObservationAndInvalidTimingRecoverIdempotently)
{
	Timer clock;
	clock.SetSeedSourceLength(384000ul);
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

	const auto noObservation = coordinator.Tick(local, true, clock,
		start + std::chrono::seconds(1));
	ASSERT_TRUE(noObservation.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::NoSync, noObservation.DesiredTransport->Intent);
	EXPECT_EQ(ninjam::NinjamNoSyncReason::ObservationDeadline, noObservation.NoSyncReason);
	EXPECT_TRUE(noObservation.PromptForTempoChange);
	EXPECT_EQ(ninjam::TempoRequestState::Expired, coordinator.RequestState());
	const auto repeatedDeadline = coordinator.Tick(local, true, clock,
		start + std::chrono::seconds(2));
	EXPECT_FALSE(repeatedDeadline.DesiredTransport.has_value());
	EXPECT_FALSE(repeatedDeadline.PromptForTempoChange);

	auto freshAfterDeadline = MakeTimingTempo(480000u, 2000u, 90.0f, 8u);
	freshAfterDeadline.AudioBlockStartSample = 256u;
	auto recovered = coordinator.Observe(freshAfterDeadline, local, true, io::UserConfig{}, clock,
		start + std::chrono::seconds(2));
	EXPECT_TRUE(recovered.DesiredTransport.has_value());
	EXPECT_TRUE(coordinator.IsConnected());

	auto invalid = MakeTimingTempo(480000u, 3000u, 90.0f, 8u);
	invalid.IsValid = false;
	const auto invalidLoss = coordinator.Observe(invalid, local, true, io::UserConfig{}, clock,
		start + std::chrono::seconds(2));
	ASSERT_TRUE(invalidLoss.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamDesiredTimingIntent::NoSync, invalidLoss.DesiredTransport->Intent);
	EXPECT_EQ(ninjam::NinjamNoSyncReason::InvalidTiming, invalidLoss.NoSyncReason);
	const auto repeatedInvalid = coordinator.Observe(invalid, local, true, io::UserConfig{}, clock,
		start + std::chrono::seconds(2));
	EXPECT_FALSE(repeatedInvalid.DesiredTransport.has_value());
	EXPECT_EQ(ninjam::NinjamNoSyncReason::None, repeatedInvalid.NoSyncReason);

	recovered = coordinator.Observe(MakeTimingTempo(480000u, 4000u, 90.0f, 8u), local, true,
		io::UserConfig{}, clock, start + std::chrono::seconds(2));
	EXPECT_TRUE(recovered.DesiredTransport.has_value());
	EXPECT_TRUE(coordinator.IsConnected());
}
