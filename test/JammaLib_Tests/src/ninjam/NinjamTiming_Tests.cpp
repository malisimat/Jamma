#include "gtest/gtest.h"
#include <cmath>
#include <limits>
#include "./ninjam/NinjamConnection.h"
#include "./ninjam/NinjamTiming.h"
#include "./ninjam/NinjamLoopAlignment.h"
#include "utils/MusicalTransport.h"
#include "utils/Timer.h"

TEST(MusicalTransport, LocalTimerGeometryProducesImmediatePpq)
{
	utils::Timer timer;
	timer.SetQuantisation(24000u, utils::Timer::QUANTISE_MULTIPLE);
	timer.SetSeedSourceLength(192000ul);
	timer.Tick(624000u, 0u);
	const auto position = timer.LocalMusicalPosition(48000u);
	ASSERT_TRUE(position.IsValid);
	EXPECT_DOUBLE_EQ(26.0, position.Ppq);
	EXPECT_EQ(8, position.BeatsPerInterval);
	EXPECT_DOUBLE_EQ(120.0, position.Tempo);
}

TEST(MusicalTransport, LocalPpqRejectsInvalidGeometry)
{
	utils::Timer timer;
	timer.SetQuantisation(128u, utils::Timer::QUANTISE_MULTIPLE);
	timer.SetSeedSourceLength(1000ul);
	EXPECT_FALSE(timer.LocalMusicalPosition(48000u).IsValid);
	EXPECT_FALSE(timer.LocalMusicalPosition(0u).IsValid);
}

TEST(MusicalTransport, ExternalJoinKeepsLocalPpqUntilTheNextExternalWrap)
{
	utils::MusicalTransport transport;
	const utils::MusicalPosition local{ true, false, 26.0, 120.0, 8 };
	transport.QueueExternal(1000u, 400u, 1200u, 8u, 120.0, local, 150u);
	EXPECT_DOUBLE_EQ(26.0, transport.PositionAt(1000u, local).Ppq);
	transport.Advance(1800u);
	const auto atWrap = transport.PositionAt(1800u, local);
	ASSERT_TRUE(atWrap.IsValid);
	EXPECT_TRUE(atWrap.PositionChanged);
	EXPECT_DOUBLE_EQ(32.0, atWrap.Ppq);
	EXPECT_DOUBLE_EQ(120.0, atWrap.Tempo);
	EXPECT_EQ(8, atWrap.BeatsPerInterval);
	transport.Advance(1801u);
	EXPECT_FALSE(transport.PositionAt(1801u, local).PositionChanged);
}

TEST(MusicalTransport, ExternalProgressionIsContinuousAfterTheForwardLocate)
{
	utils::MusicalTransport transport;
	const utils::MusicalPosition local{ true, false, 0.0, 120.0, 8 };
	transport.QueueExternal(0u, 0u, 1200u, 8u, 120.0, local, 150u);
	transport.Advance(0u);
	EXPECT_DOUBLE_EQ(0.0, transport.PositionAt(0u, local).Ppq);
	transport.Advance(300u);
	EXPECT_DOUBLE_EQ(2.0, transport.PositionAt(300u, local).Ppq);
}

TEST(MusicalTransport, ReconnectUsesTheCurrentLocalEpochAndNotExternalZero)
{
	utils::MusicalTransport transport;
	const utils::MusicalPosition local{ true, false, 32.0, 120.0, 8 };
	transport.Reset();
	transport.QueueExternal(5000u, 400u, 1000u, 4u, 96.0, local, 150u);
	transport.Advance(5600u);
	const auto atWrap = transport.PositionAt(5600u, local);
	ASSERT_TRUE(atWrap.IsValid);
	EXPECT_GE(atWrap.Ppq, local.Ppq);
}

TEST(MusicalTransport, PhaseDisciplineUsesAnotherForwardOnlyExternalWrapAlignment)
{
	utils::MusicalTransport transport;
	const utils::MusicalPosition local{ true, false, 0.0, 120.0, 8 };
	transport.QueueExternal(0u, 0u, 1200u, 8u, 120.0, local, 150u);
	transport.Advance(0u);
	const auto before = transport.PositionAt(600u, local);
	transport.QueueExternal(600u, 400u, 1200u, 8u, 120.0, before, 150u);
	transport.Advance(1400u);
	const auto after = transport.PositionAt(1400u, local);
	EXPECT_GE(after.Ppq, before.Ppq);
	EXPECT_DOUBLE_EQ(0.0, std::fmod(after.Ppq, 8.0));
}

TEST(NinjamLoopAlignment, SourcePhaseReachesZeroAtNextRemoteWrapAcrossUnequalRulers)
{
	constexpr std::uint64_t localMaster = 1000u;
	constexpr std::uint64_t remoteMaster = 1200u;
	constexpr std::uint64_t remotePhase = 250u;
	const auto sourcePhase = ninjam::SourcePhaseAtRemotePhase(remotePhase, localMaster, remoteMaster);
	const auto remainingRemote = remoteMaster - remotePhase;
	const auto elapsed = ninjam::MapRemoteElapsedToLocal(remainingRemote, localMaster, remoteMaster);

	EXPECT_EQ(208u, sourcePhase);
	EXPECT_EQ(0u, (sourcePhase + elapsed) % localMaster);
}

TEST(NinjamLoopAlignment, MapsWholeRemoteIntervalsExactlyToWholeLocalIntervals)
{
	EXPECT_EQ(3000u, ninjam::MapRemoteElapsedToLocal(3600u, 1000u, 1200u));
	EXPECT_EQ(0u, ninjam::MapRemoteElapsedToLocal(1u, 0u, 1200u));
	EXPECT_EQ(0u, ninjam::MapRemoteElapsedToLocal(1u, 1000u, 0u));
}

TEST(NinjamLoopAlignment, RebaseUsesTheActiveMapOriginWhenRoundingUnequalRulers)
{
	constexpr std::uint64_t localMaster = 1000u;
	constexpr std::uint64_t remoteMaster = 1200u;
	constexpr std::uint64_t initialRemotePhase = 250u;
	constexpr std::uint64_t elapsedRemote = 64u;
	const auto sourceOrigin = ninjam::SourcePhaseAtRemotePhase(initialRemotePhase, localMaster, remoteMaster);
	const auto activeSourcePhase = (sourceOrigin + ninjam::MapRemoteElapsedToLocal(elapsedRemote,
		localMaster, remoteMaster)) % localMaster;
	const auto observedRemotePhase = initialRemotePhase + elapsedRemote;
	const auto targetSourcePhase = ninjam::SourcePhaseAtRemotePhase(observedRemotePhase,
		localMaster, remoteMaster);

	EXPECT_EQ(261u, activeSourcePhase);
	EXPECT_EQ(262u, targetSourcePhase);
	EXPECT_EQ(0u, (targetSourcePhase + ninjam::MapRemoteElapsedToLocal(
		remoteMaster - observedRemotePhase, localMaster, remoteMaster)) % localMaster);
}

TEST(NinjamTiming, ScalesSourceSamplesToDeviceRate)
{
	EXPECT_EQ(384000u, ninjam::ScaleSampleRate(352800u, 44100u, 48000u));
	EXPECT_EQ(2u, ninjam::ScaleSampleRate(1u, 22050u, 48000u));
}

TEST(NinjamTiming, PresenceWidthAndDownsampleTailRemainDistinct)
{
	constexpr auto oldMasterLengthSamps = 1000ul;
	constexpr auto oldMasterPhaseSamps = 100u;
	constexpr auto remoteIntervalLengthSamps = 1000u;
	constexpr auto observedRemotePhaseSamps = 700u;
	constexpr auto projectionDistanceSamps = 250u;

	// The pre-fix API has no explicit presence bit. These translated pairs prove
	// that sample zero is data, not the absence representation that B004 must add.
	const auto presentAtZero = ninjam::ResolveBoundaryTimingReplacement(
		oldMasterLengthSamps, oldMasterPhaseSamps, remoteIntervalLengthSamps,
		observedRemotePhaseSamps, 0u, projectionDistanceSamps);
	const auto presentAtNonzero = ninjam::ResolveBoundaryTimingReplacement(
		oldMasterLengthSamps, oldMasterPhaseSamps, remoteIntervalLengthSamps,
		observedRemotePhaseSamps, 100u, 100u + projectionDistanceSamps);
	EXPECT_EQ(presentAtNonzero.RemotePhaseSamps, presentAtZero.RemotePhaseSamps)
		<< "valid zero and nonzero observation anchors must project equally";
	EXPECT_EQ(presentAtNonzero.LocalDeltaSamps, presentAtZero.LocalDeltaSamps)
		<< "valid zero and nonzero Timer anchors must remain equivalent";
	EXPECT_EQ(950u, presentAtZero.RemotePhaseSamps);
	EXPECT_EQ(-150, presentAtZero.LocalDeltaSamps);

	const auto maxUint = (std::numeric_limits<unsigned int>::max)();
	utils::Timer longRunningTimer;
	longRunningTimer.SetQuantisation(1u, utils::Timer::QUANTISE_MULTIPLE);
	longRunningTimer.SetSeedSourceLength(static_cast<unsigned long>(maxUint));
	longRunningTimer.Tick(maxUint, 0u);
	longRunningTimer.Tick(1u, 0u);
	EXPECT_EQ(static_cast<std::uint64_t>(maxUint) + 1u,
		longRunningTimer.AbsoluteSamplePos())
		<< "Timer absolute position must not wrap at UINT32_MAX";

	utils::Timer crossingTimer;
	crossingTimer.SetQuantisation(1u, utils::Timer::QUANTISE_MULTIPLE);
	crossingTimer.SetSeedSourceLength(static_cast<unsigned long>(maxUint - 7u));
	crossingTimer.Tick(maxUint, 0u);
	crossingTimer.Tick(maxUint, 0u);
	EXPECT_EQ(2ul, crossingTimer.LoopCount())
		<< "Tick must widen phase plus increment before division";
	EXPECT_EQ(14u, crossingTimer.SampOffset())
		<< "Tick must retain the exact remainder after crossing UINT32_MAX";

	ninjam::NinjamRemoteTiming remoteTail;
	remoteTail.IsConnected = true;
	remoteTail.IsValid = true;
	remoteTail.IntervalLengthSamps = 96u;
	remoteTail.IntervalPositionSamps = 95u;
	remoteTail.SourceSampleRate = 96u;
	remoteTail.Bpm = 60.0f;
	remoteTail.Bpi = 1u;
	const auto convertedTail = ninjam::ToDeviceTiming(
		remoteTail, true, 48u, 1u, 0ul, 1u, 0u, 0u);
	ASSERT_TRUE(convertedTail.IsValid);
	ASSERT_EQ(48u, convertedTail.IntervalLengthSamps);
	EXPECT_EQ(47u, convertedTail.IntervalPositionSamps)
		<< "the final 96 Hz source sample must not manufacture an early 48 Hz wrap";
	EXPECT_LT(convertedTail.IntervalPositionSamps, convertedTail.IntervalLengthSamps);
}

TEST(NinjamTiming, ComputesIntervalLengthFromTempo)
{
	EXPECT_EQ(192000u, ninjam::IntervalSampsFromTempo(120.0f, 8u, 48000u));
	EXPECT_EQ(0u, ninjam::IntervalSampsFromTempo(0.0f, 8u, 48000u));
}

TEST(NinjamTiming, UsesPositiveDeltaForHalfIntervalTie)
{
	EXPECT_EQ(500, ninjam::SignedCircularDifference(500u, 0u, 1000u));
	EXPECT_EQ(-400, ninjam::SignedCircularDifference(700u, 300u, 1000u));
}

TEST(NinjamTiming, ResolvesBoundaryReplacementUsingProjectedRemotePhase)
{
	const auto replacement = ninjam::ResolveBoundaryTimingReplacement(
		800ul, 100u, 1000u, 100u, 1000u, 4500u);
	EXPECT_EQ(600u, replacement.RemotePhaseSamps);
	EXPECT_EQ(-300, replacement.LocalDeltaSamps);
	EXPECT_EQ(600u, static_cast<unsigned int>((100 + replacement.LocalDeltaSamps + 800) % 800));
}

TEST(NinjamTiming, BoundaryReplacementFallsBackWithoutUsableAnchor)
{
	const auto zeroAnchor = ninjam::ResolveBoundaryTimingReplacement(
		1000ul, 100u, 1000u, 700u, 0u, 9000u);
	EXPECT_EQ(700u, zeroAnchor.RemotePhaseSamps);
	EXPECT_EQ(-400, zeroAnchor.LocalDeltaSamps);

	const auto earlierBoundary = ninjam::ResolveBoundaryTimingReplacement(
		1000ul, 100u, 1000u, 700u, 9000u, 1000u);
	EXPECT_EQ(700u, earlierBoundary.RemotePhaseSamps);
	EXPECT_EQ(-400, earlierBoundary.LocalDeltaSamps);
}

TEST(NinjamTiming, BoundaryReplacementRetainsRemotePhaseWithoutOldMaster)
{
	const auto replacement = ninjam::ResolveBoundaryTimingReplacement(
		0ul, 0u, 1000u, 100u, 1000u, 3500u);
	EXPECT_EQ(600u, replacement.RemotePhaseSamps);
	EXPECT_EQ(0, replacement.LocalDeltaSamps);
}

TEST(NinjamTiming, BoundaryReplacementUsesOldMasterForNonCommensurateIntervals)
{
	const auto replacement = ninjam::ResolveBoundaryTimingReplacement(
		900ul, 850u, 1000u, 100u, 100u, 800u);
	EXPECT_EQ(800u, replacement.RemotePhaseSamps);
	EXPECT_EQ(-50, replacement.LocalDeltaSamps);
	EXPECT_EQ(800u, static_cast<unsigned int>((850 + replacement.LocalDeltaSamps + 900) % 900));
}

TEST(NinjamTiming, BoundaryReplacementDoesNotAddConfiguredLocalOffset)
{
	const auto replacement = ninjam::ResolveBoundaryTimingReplacement(
		1000ul, 850u, 1200u, 100u, 10000u, 11350u);
	EXPECT_EQ(250u, replacement.RemotePhaseSamps);
	EXPECT_EQ(400, replacement.LocalDeltaSamps);
}

TEST(NinjamTiming, BoundaryReplacementPreservesPositiveHalfIntervalTie)
{
	const auto replacement = ninjam::ResolveBoundaryTimingReplacement(
		1000ul, 500u, 1200u, 0u, 100u, 600u);
	EXPECT_EQ(500u, replacement.RemotePhaseSamps);
	EXPECT_EQ(0, replacement.LocalDeltaSamps);

	const auto tie = ninjam::ResolveBoundaryTimingReplacement(
		1000ul, 500u, 1200u, 0u, 0u, 600u);
	EXPECT_EQ(0u, tie.RemotePhaseSamps);
	EXPECT_EQ(500, tie.LocalDeltaSamps);
}

TEST(NinjamTiming, ConvertsValidRemoteTimingToDeviceDomain)
{
	ninjam::NinjamRemoteTiming remote;
	remote.IntervalLengthSamps = 22050u;
	remote.IntervalPositionSamps = 11025u;
	remote.SourceSampleRate = 44100u;
	remote.Bpm = 120.0f;
	remote.Bpi = 8u;
	remote.IsValid = true;

	const auto timing = ninjam::ToDeviceTiming(remote, true, 48000u, 3u, 7ul, 11u, 13u, 17u);
	EXPECT_TRUE(timing.IsConnected);
	EXPECT_TRUE(timing.IsValid);
	EXPECT_EQ(24000u, timing.IntervalLengthSamps);
	EXPECT_EQ(12000u, timing.IntervalPositionSamps);
	EXPECT_EQ(48000u, timing.DeviceSampleRate);
	EXPECT_EQ(44100u, timing.SourceSampleRate);
	EXPECT_EQ(3u, timing.Generation);
	EXPECT_EQ(7ul, timing.RemoteWrapCount);
	EXPECT_EQ(11u, timing.ObservationSequence);
	EXPECT_EQ(13u, timing.LocalBlockStartSample);
	EXPECT_EQ(17u, timing.AudioBlockStartSample);
}

TEST(NinjamTiming, InvalidRemoteTimingClearsIntervalFields)
{
	ninjam::NinjamRemoteTiming remote;
	remote.IntervalLengthSamps = 22050u;
	remote.IntervalPositionSamps = 11025u;
	remote.SourceSampleRate = 44100u;

	const auto timing = ninjam::ToDeviceTiming(remote, true, 48000u, 3u, 7ul, 11u, 13u, 17u);
	EXPECT_TRUE(timing.IsConnected);
	EXPECT_FALSE(timing.IsValid);
	EXPECT_EQ(0u, timing.IntervalLengthSamps);
	EXPECT_EQ(0u, timing.IntervalPositionSamps);
	EXPECT_EQ(0u, timing.DeviceSampleRate);
	EXPECT_EQ(0u, timing.SourceSampleRate);
}

TEST(NinjamTiming, SharedValidityAcceptsPlausibleTiming)
{
	EXPECT_TRUE(ninjam::IsValidRemoteTiming(22050u, 44100u, 120.0f, 8u));
	EXPECT_TRUE(ninjam::IsValidRemoteTiming(22050u, 44100u, 20.0f, 1u));
	EXPECT_TRUE(ninjam::IsValidRemoteTiming(22050u, 44100u, 400.0f, 32u));
}

TEST(NinjamTimingInput, RejectsNonFiniteTempoWithoutEgress)
{
	constexpr auto intervalLengthSamps = 22050u;
	constexpr auto sourceSampleRate = 44100u;
	constexpr auto conversionSampleRate = 48000u;
	constexpr auto validBpi = 8u;
	constexpr auto minBpm = 20.0f;
	constexpr auto maxBpm = 400.0f;
	ninjam::NinjamConnection disconnected("", "", "", "");
	ASSERT_FALSE(disconnected.IsConnected());

	const auto expectRejected = [&](float bpm, unsigned int bpi)
	{
		EXPECT_FALSE(ninjam::IsValidRemoteTiming(
			intervalLengthSamps, sourceSampleRate, bpm, bpi));
		EXPECT_EQ(0u, ninjam::IntervalSampsFromTempo(bpm, bpi, conversionSampleRate));
		if (bpi <= static_cast<unsigned int>((std::numeric_limits<int>::max)()))
			EXPECT_FALSE(disconnected.RequestServerTempo(bpm, static_cast<int>(bpi)));
	};

	expectRejected(std::numeric_limits<float>::quiet_NaN(), validBpi);
	expectRejected(std::numeric_limits<float>::infinity(), validBpi);
	expectRejected(-std::numeric_limits<float>::infinity(), validBpi);
	expectRejected(0.0f, validBpi);
	expectRejected(-1.0f, validBpi);
	expectRejected(std::nextafter(minBpm, -std::numeric_limits<float>::infinity()), validBpi);
	expectRejected(std::nextafter(maxBpm, std::numeric_limits<float>::infinity()), validBpi);
	expectRejected((std::numeric_limits<float>::lowest)(), validBpi);
	expectRejected((std::numeric_limits<float>::max)(), validBpi);
	expectRejected(120.0f, 0u);
	expectRejected(120.0f, 33u);
	expectRejected(120.0f, (std::numeric_limits<unsigned int>::max)());
	EXPECT_FALSE(ninjam::IsValidNinjamTempo(120.0f, -1));
	EXPECT_FALSE(disconnected.RequestServerTempo(120.0f, -1));
	EXPECT_FALSE(ninjam::IsValidNinjamTempo(120.0f, (std::numeric_limits<int>::max)()));
	EXPECT_FALSE(disconnected.RequestServerTempo(120.0f, (std::numeric_limits<int>::max)()));

	EXPECT_TRUE(ninjam::IsValidRemoteTiming(intervalLengthSamps, sourceSampleRate, minBpm, 1u));
	EXPECT_TRUE(ninjam::IsValidRemoteTiming(intervalLengthSamps, sourceSampleRate, maxBpm, 32u));
	EXPECT_EQ(144000u, ninjam::IntervalSampsFromTempo(minBpm, 1u, conversionSampleRate));
	EXPECT_EQ(230400u, ninjam::IntervalSampsFromTempo(maxBpm, 32u, conversionSampleRate));
	EXPECT_EQ((std::numeric_limits<unsigned int>::max)(), ninjam::IntervalSampsFromTempo(
		minBpm, 32u, (std::numeric_limits<unsigned int>::max)()));
	EXPECT_EQ("20", ninjam::NinjamConnection::FormatTempoBpm(minBpm));
	EXPECT_EQ("400", ninjam::NinjamConnection::FormatTempoBpm(maxBpm));

	// This existing-owner disconnected instance proves rejection results without
	// network I/O. It cannot count sends; no-send remains a structural guarantee
	// of RequestServerTempo's first-statement guard before formatting/locking/sends.
	EXPECT_FALSE(disconnected.IsConnected());
}

TEST(NinjamTiming, SharedValidityRejectsPlaceholderTempo)
{
	// The njclient placeholder (e.g. bpm=2646, bpi=1) satisfies "> 0" but must be
	// rejected on both the live and snapshot paths through one boundary (§2.9).
	EXPECT_FALSE(ninjam::IsValidRemoteTiming(22050u, 44100u, 2646.0f, 1u));
	EXPECT_FALSE(ninjam::IsValidRemoteTiming(22050u, 44100u, 5.0f, 8u));
	EXPECT_FALSE(ninjam::IsValidRemoteTiming(22050u, 44100u, 120.0f, 64u));
	EXPECT_FALSE(ninjam::IsValidRemoteTiming(0u, 44100u, 120.0f, 8u));
	EXPECT_FALSE(ninjam::IsValidRemoteTiming(22050u, 0u, 120.0f, 8u));
}
