#include "gtest/gtest.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "audio/AudioHost.h"
#include "engine/LoopTake.h"
#include "engine/Loop.h"
#include "engine/Quantiser.h"
#include "engine/LocalTiming.h"
#include "ninjam/NinjamTimingCoordinator.h"
#include "engine/Station.h"
#include "io/UserConfig.h"
#include "midi/MidiLoop.h"
#include "utils/Timer.h"

namespace audio
{
    class LocalMasterPromotionSafetyTestAccess
    {
    public:
        static bool Claim(AudioHost& host,std::uint64_t version) noexcept { return host.TryClaimLocalMasterActivation(version); }
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

    void MarkIndependent() noexcept { _independentRecording=true; }
    void SetMidiCursor(unsigned long position, unsigned long length) noexcept
    {
        _midiVisualPlayIndex.store(position, std::memory_order_relaxed);
        _midiVisualLoopLength.store(length, std::memory_order_relaxed);
    }
};

class LocalMasterRemoteStation : public engine::Station
{
public:
    LocalMasterRemoteStation(engine::StationParams params,audio::AudioMixerParams mixer):Station(params,mixer) {}
    bool IsRemote() const noexcept override { return true; }
};

class LocalMasterSafetyFixture
{
public:
    static std::shared_ptr<LocalMasterSafetyTake> MakeTake(const std::string& id="promotion-safety-take",bool audio=false)
    {
        engine::LoopTakeParams params;
        params.Id = id;
        params.Size = { 100, 100 };
        audio::MergeMixBehaviourParams merge;
        auto take = std::make_shared<LocalMasterSafetyTake>(params,
            engine::LoopTake::GetMixerParams(params.Size, merge));
        if(audio)
        {
            take->Record({0u},"station"); take->CommitChanges();
            std::vector<float> samples(constants::MaxLoopFadeSamps+48000u,0.25f);
            base::AudioWriteRequest write; write.samples=samples.data(); write.numSamps=static_cast<unsigned int>(samples.size()); write.fadeNew=1.0f;
            auto loop=take->GetLoops().front(); loop->OnBlockWrite(write,0u); loop->EndWrite(write.numSamps,true);
            take->Play(constants::MaxLoopFadeSamps,48000u,0u);
        }
        else { take->Record({}, "station", { 0u }, { "" }); take->Play(0u,48000u,0u); }
        take->SetSampleRate(48000.0f);
        take->SetMidiCursor(12000ul,48000ul);
        return take;
    }

    static std::shared_ptr<engine::Station> MakeStation(
        const std::shared_ptr<LocalMasterSafetyTake>& take)
    {
        engine::StationParams params;
        params.Name = "promotion-safety-station";
        params.SampleRate=48000.0f;
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

TEST(LoopTakeLocalMasterSafety, AudioOnlyReprepareWaitsForCallbackClaimAndKeepsWholeGeometry)
{
    engine::LoopTakeParams params;
    params.Id = "audio-only-tap-safety";
    params.Size = { 100, 100 };
    auto take = std::make_shared<engine::LoopTake>(params,
        engine::LoopTake::GetMixerParams(params.Size, audio::MergeMixBehaviourParams{}));
    take->Record({ 0u }, "station", {}, {});
    take->CommitChanges();
    ASSERT_EQ(1u, take->GetLoops().size());
    ASSERT_TRUE(take->GetMidiLoops().empty());
    auto loop = take->GetLoops().front();
    std::vector<float> samples(constants::MaxLoopFadeSamps + 1000u, 1.0f);
    base::AudioWriteRequest write;
    write.samples = samples.data();
    write.numSamps = static_cast<unsigned int>(samples.size());
    write.stride = 1u;
    write.fadeCurrent = 0.0f;
    write.fadeNew = 1.0f;
    write.source = base::Audible::AUDIOSOURCE_ADC;
    loop->OnBlockWrite(write, 0u);
    loop->EndWrite(static_cast<unsigned int>(samples.size()), true);
    take->Play(0u, 1000u, 0u);
    take->CommitChanges();
    ASSERT_TRUE(take->PrepareLocalMasterQuantisation(100u, 0u, 1000u, 10u));
    ASSERT_TRUE(take->TryClaimPreparedMidiQuantisationAtAudioBoundary());

    std::atomic<bool> entered{ false }, completed{ false }, prepared{ false };
    std::thread publisher([&]
    {
        entered.store(true, std::memory_order_release);
        prepared.store(take->PrepareLocalMasterQuantisation(100u, 0u, 900u, 9u), std::memory_order_release);
        completed.store(true, std::memory_order_release);
    });
    for (unsigned int attempt = 0u; attempt < 100000u &&
        !entered.load(std::memory_order_acquire); ++attempt) std::this_thread::yield();
    EXPECT_TRUE(entered.load(std::memory_order_acquire));
    for (unsigned int attempt = 0u; attempt < 1000u; ++attempt) std::this_thread::yield();
    EXPECT_FALSE(completed.load(std::memory_order_acquire));
    take->ActivatePreparedMidiQuantisationAtAudioBoundary();
    EXPECT_EQ(1000ul, loop->LoopLength());
    EXPECT_EQ(1000u, take->ResolvedMidiQuantisation().BaseIntervalSamps);
    EXPECT_EQ(10u, take->ResolvedMidiQuantisation().BaseDivisions);
    take->ReleasePreparedMidiQuantisationClaimAtAudioBoundary();
    publisher.join();
    ASSERT_TRUE(prepared.load(std::memory_order_acquire));
    ASSERT_TRUE(take->TryClaimPreparedMidiQuantisationAtAudioBoundary());
    take->ActivatePreparedMidiQuantisationAtAudioBoundary();
    EXPECT_EQ(900ul, loop->LoopLength());
    EXPECT_EQ(1000ul, loop->PhysicalLoopLength());
    EXPECT_EQ(900u, take->ResolvedMidiQuantisation().BaseIntervalSamps);
    EXPECT_EQ(9u, take->ResolvedMidiQuantisation().BaseDivisions);
    take->ReleasePreparedMidiQuantisationClaimAtAudioBoundary();
    // Rejected preparation releases the owner claim on its early return too.
    EXPECT_FALSE(take->PrepareLocalMasterQuantisation(100u, 0u, 1001u, 10u));
    EXPECT_TRUE(take->TryClaimPreparedMidiQuantisationAtAudioBoundary());
    take->ReleasePreparedMidiQuantisationClaimAtAudioBoundary();
}

TEST(QuantiserLocalMasterSafety, IdentityNoopsAndBusyRequestsHaveNoTimingTapOrPublisherSideEffects)
{
    using Outcome=engine::Quantiser::MasterOutcome;
    auto a=LocalMasterSafetyFixture::MakeTake("a"),b=LocalMasterSafetyFixture::MakeTake("b"); b->SetMidiCursor(7000u,48000u);
    auto station=LocalMasterSafetyFixture::MakeStation(a); station->AddTake(b); station->CommitChanges();
    auto clock=std::make_shared<utils::Timer>(); engine::Quantiser quantiser; quantiser.SetClock(clock); quantiser.Set(777u,utils::Timer::QUANTISE_POWER);
    audio::AudioHost host(io::UserConfig{}); engine::LocalTiming local; unsigned int publications=0u;
    ninjam::NinjamTimingCoordinator coordinator; ninjam::NinjamTempoJoinOptions options; options.PromptBeforeApplyingRemoteTempo=false; coordinator.Connect(options,std::nullopt);
    quantiser.SetLocalMasterTimingPublisher([&](const auto& timing,auto type,const auto* take,const auto* loop) {
        ++publications; coordinator.BeginLocalTiming();
        const auto published=host.PublishLocalMasterTiming(timing.SeedSamps,timing.MasterLoopSamps,type,take,loop); coordinator.BindLocalTimingVersion(published); return published;
    });
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(1)};
    const auto assign=[&](const auto& take) { return quantiser.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true,now); };
    a->SetGroupMutedAudio(true);
    ASSERT_EQ(Outcome::Applying,assign(a).Outcome); const auto version=quantiser.PendingLocalMasterTimingVersion();
    EXPECT_EQ(Outcome::Unchanged,assign(a).Outcome); EXPECT_EQ(1u,publications);
    EXPECT_EQ(Outcome::Rejected,assign(b).Outcome); EXPECT_EQ(version,quantiser.PendingLocalMasterTimingVersion());
    EXPECT_FALSE(quantiser.HandleTapTempo(10000u,48000u,{station},io::UserConfig{},true)); EXPECT_EQ(1u,publications);
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get()); local.Advance(quantiser,host,{station},now);
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get());
    ASSERT_EQ(engine::LocalTiming::Outcome::Activated,local.Advance(quantiser,host,{station},now).State);
    const auto activated=quantiser.CurrentTempoTiming(48000u); ASSERT_TRUE(activated);
    ASSERT_TRUE(coordinator.PushActivatedLocalTempo(*activated).TempoRequest); coordinator.NotifyTempoRequestSent(true);
    ninjam::NinjamTiming observed; observed.IsConnected=true; observed.IsValid=true; observed.DeviceSampleRate=48000u; observed.SourceSampleRate=48000u;
    observed.IntervalLengthSamps=activated->MasterLoopSamps; observed.IntervalPositionSamps=100u; observed.Bpm=activated->Bpm; observed.Bpi=activated->Bpi;
    observed.HasDeviceAudioSampleAtObservation=true; observed.DeviceAudioSampleAtObservation=1000u; observed.HasLocalTransport=true;
    observed.LocalTransport.MasterLengthSamps=activated->MasterLoopSamps; observed.LocalTransport.AbsoluteSamplePos=100u;
    coordinator.Observe(observed,*activated,true,io::UserConfig{},*clock);
    ASSERT_EQ(ninjam::NinjamLocalTempoState::Following,coordinator.LocalTempoState());
    const auto sent=coordinator.Diagnostics().TempoRequestsSent;
    quantiser.SetRemoteMidiGrid({96000ul,8u,0u,120.0f,1u},0,{station},true);
    quantiser.SetMidiGrain(12000u,"followed remote grid",{station});
    quantiser.SetDefaultMidiQuantisationFraction(midi::MidiQuantisationFraction::Sixth,{station},false);
    const auto absolute=clock->AbsoluteSamplePos(),scene=clock->SceneSamplePos(); const auto grain=quantiser.EffectiveSamps();
    EXPECT_EQ(Outcome::Unchanged,assign(a).Outcome); EXPECT_EQ(1u,publications);
    EXPECT_EQ(absolute,clock->AbsoluteSamplePos()); EXPECT_EQ(scene,clock->SceneSamplePos()); EXPECT_EQ(grain,quantiser.EffectiveSamps());
    EXPECT_EQ(Outcome::Unchanged,quantiser.SetMasterFromTarget(a,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true,now,false).Outcome);
    EXPECT_EQ(ninjam::NinjamLocalTempoState::Following,coordinator.LocalTempoState()); EXPECT_EQ(sent,coordinator.Diagnostics().TempoRequestsSent);
    ASSERT_EQ(Outcome::Applying,assign(b).Outcome); EXPECT_EQ(2u,publications); EXPECT_GT(quantiser.PendingLocalMasterTimingVersion(),version);
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get()); local.Advance(quantiser,host,{station},now);
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get()); local.Advance(quantiser,host,{station},now);
    EXPECT_EQ(7000u,clock->SampOffset());
    b->SetMidiCursor(9000u,96000u);
    EXPECT_EQ(Outcome::Applying,assign(b).Outcome); EXPECT_EQ(3u,publications); // Same identity, changed effective geometry.
}

TEST(QuantiserLocalMasterSafety, DetachedRecordingAndStationOnlyTargetsRejectBeforePublication)
{
    engine::Quantiser quantiser; auto clock=std::make_shared<utils::Timer>(); quantiser.SetClock(clock); quantiser.Set(777u,utils::Timer::QUANTISE_POWER);
    unsigned int calls=0u; quantiser.SetLocalMasterTimingPublisher([&](const auto&,auto,const auto*,const auto*){return ++calls;});
    auto take=LocalMasterSafetyFixture::MakeTake(); auto station=LocalMasterSafetyFixture::MakeStation(take);
    EXPECT_EQ(engine::Quantiser::MasterOutcome::Rejected,quantiser.SetMasterFromTarget(station,base::DEPTH_STATION,{station},48000u,io::UserConfig{},true).Outcome);
    EXPECT_EQ(engine::Quantiser::MasterOutcome::Rejected,quantiser.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,{},48000u,io::UserConfig{},true).Outcome);
    EXPECT_EQ(engine::Quantiser::MasterOutcome::Rejected,quantiser.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true,std::chrono::steady_clock::now(),false).Outcome);
    station->SetMembershipRemoving(true);
    EXPECT_EQ(engine::Quantiser::MasterOutcome::Rejected,quantiser.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true).Outcome);
    station->SetMembershipRemoving(false);
    engine::StationParams params; params.Size={200u,320u};
    auto remote=std::make_shared<LocalMasterRemoteStation>(params,engine::Station::GetMixerParams(params.Size,audio::MergeMixBehaviourParams{}));
    auto remoteTake=LocalMasterSafetyFixture::MakeTake("remote"); remote->AddTake(remoteTake); remote->CommitChanges();
    EXPECT_EQ(engine::Quantiser::MasterOutcome::Rejected,quantiser.SetMasterFromTarget(remoteTake,base::DEPTH_LOOPTAKE,{remote},48000u,io::UserConfig{},true).Outcome);
    auto recording=station->AddTake(); recording->Record({},"busy",{0u},{""}); station->CommitChanges();
    EXPECT_EQ(engine::Quantiser::MasterOutcome::Rejected,quantiser.SetMasterFromTarget(recording,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true).Outcome);
    EXPECT_EQ(0u,calls); EXPECT_EQ(777u,quantiser.EffectiveSamps()); EXPECT_FALSE(quantiser.LocalMasterTimingPending());
}

TEST(AudioHostLocalMasterSafety, DeadlineCancellationWinsBeforeClaimAndLateReceiptsCannotActivate)
{
    auto take=LocalMasterSafetyFixture::MakeTake(); auto station=LocalMasterSafetyFixture::MakeStation(take);
    engine::Quantiser quantiser; auto clock=std::make_shared<utils::Timer>(); quantiser.SetClock(clock); quantiser.Set(777u,utils::Timer::QUANTISE_POWER);
    audio::AudioHost host(io::UserConfig{}); engine::LocalTiming local;
    quantiser.SetLocalMasterTimingPublisher([&](const auto& timing,auto type,const auto* target,const auto* loop){return host.PublishLocalMasterTiming(timing.SeedSamps,timing.MasterLoopSamps,type,target,loop);});
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(1)};
    ASSERT_EQ(engine::Quantiser::MasterOutcome::Applying,quantiser.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true,now).Outcome);
    const auto version=quantiser.PendingLocalMasterTimingVersion();
    const auto result=local.Advance(quantiser,host,{station},now+engine::LocalTiming::PreparationDeadline);
    EXPECT_EQ(engine::LocalTiming::Outcome::Cancelled,result.State); EXPECT_EQ(version,result.Version); EXPECT_EQ(777u,quantiser.EffectiveSamps());
    host.CommitLocalMasterTiming(version); audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get());
    EXPECT_LT(host.LastActivatedLocalMasterTimingVersion(),version); EXPECT_FALSE(quantiser.LocalMasterTimingPending());
    EXPECT_EQ(777u,clock->QuantiseSamps());
}

TEST(AudioHostLocalMasterSafety, LostCancellationPollsWithoutRollingBackClaimedActivation)
{
    auto take=LocalMasterSafetyFixture::MakeTake(); auto station=LocalMasterSafetyFixture::MakeStation(take);
    engine::Quantiser quantiser; auto clock=std::make_shared<utils::Timer>(); quantiser.SetClock(clock); quantiser.Set(777u,utils::Timer::QUANTISE_POWER);
    audio::AudioHost host(io::UserConfig{}); engine::LocalTiming local;
    quantiser.SetLocalMasterTimingPublisher([&](const auto& timing,auto type,const auto* target,const auto* loop){return host.PublishLocalMasterTiming(timing.SeedSamps,timing.MasterLoopSamps,type,target,loop);});
    const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds(1)};
    quantiser.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true,now);
    const auto version=quantiser.PendingLocalMasterTimingVersion(); ASSERT_TRUE(audio::LocalMasterPromotionSafetyTestAccess::Claim(host,version));
    EXPECT_FALSE(host.CancelLocalMasterTiming(version));
    EXPECT_EQ(engine::LocalTiming::Outcome::None,local.Advance(quantiser,host,{station},now+engine::LocalTiming::PreparationDeadline).State);
    EXPECT_TRUE(quantiser.LocalMasterTimingPending()); EXPECT_NE(777u,quantiser.EffectiveSamps());
}

TEST(QuantiserLocalMasterSafety, AutomaticallyAdoptedIndependentMidiMasterHasStableTakeIdentity)
{
    auto take=LocalMasterSafetyFixture::MakeTake(); take->MarkIndependent(); auto station=LocalMasterSafetyFixture::MakeStation(take);
    engine::Quantiser quantiser; auto clock=std::make_shared<utils::Timer>(); quantiser.SetClock(clock);
    const auto timing=engine::Quantiser::DeduceSeedTiming(48000ul,48000u,engine::Quantiser::Policy(io::UserConfig{})); ASSERT_TRUE(timing);
    quantiser.SetSeedUsesPowers(io::UserConfig{}.Loop.SeedUsesPowers); quantiser.ApplyTiming(*timing,"recorded MIDI master");
    quantiser.SetRemoteMidiGrid({96000ul,8u,0u,120.0f,1u},0,{station},true); quantiser.AdoptRecordedMaster({station});
    ASSERT_FALSE(quantiser.CurrentMasterDescription().empty()); EXPECT_TRUE(quantiser.MasterAudioLoopId().empty());
    unsigned int published=0u; quantiser.SetLocalMasterTimingPublisher([&](const auto&,auto,const auto*,const auto*){return ++published;});
    const auto phase=clock->AbsoluteSamplePos();
    EXPECT_EQ(engine::Quantiser::MasterOutcome::Unchanged,quantiser.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true).Outcome);
    EXPECT_EQ(0u,published); EXPECT_EQ(phase,clock->AbsoluteSamplePos());
}

TEST(QuantiserLocalMasterSafety, RemovedAcceptedAudioAndMidiIdentityClearsWithoutChangingTransportOrResurrectingPriorReference)
{
    for(const bool audio:{false,true})
    {
        SCOPED_TRACE(audio);
        auto a=LocalMasterSafetyFixture::MakeTake("accepted",audio),b=LocalMasterSafetyFixture::MakeTake("pending",audio);
        auto station=LocalMasterSafetyFixture::MakeStation(a); station->AddTake(b); station->CommitChanges();
        engine::Quantiser quantiser; auto clock=std::make_shared<utils::Timer>(); quantiser.SetClock(clock);
        ASSERT_EQ(engine::Quantiser::MasterOutcome::Accepted,quantiser.SetMasterFromTarget(a,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true).Outcome);
        ASSERT_FALSE(quantiser.CurrentMasterDescription().empty());
        audio::AudioHost host(io::UserConfig{}); engine::LocalTiming local;
        quantiser.SetLocalMasterTimingPublisher([&](const auto& timing,auto type,const auto* target,const auto* loop){return host.PublishLocalMasterTiming(timing.SeedSamps,timing.MasterLoopSamps,type,target,loop);});
        ASSERT_EQ(engine::Quantiser::MasterOutcome::Applying,quantiser.SetMasterFromTarget(b,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true).Outcome);
        const auto version=quantiser.PendingLocalMasterTimingVersion(),phase=clock->AbsoluteSamplePos(); const auto length=clock->SeedSourceLength();
        ASSERT_TRUE(station->DitchTake(a)); station->CommitChanges(); // A remains pinned externally.
        ASSERT_TRUE(station->DitchTake(b)); station->CommitChanges();
        EXPECT_EQ(engine::LocalTiming::Outcome::Cancelled,local.Advance(quantiser,host,{station}).State);
        EXPECT_TRUE(quantiser.CurrentMasterDescription().empty()); EXPECT_TRUE(quantiser.MasterAudioLoopId().empty());
        EXPECT_EQ(phase,clock->AbsoluteSamplePos()); EXPECT_EQ(length,clock->SeedSourceLength());
        EXPECT_LT(host.LastActivatedLocalMasterTimingVersion(),version);
    }
}

TEST(AudioHostLocalMasterSafety, RemovedAfterActivationBeforeOwnerReceiptDoesNotReinstallCurrentBadge)
{
    auto take=LocalMasterSafetyFixture::MakeTake(); auto station=LocalMasterSafetyFixture::MakeStation(take);
    engine::Quantiser quantiser; auto clock=std::make_shared<utils::Timer>(); quantiser.SetClock(clock);
    audio::AudioHost host(io::UserConfig{}); engine::LocalTiming local;
    quantiser.SetLocalMasterTimingPublisher([&](const auto& timing,auto type,const auto* target,const auto* loop){return host.PublishLocalMasterTiming(timing.SeedSamps,timing.MasterLoopSamps,type,target,loop);});
    quantiser.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true);
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get()); local.Advance(quantiser,host,{station});
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get());
    const auto version=host.LastActivatedLocalMasterTimingVersion(); ASSERT_GT(version,0u);
    const auto phase=clock->AbsoluteSamplePos(); const auto length=clock->SeedSourceLength();
    ASSERT_TRUE(station->DitchTake(take)); station->CommitChanges();
    EXPECT_EQ(engine::LocalTiming::Outcome::Activated,local.Advance(quantiser,host,{station}).State);
    EXPECT_TRUE(quantiser.CurrentMasterDescription().empty()); EXPECT_TRUE(quantiser.MasterAudioLoopId().empty());
    EXPECT_EQ(phase,clock->AbsoluteSamplePos()); EXPECT_EQ(length,clock->SeedSourceLength()); EXPECT_EQ(version,host.LastActivatedLocalMasterTimingVersion());
}

TEST(AudioHostLocalMasterSafety, FinalActivationReceiptAllowsNextRequestWithoutOldGateReplay)
{
    auto a=LocalMasterSafetyFixture::MakeTake("receipt-a"),b=LocalMasterSafetyFixture::MakeTake("receipt-b");
    auto station=LocalMasterSafetyFixture::MakeStation(a); station->AddTake(b); station->CommitChanges();
    audio::AudioHost host(io::UserConfig{}); auto clock=std::make_shared<utils::Timer>(); engine::Quantiser quantiser;
    quantiser.SetClock(clock); engine::LocalTiming local;
    quantiser.SetLocalMasterTimingPublisher([&](const auto& t,auto type,const auto* take,const auto* loop) { return host.PublishLocalMasterTiming(t.SeedSamps,t.MasterLoopSamps,type,take,loop); });
    ASSERT_EQ(engine::Quantiser::MasterOutcome::Applying,quantiser.SetMasterFromTarget(a,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true).Outcome);
    const auto first=quantiser.PendingLocalMasterTimingVersion();
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get()); local.Advance(quantiser,host,{station});
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get());
    ASSERT_EQ(first,host.LastActivatedLocalMasterTimingVersion());
    ASSERT_EQ(engine::LocalTiming::Outcome::Activated,local.Advance(quantiser,host,{station}).State);
    ASSERT_EQ(engine::Quantiser::MasterOutcome::Applying,quantiser.SetMasterFromTarget(b,base::DEPTH_LOOPTAKE,{station},48000u,io::UserConfig{},true).Outcome);
    const auto second=quantiser.PendingLocalMasterTimingVersion(); ASSERT_GT(second,first);
    EXPECT_FALSE(host.CancelLocalMasterTiming(first));
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get()); local.Advance(quantiser,host,{station});
    audio::LocalMasterPromotionSafetyTestAccess::Apply(host,{station},clock.get());
    EXPECT_EQ(second,host.LastActivatedLocalMasterTimingVersion());
    EXPECT_EQ(engine::LocalTiming::Outcome::Activated,local.Advance(quantiser,host,{station}).State);
}
