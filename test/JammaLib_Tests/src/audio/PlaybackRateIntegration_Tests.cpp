#include "gtest/gtest.h"
#include "audio/PlaybackRate.h"
#include "utils/Timer.h"
#include "ninjam/NinjamTimingCoordinator.h"
#include "io/JamFile.h"
#include <limits>

TEST(PlaybackRateIntegration, MusicalProgressRetainsFractionWithoutChangingPhysicalClock)
{
    utils::Timer timer;
    timer.SetQuantisation(100u, utils::Timer::QUANTISE_MULTIPLE);
    timer.SetSeedSourceLength(700u);
    timer.SetMasterRecordingPlaybackRate(2.0);
    for (unsigned int frame = 0u; frame < 2801u; ++frame)
    {
        timer.SetBlockMusicalAdvance(0.25, 0.5);
        timer.Tick(1u, 0u);
    }
    EXPECT_EQ(2801u, timer.SceneSamplePos());
    EXPECT_EQ(700u, timer.AbsoluteSamplePos());
    EXPECT_EQ(7, timer.LocalMusicalPosition(48000u).BeatsPerInterval);
    EXPECT_DOUBLE_EQ(7200.0, timer.LocalMusicalPosition(48000u).Tempo);
}

TEST(PlaybackRateIntegration, ManualIntentCarriesRateAndDetachEvenWhenDisconnected)
{
    ninjam::NinjamTimingCoordinator coordinator;
    const auto first = coordinator.SetLocalPlaybackRate(0.5);
    ASSERT_TRUE(first.DesiredTransport);
    EXPECT_EQ(ninjam::NinjamLocalFollowPolicy::NoSync, first.DesiredTransport->LocalFollowPolicy);
    EXPECT_DOUBLE_EQ(0.5, first.DesiredTransport->PlaybackRate);
    const auto reset = coordinator.SetLocalPlaybackRate(1.0);
    ASSERT_TRUE(reset.DesiredTransport);
    EXPECT_GT(reset.DesiredTransport->PlaybackRateRevision, first.DesiredTransport->PlaybackRateRevision);
    EXPECT_GT(reset.DesiredTransport->Version, first.DesiredTransport->Version);
    EXPECT_EQ(ninjam::NinjamLocalTempoState::StayLocal, coordinator.LocalTempoState());
    EXPECT_FALSE(coordinator.SetLocalPlaybackRate(std::numeric_limits<double>::quiet_NaN()).DesiredTransport);
    EXPECT_DOUBLE_EQ(1.0, coordinator.LocalPlaybackRate());
}

TEST(PlaybackRateIntegration, ServerRequestRejectsSupersededRateReceipt)
{
    ninjam::NinjamTimingCoordinator coordinator;
    coordinator.Connect({}, std::nullopt);
    const auto first = coordinator.SetLocalPlaybackRate(0.5);
    ASSERT_TRUE(first.DesiredTransport);
    coordinator.SetLocalPlaybackRate(2.0);
    const engine::QuantisationTiming timing{24000u, 168000u, 7u, 120.0f, 7u};
    EXPECT_FALSE(coordinator.RequestServerTempo(timing, first.DesiredTransport->PlaybackRateRevision).TempoRequest);
    EXPECT_EQ(ninjam::NinjamLocalTempoState::StayLocal, coordinator.LocalTempoState());
}

TEST(PlaybackRateIntegration, MissingAndMalformedTakeRatesUseUnityWithoutRewritingMedia)
{
    for (const auto rate : {"", ",\"recordingPlaybackRate\":0", ",\"recordingPlaybackRate\":8", ",\"recordingPlaybackRate\":\"bad\""})
    {
        std::stringstream stream(std::string("{\"name\":\"take\",\"loops\":[]") + rate + "}");
        const auto json = io::Json::FromStream(std::move(stream));
        ASSERT_TRUE(json);
        const auto take = io::JamFile::LoopTake::FromJson(std::get<io::Json::JsonPart>(*json));
        ASSERT_TRUE(take);
        EXPECT_DOUBLE_EQ(1.0, take->RecordingPlaybackRate);
    }
}

TEST(PlaybackRateIntegration, JamRoundTripRetainsMixedRecordingRatesAndFullMasterBpi)
{
    io::JamFile jam{};
    jam.Version = io::JamFile::VERSION_V;
    jam.Name = "rate-roundtrip";
    jam.PlaybackRate = 1.25;
    jam.MasterRecordingPlaybackRate = 0.5;
    jam.MasterBeatsPerInterval = 7u;
    io::JamFile::Station station{};
    station.Name = "station";
    for (const auto rate : {0.5, 1.0, 2.0})
    {
        io::JamFile::LoopTake take{};
        take.Name = "take";
        take.RecordingPlaybackRate = rate;
        take.RecordingMasterLengthSamps = 168000u;
        take.RecordingBeatsPerInterval = 7u;
        station.LoopTakes.push_back(take);
    }
    jam.Stations.push_back(station);
    std::stringstream saved;
    ASSERT_TRUE(io::JamFile::ToStream(jam, saved));
    const auto restored = io::JamFile::FromStream(std::move(saved));
    ASSERT_TRUE(restored);
    EXPECT_DOUBLE_EQ(1.25, restored->PlaybackRate);
    EXPECT_DOUBLE_EQ(0.5, restored->MasterRecordingPlaybackRate);
    EXPECT_EQ(7u, restored->MasterBeatsPerInterval);
    ASSERT_EQ(3u, restored->Stations[0].LoopTakes.size());
    for (std::size_t index = 0; index < 3u; ++index)
    {
        EXPECT_DOUBLE_EQ(station.LoopTakes[index].RecordingPlaybackRate,
            restored->Stations[0].LoopTakes[index].RecordingPlaybackRate);
        EXPECT_EQ(168000u, restored->Stations[0].LoopTakes[index].RecordingMasterLengthSamps);
        EXPECT_EQ(7u, restored->Stations[0].LoopTakes[index].RecordingBeatsPerInterval);
    }
}

TEST(PlaybackRateIntegration, MissingAndMalformedTransportRatesUseUnity)
{
    for (const auto fields : {"", ",\"playbackRate\":0,\"masterRecordingPlaybackRate\":-2",
        ",\"playbackRate\":3,\"masterRecordingPlaybackRate\":\"bad\""})
    {
        const auto document = std::string("{\"name\":\"rate-defaults\",\"stations\":[],\"transport\":{\"initialized\":false") + fields + "}}";
        const auto restored = io::JamFile::FromStream(std::stringstream(document));
        ASSERT_TRUE(restored);
        EXPECT_DOUBLE_EQ(1.0, restored->PlaybackRate);
        EXPECT_DOUBLE_EQ(1.0, restored->MasterRecordingPlaybackRate);
    }
}
