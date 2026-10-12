#include "gtest/gtest.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "audio/AudioHost.h"
#include "engine/LoopTake.h"
#include "engine/Quantiser.h"
#include "engine/Station.h"
#include "io/UserConfig.h"
#include "midi/MidiLoop.h"
#include "utils/Timer.h"

namespace audio
{
    class LocalMasterPromotionSafetyTestAccess
    {
    public:
        static void Apply(AudioHost& host,
            const std::vector<std::shared_ptr<engine::Station>>& stations,
            utils::Timer* clock) noexcept
        {
            host.ApplyLocalMasterTimingAtAudioBoundary(stations, clock);
        }
    };
}

class LocalMasterSafetyTake : public engine::LoopTake
{
public:
    LocalMasterSafetyTake(engine::LoopTakeParams params,
        audio::AudioMixerParams mixerParams) : LoopTake(params, mixerParams) {}

    void SetMidiCursor(unsigned long position, unsigned long length) noexcept
    {
        _midiVisualPlayIndex.store(position, std::memory_order_relaxed);
        _midiVisualLoopLength.store(length, std::memory_order_relaxed);
    }
};

class LocalMasterSafetyFixture
{
public:
    static std::shared_ptr<LocalMasterSafetyTake> MakeTake()
    {
        engine::LoopTakeParams params;
        params.Id = "promotion-safety-take";
        params.Size = { 100, 100 };
        audio::MergeMixBehaviourParams merge;
        auto take = std::make_shared<LocalMasterSafetyTake>(params,
            engine::LoopTake::GetMixerParams(params.Size, merge));
        take->Record({}, "station", { 0u }, { "" });
        take->Play(0u, 48000u, 0u);
        take->SetMidiCursor(12000ul, 48000ul);
        return take;
    }

    static std::shared_ptr<engine::Station> MakeStation(
        const std::shared_ptr<LocalMasterSafetyTake>& take)
    {
        engine::StationParams params;
        params.Name = "promotion-safety-station";
        params.Size = { 200, 320 };
        audio::MergeMixBehaviourParams merge;
        auto station = std::make_shared<engine::Station>(params,
            engine::Station::GetMixerParams(params.Size, merge));
        station->AddTake(take);
        station->CommitChanges();
        return station;
    }
};

TEST(AudioHostLocalMasterSafety, RemovedPreparedTargetCannotCommitClock)
{
    auto take = LocalMasterSafetyFixture::MakeTake();
    auto station = LocalMasterSafetyFixture::MakeStation(take);
    const std::vector<std::shared_ptr<engine::Station>> stations{ station };
    const std::vector<std::shared_ptr<engine::Station>> noStations;
    audio::AudioHost host{ io::UserConfig{} };
    auto clock = std::make_shared<utils::Timer>();
    clock->SetQuantisation(6000u, utils::Timer::QUANTISE_POWER);
    clock->SetSeedSourceLength(24000u);
    clock->InitialiseAbsoluteSamplePos(125u);
    host.SetTimingClock(clock);

    const auto version = host.PublishLocalMasterTiming(12000u, 48000u,
        utils::Timer::QUANTISE_POWER, take.get(), nullptr);
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host, stations, clock.get());
    const auto receipt = host.LastPreparedLocalMasterTiming();
    ASSERT_TRUE(receipt.has_value());
    ASSERT_EQ(version, receipt->Version);
    ASSERT_TRUE(take->PrepareLocalMasterQuantisation(12000u, 0u));
    host.CommitLocalMasterTiming(version);

    audio::LocalMasterPromotionSafetyTestAccess::Apply(host, noStations, clock.get());
    EXPECT_EQ(24000ul, clock->SeedSourceLength());
    EXPECT_EQ(125u, clock->SampOffset());
    EXPECT_LT(host.LastActivatedLocalMasterTimingVersion(), version);

    host.CancelLocalMasterTiming(version);
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host, stations, clock.get());
    EXPECT_EQ(24000ul, clock->SeedSourceLength());
    EXPECT_LT(host.LastActivatedLocalMasterTimingVersion(), version);
}

TEST(QuantiserLocalMasterSafety, CancelRestoresPendingGeometry)
{
    auto take = LocalMasterSafetyFixture::MakeTake();
    auto station = LocalMasterSafetyFixture::MakeStation(take);
    const std::vector<std::shared_ptr<engine::Station>> stations{ station };
    auto clock = std::make_shared<utils::Timer>();
    engine::Quantiser quantiser;
    quantiser.SetClock(clock);
    quantiser.Set(777u, utils::Timer::QUANTISE_POWER);
    quantiser.SetLocalMasterTimingPublisher(
        [](const engine::QuantisationTiming&, utils::Timer::QuantisationType,
            const engine::LoopTake*, const engine::Loop*) { return 42u; });

    ASSERT_TRUE(quantiser.TrySetMasterFromHover(take, base::DEPTH_LOOPTAKE,
        stations, 48000u, io::UserConfig{}, false));
    ASSERT_TRUE(quantiser.LocalMasterTimingPending());
    EXPECT_EQ(take.get(), quantiser.PendingLocalMasterTake());
    EXPECT_EQ(42u, quantiser.PendingLocalMasterTimingVersion());
    EXPECT_NE(777u, quantiser.EffectiveSamps());

    quantiser.CancelLocalMasterTiming(42u);
    EXPECT_FALSE(quantiser.LocalMasterTimingPending());
    EXPECT_EQ(nullptr, quantiser.PendingLocalMasterTake());
    EXPECT_EQ(777u, quantiser.EffectiveSamps());
    EXPECT_EQ(777u, clock->QuantiseSamps());
}

TEST(MidiLoopLocalMasterSafety, SourcePublicationWaitsForCallbackClaim)
{
    midi::MidiLoop loop;
    const auto attack = midi::MidiEvent::MakeNoteOn(37u, 0u, 60u, 100u);
    loop.ReplaceRecordedEvents(&attack, 1u, 1000u);
    midi::MidiQuantisationSettings grid;
    grid.Enabled = true;
    grid.GrainSamps = 100u;
    ASSERT_TRUE(loop.PrepareQuantisation(grid, 0u));
    ASSERT_TRUE(loop.TryClaimPreparedQuantisationAtAudioBoundary());

    std::atomic<bool> entered{ false };
    std::atomic<bool> completed{ false };
    const auto replacement = midi::MidiEvent::MakeNoteOn(80u, 0u, 61u, 90u);
    std::thread publisher([&]
    {
        entered.store(true, std::memory_order_release);
        loop.ReplaceRecordedEvents(&replacement, 1u, 1000u);
        completed.store(true, std::memory_order_release);
    });
    for (unsigned int attempt = 0u; attempt < 100000u &&
        !entered.load(std::memory_order_acquire); ++attempt)
        std::this_thread::yield();
    EXPECT_TRUE(entered.load(std::memory_order_acquire));
    for (unsigned int attempt = 0u; attempt < 1000u; ++attempt)
        std::this_thread::yield();
    EXPECT_FALSE(completed.load(std::memory_order_acquire));

    loop.ReleasePreparedQuantisationClaimAtAudioBoundary();
    publisher.join();
    EXPECT_TRUE(completed.load(std::memory_order_acquire));
    EXPECT_FALSE(loop.PreparedQuantisationReadyAtAudioBoundary());
    midi::MidiEvent current{};
    ASSERT_TRUE(loop.TryGetPlaybackEvent(0u, current));
    EXPECT_EQ(61u, current.data1);
}
