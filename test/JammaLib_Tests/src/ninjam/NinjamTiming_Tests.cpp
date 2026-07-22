#include "gtest/gtest.h"
#include "./ninjam/NinjamTiming.h"

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

TEST(NinjamTiming, ConvertsValidRemoteTimingToDeviceDomain)
{
	ninjam::NinjamRemoteTiming remote;
	remote.IntervalLengthSamps = 22050u;
	remote.IntervalPositionSamps = 11025u;
	remote.SourceSampleRate = 44100u;
	remote.Bpm = 120.0f;
	remote.Bpi = 8u;
	remote.IsValid = true;

	const auto timing = ninjam::ToDeviceTiming(remote, true, 48000u, 3u, 7ul, 11u, 13u);
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
}

TEST(NinjamTiming, InvalidRemoteTimingClearsIntervalFields)
{
	ninjam::NinjamRemoteTiming remote;
	remote.IntervalLengthSamps = 22050u;
	remote.IntervalPositionSamps = 11025u;
	remote.SourceSampleRate = 44100u;

	const auto timing = ninjam::ToDeviceTiming(remote, true, 48000u, 3u, 7ul, 11u, 13u);
	EXPECT_TRUE(timing.IsConnected);
	EXPECT_FALSE(timing.IsValid);
	EXPECT_EQ(0u, timing.IntervalLengthSamps);
	EXPECT_EQ(0u, timing.IntervalPositionSamps);
	EXPECT_EQ(0u, timing.DeviceSampleRate);
	EXPECT_EQ(0u, timing.SourceSampleRate);
}