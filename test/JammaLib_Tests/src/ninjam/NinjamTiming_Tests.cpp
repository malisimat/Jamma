#include "gtest/gtest.h"
#include "./ninjam/NinjamTiming.h"
#include "./ninjam/NinjamLoopAlignment.h"

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
