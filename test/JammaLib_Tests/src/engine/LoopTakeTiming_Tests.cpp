#include "gtest/gtest.h"
#include "../../TestPaths.h"
#include "engine/Scene.h"
#include "io/IoSessionExporter.h"
#include <filesystem>
#include <fstream>
#include <sstream>

#include <memory>
#include <string>
#include <vector>

#include "base/AudioSink.h"
#include "engine/Loop.h"
#include "engine/Station.h"
#include "engine/Quantiser.h"
#include "io/UserConfig.h"
#include "engine/LoopTake.h"
#include "io/JamFile.h"
#include "ninjam/NinjamLoopAlignment.h"
#include "utils/Timer.h"

namespace engine
{
class SceneTimingRestoreTestAccess
{
public:
    static std::shared_ptr<utils::Timer> Clock(const Scene& scene) { return scene._quantisation.Clock(); }
};
}

using audio::MergeMixBehaviourParams;
using base::AudioWriteRequest;
using base::Audible;
using engine::LoopTake;
using engine::LoopTakeParams;
using utils::Timer;

class TimingTestLoopTake : public LoopTake
{
public:
	TimingTestLoopTake(LoopTakeParams params, audio::AudioMixerParams mixerParams) :
		LoopTake(params, mixerParams)
	{
	}

	void SetMidiVisualPosition(unsigned long position, unsigned long length)
	{
		_midiVisualPlayIndex.store(position, std::memory_order_relaxed);
		_midiVisualLoopLength.store(length, std::memory_order_relaxed);
	}

	unsigned long MidiVisualPosition() const
	{
		return _midiVisualPlayIndex.load(std::memory_order_relaxed);
	}

	void InvalidateMidiSceneAnchor()
	{
		_hasMidiSceneAnchor.store(false, std::memory_order_release);
	}

    void PublishCompletionStateBeforeLoopNormalization()
    {
        // Exact first operation of EndRecording, paused before its loop traversal.
        _state.store(STATE_PLAYING, std::memory_order_release);
    }

    void OnBlockWriteChannel(unsigned int channel, const AudioWriteRequest& request, int offset) override
    {
        if (request.source == Audible::AUDIOSOURCE_MIXER)
            RenderedSamples += request.numSamps;
        LoopTake::OnBlockWriteChannel(channel, request, offset);
    }

    unsigned int RenderedSamples = 0u;
};

TEST(TemporaryPlayback, CompletionPublicationBetweenChannelNormalizationsKeepsRendering)
{
    LoopTakeParams params;
    params.Size = {100, 100};
    params.FadeSamps = 0u;
    audio::MergeMixBehaviourParams merge;
    auto take = std::make_shared<TimingTestLoopTake>(params, LoopTake::GetMixerParams(params.Size, merge));
    constexpr auto trigger = 1000u;
    constexpr auto length = 64u;
    constexpr auto count = 4u;
    take->ConfigureTriggerCapture({trigger, -8, 10000u, 0u, 0u});
    take->Record({0u, 1u}, "completion-interleave");
    take->CommitChanges();
    take->BeginCaptureAudioBoundary(trigger, 0u, 0u);
    take->Play(constants::MaxLoopFadeSamps, length, 0u);
    const auto loops = take->GetLoops();
    ASSERT_EQ(2u, loops.size());
    std::vector<float> selected(length, 0.25f);
    AudioWriteRequest request;
    request.samples = selected.data();
    request.numSamps = length;
    request.fadeNew = 1.0f;
    request.fadeCurrent = 0.0f;
    request.stride = 1u;
    request.source = Audible::AUDIOSOURCE_ADC;
    for (const auto& loop : loops)
    {
        loop->OnBlockWrite(request, 0);
        loop->SetBodyPlayIndex(length - 8u);
        ASSERT_TRUE(loop->TriggerCaptureComplete());
    }
    ASSERT_FALSE(take->AudioCaptureReady(count));
    audio::InputHistoryChannel channels[2]{};
    const audio::DacRenderContext dac{{channels, 2u, 0u, 0u, trigger, trigger}};
    auto destination = std::make_shared<base::MultiAudioSink>();
    const auto render = [&]() {
        take->RenderedSamples = 0u;
        take->WriteBlock(destination, nullptr, 0, count, &dac);
        EXPECT_EQ(2u * count, take->RenderedSamples);
    };
    render();
    take->PublishCompletionStateBeforeLoopNormalization();
    render();
    loops[0]->EndRecording();
    ASSERT_FALSE(take->AudioCaptureReady(count));
    render();
    loops[1]->EndRecording();
    ASSERT_TRUE(take->AudioCaptureReady(count));
    render();
}

TEST(TemporaryPlayback, MixedMidiOnlyCancelledAndDitchedTakesKeepRecordedOnlyRouting)
{
    for (auto route = 0u; route < 4u; ++route)
    {
        SCOPED_TRACE(route);
        LoopTakeParams params;
        params.Size = {100, 100};
        params.FadeSamps = 0u;
        audio::MergeMixBehaviourParams merge;
        auto take = std::make_shared<TimingTestLoopTake>(params, LoopTake::GetMixerParams(params.Size, merge));
        take->ConfigureTriggerCapture({1000u, 0, 10000u, 0u, 0u});
        const bool midi = route < 2u;
        take->Record(route == 1u ? std::vector<unsigned int>{} : std::vector<unsigned int>{0u},
            "unsupported-temporary-route", midi ? std::vector<unsigned int>{0u} : std::vector<unsigned int>{},
            midi ? std::vector<std::string>{"keys"} : std::vector<std::string>{});
        take->CommitChanges();
        take->BeginCaptureAudioBoundary(1000u, 0u, 0u);
        take->Play(constants::MaxLoopFadeSamps, 64u, 0u);
        if (route == 2u) take->CancelCapture();
        if (route == 3u) take->Ditch();
        unsigned int reads = 0u;
        audio::InputHistoryChannel channel{};
        const audio::DacRenderContext dac{{&channel, 1u, 0u, 0u, 1000u, 1000u, &reads}};
        auto destination = std::make_shared<base::MultiAudioSink>();
        const auto ready = take->AudioCaptureReady(4u);
        take->WriteBlock(destination, nullptr, 0, 4u);
        const auto baseline = take->RenderedSamples;
        take->RenderedSamples = 0u;
        take->WriteBlock(destination, nullptr, 0, 4u, &dac);
        EXPECT_EQ(baseline, take->RenderedSamples);
        EXPECT_EQ(ready, take->AudioCaptureReady(4u));
        EXPECT_EQ(0u, reads);
    }
}

class TemporaryTimingLoop final : public engine::Loop
{
public:
    TemporaryTimingLoop(engine::LoopParams params, audio::AudioMixerParams mixer) : Loop(params, mixer) {}

    bool InstallSpectralSourceForEligibilityTest()
    {
        auto source = std::make_shared<spectral::AudioSpectralSource>();
        source->Identity = _audioSourceIdentity;
        source->Generation = AudioSourceGeneration();
        source->Layout = {64u, 64u, 0u, 48000.0f};
        source->Samples.resize(source->Layout.StoredSampleCount(), 0.5f);
        auto pcm = std::make_shared<spectral::AudioSpectralPcm>();
        pcm->Source = source;
        std::string reason;
        auto playback = spectral::AudioSpectralPlaybackSnapshot::Create(pcm, nullptr, 1u, 1u, 1u, reason);
        if (!playback) return false;
        _audioSpectralPlayback.store(std::move(playback));
        _activeAudioSpectral = _audioSpectralPlayback.borrow();
        return static_cast<bool>(_activeAudioSpectral);
    }
};

TEST(TemporaryPlayback, SpectralAndPrepassPaddingKeepOriginalLoopReadinessAndNeverReadHistory)
{
    for (const auto spectralSource : {false, true})
    {
        SCOPED_TRACE(spectralSource);
        engine::LoopParams params;
        params.FadeSamps = spectralSource ? 0u : 16u;
        audio::WireMixBehaviourParams wire;
        wire.Channels = {0u};
        auto loop = std::make_shared<TemporaryTimingLoop>(params, engine::Loop::GetMixerParams({80, 80}, wire));
        loop->Record();
        loop->ConfigureTriggerCapture(constants::MaxLoopFadeSamps - 8, true, false);
        ASSERT_TRUE(loop->SetTriggerCaptureLength(64u));
        loop->Play(constants::MaxLoopFadeSamps, 64u, true);
        if (spectralSource) ASSERT_TRUE(loop->InstallSpectralSourceForEligibilityTest());
        else loop->SetBodyPlayIndex(48u); // this request touches unproved prepass padding
        ASSERT_FALSE(loop->CanUseTemporaryPlayback(loop->BodyPlayIndex(), 4u));
        ASSERT_FALSE(loop->TriggerCaptureReady(loop->BodyPlayIndex(), 4u));
        unsigned int reads = 0u;
        audio::InputHistoryChannel channel{};
        const audio::DacRenderContext dac{{&channel, 1u, 0u, 0u, 1000u, 1000u, &reads}};
        float baseline[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        float enabled[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        EXPECT_EQ(loop->ReadBlock(baseline, 0, 4u), loop->ReadBlock(enabled, 0, 4u, &dac, 1000u, -8));
        for (auto i = 0u; i < 4u; ++i) EXPECT_FLOAT_EQ(baseline[i], enabled[i]);
        EXPECT_FALSE(loop->TriggerCaptureReady(loop->BodyPlayIndex(), 4u));
        EXPECT_EQ(0u, reads);
    }
}

static std::shared_ptr<TimingTestLoopTake> MakeTimingTestLoopTake(const std::string& id = "take-0")
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<TimingTestLoopTake>(params, mixerParams);
}

static std::shared_ptr<engine::Loop> MakeTimingLoop(unsigned long loopLength, bool ramp = false)
{
	audio::WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0u };
	audio::AudioMixerParams mixerParams;
	mixerParams.Size = { 160, 320 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = mixBehaviour;

	engine::LoopParams loopParams;
	loopParams.Wav = "phase-test";
	loopParams.Size = { 80, 80 };
	loopParams.Position = { 10, 22 };
	auto loop = std::make_shared<engine::Loop>(loopParams, mixerParams);

	loop->Record();
	const auto recordedLength = constants::MaxLoopFadeSamps + loopLength;
	std::vector<float> samples(recordedLength, 1.0f);
	if (ramp) for (unsigned long i = 0; i < recordedLength; ++i) samples[i] = static_cast<float>(i) / recordedLength;
	AudioWriteRequest request;
	request.samples = samples.data();
	request.numSamps = static_cast<unsigned int>(recordedLength);
	request.stride = 1u;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = Audible::AUDIOSOURCE_ADC;
	loop->OnBlockWrite(request, 0);
	loop->EndWrite(static_cast<unsigned int>(recordedLength), true);
	loop->Play(constants::MaxLoopFadeSamps, loopLength, false);
	return loop;
}

static std::shared_ptr<TimingTestLoopTake> MakePlayingTimingTake(const std::string& id,
	unsigned long loopLength, unsigned long position)
{
	auto take = MakeTimingTestLoopTake(id);
	auto loop = MakeTimingLoop(loopLength);
	loop->ShiftPlayIndex(static_cast<long long>(position));
	take->AddLoop(loop);
	take->CommitChanges();
	take->SetMidiVisualPosition(position % loopLength, loopLength);
	return take;
}

static unsigned long TimingLoopBodyPosition(const engine::Loop& loop)
{
	return loop.PlayIndex() - constants::MaxLoopFadeSamps;
}

static void ExpectMasterPhaseAdjustmentPreservesIndependentLoopPhases()
{
	constexpr unsigned long shortLength = 1000ul;
	constexpr unsigned long longLength = shortLength * 3ul;
	constexpr unsigned long shortStart = shortLength / 4ul;
	constexpr unsigned long longStart = (longLength * 3ul) / 8ul;
	constexpr long long masterDelta = 7600;
	constexpr unsigned long recurrenceSamps = 1400ul;

	Timer master;
	master.SetSeedSourceLength(shortLength);
	master.Tick(static_cast<unsigned int>(shortStart), 0u);
	const Timer::Command masterAdjustment{
		Timer::CommandType::PhaseCorrection, 1u, 0ul, 0u,
		Timer::QUANTISE_OFF, masterDelta };

	auto shortTake = MakePlayingTimingTake("independent-short", shortLength, shortStart);
	auto longTake = MakePlayingTimingTake("independent-long", longLength, longStart);
	master.ApplyCommand(masterAdjustment);
	shortTake->ApplyAcceptedTimingCorrection(masterDelta, 1u);
	longTake->ApplyAcceptedTimingCorrection(masterDelta, 1u);

	for (auto sample = 0ul; sample < recurrenceSamps; ++sample)
	{
		master.Tick(1u, 0u);
		shortTake->EndMultiPlay(1u);
		longTake->EndMultiPlay(1u);
	}

	EXPECT_EQ(shortStart, master.SampOffset());
	EXPECT_EQ(shortStart, TimingLoopBodyPosition(*shortTake->GetLoops().front()));
	EXPECT_EQ(shortStart, shortTake->MidiVisualPosition());
	EXPECT_EQ(longStart, TimingLoopBodyPosition(*longTake->GetLoops().front()));
	EXPECT_EQ(longStart, longTake->MidiVisualPosition());
}

TEST(ExternalPhaseCorrection, SharedDeltaPreservesDifferentTakeLengths)
{
	constexpr unsigned long length = 1000ul;
	auto shortTake = MakePlayingTimingTake("short", length, 0ul);
	auto longTake = MakePlayingTimingTake("long", length * 2ul, length);
	auto oddTake = MakePlayingTimingTake("odd", 777ul, 700ul);

	shortTake->QueueTimingCorrection(2, 1u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	longTake->QueueTimingCorrection(2, 1u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	oddTake->QueueTimingCorrection(100, 1u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	shortTake->EndMultiPlay(0u);
	longTake->EndMultiPlay(0u);
	oddTake->EndMultiPlay(0u);

	EXPECT_EQ(2ul, TimingLoopBodyPosition(*shortTake->GetLoops().front()));
	EXPECT_EQ(length + 2ul, TimingLoopBodyPosition(*longTake->GetLoops().front()));
	EXPECT_EQ(23ul, TimingLoopBodyPosition(*oddTake->GetLoops().front()));
}

TEST(ExternalPhaseCorrection, EveryChannelConsumesInSameBlock)
{
	auto take = MakePlayingTimingTake("channels", 1000ul, 100ul);
	auto secondLoop = MakeTimingLoop(1000ul);
	secondLoop->ShiftPlayIndex(100);
	take->AddLoop(secondLoop);
	take->CommitChanges();

	take->QueueTimingCorrection(-150, 2u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->EndMultiPlay(0u);
	ASSERT_EQ(2u, take->GetLoops().size());
	EXPECT_EQ(950ul, TimingLoopBodyPosition(*take->GetLoops()[0]));
	EXPECT_EQ(950ul, TimingLoopBodyPosition(*take->GetLoops()[1]));
}

TEST(ExternalPhaseCorrection, NegativeDeltaMovesAudioAndMidiWithSameSign)
{
	auto take = MakePlayingTimingTake("negative", 30000ul, 5000ul);
	take->QueueTimingCorrection(-4900, 1u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->EndMultiPlay(0u);

	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(4900, take->MidiAnchorCorrection());
}

TEST(ExternalPhaseCorrection, QueuedEventsAccumulateAndConsumeExactlyOnce)
{
	auto take = MakePlayingTimingTake("accumulate", 1000ul, 100ul);
	take->QueueTimingCorrection(20, 7u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->QueueTimingCorrection(-5, 7u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->EndMultiPlay(10u);
	EXPECT_EQ(125ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(2u, take->QueuedExternalPhaseCorrectionCount());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());

	take->EndMultiPlay(10u);
	EXPECT_EQ(135ul, TimingLoopBodyPosition(*take->GetLoops().front()));
}

TEST(ExternalPhaseCorrection, QueuedLocalCorrectionSurvivesNinjamEpochReset)
{
	auto take = MakePlayingTimingTake("queued-local-across-ninjam-epoch", 1000ul, 100ul);
	take->QueueTimingCorrection(200, 3u, LoopTake::TimingCorrectionReason::PhaseDiscipline);

	// This reset belongs only to the direct NINJAM desired-state consumer. The
	// separate queued local correction remains pending for normal block consume.
	take->ResetTimingEpoch();
	take->EndMultiPlay(0u);

	EXPECT_EQ(300ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(300ul, take->MidiVisualPosition());
	EXPECT_EQ(-200, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, AppliesOnceAndZeroingAppliesExactInverse)
{
	auto take = MakePlayingTimingTake("transport-offset", 1000ul, 100ul);
	take->SetLocalTransportOffsetSamps(350);
	EXPECT_EQ(450ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(450ul, take->MidiVisualPosition());
	EXPECT_EQ(-350, take->MidiAnchorCorrection());

	take->SetLocalTransportOffsetSamps(350);
	EXPECT_EQ(450ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(450ul, take->MidiVisualPosition());

	take->SetLocalTransportOffsetSamps(0);
	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
}

TEST(TransportPhaseOffset, PendingOffsetWaitsForPlayableLoop)
{
	auto take = MakeTimingTestLoopTake("transport-offset-pending");
	take->SetLocalTransportOffsetSamps(250);
	take->SetMidiVisualPosition(100ul, 1000ul);
	take->EndMultiPlay(0u);
	EXPECT_EQ(350ul, take->MidiVisualPosition());
	EXPECT_EQ(-250, take->MidiAnchorCorrection());

	take->SetLocalTransportOffsetSamps(0);
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
}

TEST(TransportPhaseOffset, FractionalTargetsRestoreOriginalCursorWithoutRoundingResidue)
{
	constexpr long long masterLength = 101;
	auto take = MakePlayingTimingTake("transport-offset-fractional", 1000ul, 100ul);
	const auto setFraction = [&](double fraction)
	{
		take->SetLocalTransportOffsetSamps(std::llround(fraction * masterLength));
	};

	setFraction(0.005);
	setFraction(0.010);
	setFraction(0.0);
	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
}

TEST(JamFile, SignedTransportOffsetPreservesM2M3MEntityPhases)
{
	constexpr unsigned long masterLength = 1000ul;
	auto parsed = io::JamFile::FromStream(std::stringstream(
		"{\"name\":\"jam\",\"transportoffsetloopfrac\":-0.25,\"stations\":[]}"));
	ASSERT_TRUE(parsed.has_value());
	const auto targetSamps = std::llround(parsed->TransportOffsetLoopFrac
		* static_cast<double>(masterLength));

	auto take = MakeTimingTestLoopTake("signed-persisted-local-offset");
	for (const auto length : { masterLength, 2ul * masterLength, 3ul * masterLength, 777ul })
	{
		auto loop = MakeTimingLoop(length);
		loop->ShiftPlayIndex(100);
		take->AddLoop(loop);
	}
	take->CommitChanges();
	take->SetMidiVisualPosition(100ul, 2500ul);
	take->SetLocalTransportOffsetSamps(targetSamps);

	ASSERT_EQ(4u, take->GetLoops().size());
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*take->GetLoops()[0]));
	EXPECT_EQ(1850ul, TimingLoopBodyPosition(*take->GetLoops()[1]));
	EXPECT_EQ(2850ul, TimingLoopBodyPosition(*take->GetLoops()[2]));
	EXPECT_EQ(627ul, TimingLoopBodyPosition(*take->GetLoops()[3]));
	EXPECT_EQ(2350ul, take->MidiVisualPosition());
	EXPECT_EQ(250, take->MidiAnchorCorrection());

	constexpr auto globalSample = 5000;
	constexpr auto frozenAutomationGlobalSampleOrigin = 2400;
	const auto automationPosition = static_cast<unsigned long>(
		(globalSample - frozenAutomationGlobalSampleOrigin - take->MidiAnchorCorrection()) % 2500);
	EXPECT_EQ(take->MidiVisualPosition(), automationPosition);
}

TEST(TransportPhaseOffset, DirectTimingCommandRebasesMidiOnlyTake)
{
	auto take = MakeTimingTestLoopTake("midi-only");
	take->SetMidiVisualPosition(100ul, 1000ul);
	take->ApplyAcceptedTimingCorrection(-1250, 1u);
	EXPECT_EQ(850ul, take->MidiVisualPosition());
	EXPECT_EQ(1250, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandMovesAudioAndMidiOnce)
{
	auto take = MakePlayingTimingTake("direct-audio-midi", 1000ul, 100ul);
	take->ApplyAcceptedTimingCorrection(1250, 1u);
	EXPECT_EQ(350ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(350ul, take->MidiVisualPosition());
	EXPECT_EQ(-1250, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandPreservesGenerationAndZeroDeltaGates)
{
	auto take = MakePlayingTimingTake("direct-generation-gates", 1000ul, 100ul);
	take->ApplyAcceptedTimingCorrection(-250, 0u);
	take->ApplyAcceptedTimingCorrection(0, 1u);
	take->ApplyAcceptedTimingCorrection(-250, 1u);

	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(100ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
	EXPECT_EQ(0u, take->ConsumedExternalPhaseCorrectionCount());

	take->ApplyAcceptedTimingCorrection(-250, 2u);
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(850ul, take->MidiVisualPosition());
	EXPECT_EQ(250, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandUsesEachAudioAndMidiModulo)
{
	auto take = MakeTimingTestLoopTake("direct-independent-modulo");
	auto shortLoop = MakeTimingLoop(1000ul);
	auto longLoop = MakeTimingLoop(1500ul);
	shortLoop->ShiftPlayIndex(100);
	longLoop->ShiftPlayIndex(1400);
	take->AddLoop(shortLoop);
	take->AddLoop(longLoop);
	take->CommitChanges();
	take->SetMidiVisualPosition(700ul, 777ul);

	take->ApplyAcceptedTimingCorrection(-250, 1u);

	ASSERT_EQ(2u, take->GetLoops().size());
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*take->GetLoops()[0]));
	EXPECT_EQ(1150ul, TimingLoopBodyPosition(*take->GetLoops()[1]));
	EXPECT_EQ(450ul, take->MidiVisualPosition());
	EXPECT_EQ(250, take->MidiAnchorCorrection());
	EXPECT_EQ(1u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandHandlesAudioOnlyAndEmptyTakes)
{
	auto audioOnly = MakeTimingTestLoopTake("direct-audio-only");
	auto loop = MakeTimingLoop(1000ul);
	loop->ShiftPlayIndex(100);
	audioOnly->AddLoop(loop);
	audioOnly->CommitChanges();
	audioOnly->ApplyAcceptedTimingCorrection(250, 1u);

	EXPECT_EQ(350ul, TimingLoopBodyPosition(*audioOnly->GetLoops().front()));
	EXPECT_EQ(0ul, audioOnly->MidiVisualPosition());
	EXPECT_EQ(0, audioOnly->MidiAnchorCorrection());
	EXPECT_EQ(1u, audioOnly->ConsumedExternalPhaseCorrectionCount());

	auto empty = MakeTimingTestLoopTake("direct-empty");
	empty->ApplyAcceptedTimingCorrection(-250, 1u);
	EXPECT_EQ(0ul, empty->MidiVisualPosition());
	EXPECT_EQ(0, empty->MidiAnchorCorrection());
	EXPECT_EQ(0u, empty->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, LocalOffsetHandlesAudioOnlyMidiOnlyAndEmptyTakes)
{
	auto audioOnly = MakeTimingTestLoopTake("local-offset-audio-only");
	auto loop = MakeTimingLoop(1000ul);
	loop->ShiftPlayIndex(100);
	audioOnly->AddLoop(loop);
	audioOnly->CommitChanges();
	audioOnly->SetLocalTransportOffsetSamps(-250);
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*audioOnly->GetLoops().front()));
	EXPECT_EQ(0, audioOnly->MidiAnchorCorrection());
	audioOnly->SetLocalTransportOffsetSamps(-250);
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*audioOnly->GetLoops().front()));
	audioOnly->SetLocalTransportOffsetSamps(0);
	EXPECT_EQ(100ul, TimingLoopBodyPosition(*audioOnly->GetLoops().front()));

	auto midiOnly = MakeTimingTestLoopTake("local-offset-midi-only");
	midiOnly->SetMidiVisualPosition(700ul, 777ul);
	midiOnly->SetLocalTransportOffsetSamps(-250);
	EXPECT_EQ(450ul, midiOnly->MidiVisualPosition());
	EXPECT_EQ(250, midiOnly->MidiAnchorCorrection());

	auto empty = MakeTimingTestLoopTake("local-offset-empty");
	empty->SetLocalTransportOffsetSamps(-250);
	EXPECT_EQ(0ul, empty->MidiVisualPosition());
	EXPECT_EQ(0, empty->MidiAnchorCorrection());
	auto lateLoop = MakeTimingLoop(1000ul);
	lateLoop->ShiftPlayIndex(100);
	empty->AddLoop(lateLoop);
	empty->CommitChanges();
	empty->EndMultiPlay(0u);
	EXPECT_EQ(850ul, TimingLoopBodyPosition(*empty->GetLoops().front()));
}

TEST(TransportPhaseOffset, DirectTimingCommandKeepsMidiAutomationWithNoteCursor)
{
	auto take = MakePlayingTimingTake("direct-midi-automation", 1000ul, 100ul);
	take->ApplyAcceptedTimingCorrection(250, 1u);

	constexpr auto globalSample = 1000;
	constexpr auto frozenAutomationGlobalSampleOrigin = 900;
	const auto automationPosition = static_cast<unsigned long>(
		(globalSample - frozenAutomationGlobalSampleOrigin - take->MidiAnchorCorrection()) % 1000);
	EXPECT_EQ(350ul, take->MidiVisualPosition());
	EXPECT_EQ(take->MidiVisualPosition(), automationPosition);
}

TEST(TransportPhaseOffset, ContinuousSyncMasterAdjustmentPreservesIndependentAudioAndMidiPhases)
{
	ExpectMasterPhaseAdjustmentPreservesIndependentLoopPhases();
}

TEST(TransportPhaseOffset, BlockSyncMasterAdjustmentPreservesIndependentAudioAndMidiPhases)
{
	ExpectMasterPhaseAdjustmentPreservesIndependentLoopPhases();
}

TEST(TransportPhaseOffset, SyncPhaseMapRebasesAfterTimingCorrection)
{
	auto take = MakePlayingTimingTake("boundary-restore", 1000ul, 100ul);
	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 0);
	take->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(6100u));
	EXPECT_EQ(100ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	take->ApplyAcceptedTimingCorrection(125, 1u);
	// The source ruler moved by the same correction. Rebase it without
	// recapturing this take's anchor.
	map.Rebase(6100u, 1125);
	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(7200u));
	EXPECT_EQ(225ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(225ul, take->MidiVisualPosition());
}

TEST(TransportPhaseOffset, SyncPhaseMapRestoresBeforeRebasingAfterDeviceRateAdvance)
{
	auto take = MakePlayingTimingTake("boundary-restore-device-advance", 1000ul, 100ul);
	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 0);
	take->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	take->EndMultiPlay(550u);
	EXPECT_EQ(650ul, take->MidiVisualPosition());
	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(5550u));
	EXPECT_EQ(600ul, take->MidiVisualPosition());

	take->ApplyAcceptedTimingCorrection(125, 1u);
	map.Rebase(5550u, 625);
	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(6100u));

	EXPECT_EQ(225ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(225ul, take->MidiVisualPosition());
}

TEST(TransportPhaseOffset, SyncPhaseMapMapsAudioAndMidiFromTheSameRemoteMaster)
{
	auto take = MakePlayingTimingTake("boundary-map", 1000ul, 100ul);
	take->ApplyAcceptedTimingCorrection(125, 1u);
	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 0);
	take->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);

	for (const auto scene : { 5000u, 5550u, 6100u, 7200u })
	{
		take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		const auto elapsed = static_cast<std::uint64_t>(scene - 5000u);
		const auto scaled = (elapsed * 1000u + 550u) / 1100u;
		const auto expected = static_cast<unsigned long>((225u + scaled) % 1000u);
		EXPECT_EQ(expected, TimingLoopBodyPosition(*take->GetLoops().front()));
		EXPECT_EQ(expected, take->MidiVisualPosition());
	}
}

TEST(TransportPhaseOffset, SyncPhaseMapRebasePreservesIndependentTakeOrigins)
{
	auto earlyTake = MakePlayingTimingTake("early-sync-origin", 1000ul, 100ul);
	auto lateTake = MakePlayingTimingTake("late-sync-origin", 2000ul, 1700ul);

	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 400);
	earlyTake->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	lateTake->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	for (const auto scene : { 5550u, 6100u, 7200u })
	{
		const auto elapsed = static_cast<std::uint64_t>(scene - 5000u);
		const auto sourceCoordinate = static_cast<std::int64_t>(400u
			+ ((elapsed * 1000u + 550u) / 1100u));
		earlyTake->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		lateTake->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		EXPECT_EQ(ninjam::PositiveModulo(sourceCoordinate - 300, 1000ul),
			TimingLoopBodyPosition(*earlyTake->GetLoops().front()));
		EXPECT_EQ(ninjam::PositiveModulo(sourceCoordinate - 700, 2000ul),
			TimingLoopBodyPosition(*lateTake->GetLoops().front()));
	}

	// A remote phase correction changes the common source phase, never either
	// loop's independently recorded origin.
	earlyTake->ApplyAcceptedTimingCorrection(125, 1u);
	lateTake->ApplyAcceptedTimingCorrection(125, 1u);
	map.Rebase(7200u, 2925);

	for (const auto scene : { 7200u, 7750u, 8300u, 9400u })
	{
		const auto elapsed = static_cast<std::uint64_t>(scene - 7200u);
		const auto sourceCoordinate = static_cast<std::int64_t>(2925u
			+ ((elapsed * 1000u + 550u) / 1100u));
		earlyTake->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		lateTake->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(scene));
		EXPECT_EQ(ninjam::PositiveModulo(sourceCoordinate - 300, 1000ul),
			TimingLoopBodyPosition(*earlyTake->GetLoops().front()));
		EXPECT_EQ(ninjam::PositiveModulo(sourceCoordinate - 700, 2000ul),
			TimingLoopBodyPosition(*lateTake->GetLoops().front()));
	}
}

TEST(TransportPhaseOffset, SyncPhaseMapPreservesLateAudioAndMidiOrigins)
{
	auto take = MakePlayingTimingTake("late-sync-map", 1000ul, 100ul);
	ninjam::SyncPhaseMap map;
	map.SourceLengthSamps = 1000ul;
	map.RemoteLengthSamps = 1100ul;
	map.Rebase(5000u, 0);
	take->CaptureMappedSourceAnchors(map.SourceCoordinateAtOrigin);
	auto loop = take->GetLoops().front();
	loop->SetBodyPlayIndex(700ul);
	loop->InvalidateSceneAnchor();
	take->SetMidiVisualPosition(700ul, 1000ul);
	take->InvalidateMidiSceneAnchor();

	take->RestoreMappedSourceCoordinate(map.SourceCoordinateAt(5550u));
	EXPECT_EQ(700ul, TimingLoopBodyPosition(*loop));
	EXPECT_EQ(700ul, take->MidiVisualPosition());
}

TEST(TransportPhaseOffset, AbsoluteLocalOffsetIsIndependentOfNinjamGeneration)
{
	auto take = MakeTimingTestLoopTake("midi-only-local-offset");
	take->SetMidiVisualPosition(100ul, 1000ul);
	take->ApplyAcceptedTimingCorrection(0, 7u);
	take->SetLocalTransportOffsetSamps(-1250);
	EXPECT_EQ(850ul, take->MidiVisualPosition());
	EXPECT_EQ(1250, take->MidiAnchorCorrection());
	EXPECT_EQ(0u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(TransportPhaseOffset, DirectTimingCommandLeavesEmptyTakeUnmoved)
{
	auto take = MakeTimingTestLoopTake("empty");
	take->ApplyAcceptedTimingCorrection(250, 2u);
	take->ApplyAcceptedTimingCorrection(500, 1u);
	EXPECT_EQ(0ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
	EXPECT_EQ(0u, take->ConsumedExternalPhaseCorrectionCount());
}

TEST(ExternalPhaseCorrection, InvalidationLeavesDisconnectedAdvanceUnchanged)
{
	auto take = MakePlayingTimingTake("disconnect", 1000ul, 100ul);
	take->QueueTimingCorrection(200, 3u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->InvalidateTimingCorrections();
	take->EndMultiPlay(10u);
	take->EndMultiPlay(10u);

	EXPECT_EQ(120ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(120ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiAnchorCorrection());
}

TEST(ExternalPhaseCorrection, ReconnectCannotConsumeStaleGeneration)
{
	auto take = MakePlayingTimingTake("reconnect", 1000ul, 100ul);
	take->QueueTimingCorrection(300, 4u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->InvalidateTimingCorrections();
	take->QueueTimingCorrection(-20, 5u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
	take->EndMultiPlay(10u);

	EXPECT_EQ(90ul, TimingLoopBodyPosition(*take->GetLoops().front()));
	EXPECT_EQ(90ul, take->MidiVisualPosition());
	EXPECT_EQ(20, take->MidiAnchorCorrection());
}

TEST(ExternalPhaseCorrection, LongSimulationRemainsExactAcrossLengths)
{
	const unsigned long lengths[] = { 1000ul, 2000ul, 500ul, 777ul };
	std::shared_ptr<TimingTestLoopTake> takes[std::size(lengths)];
	unsigned long expected[std::size(lengths)]{};
	for (auto index = 0u; index < std::size(lengths); ++index)
		takes[index] = MakePlayingTimingTake("simulation-" + std::to_string(index), lengths[index], 0ul);

	for (auto interval = 0u; interval < 2000u; ++interval)
	{
		const auto delta = static_cast<long long>(interval % 15u) - 7;
		for (auto index = 0u; index < std::size(lengths); ++index)
		{
			takes[index]->QueueTimingCorrection(delta, 9u, LoopTake::TimingCorrectionReason::PhaseDiscipline);
			takes[index]->EndMultiPlay(64u);
			const auto length = static_cast<long long>(lengths[index]);
			auto next = (static_cast<long long>(expected[index]) + 64 + delta) % length;
			if (next < 0)
				next += length;
			expected[index] = static_cast<unsigned long>(next);
			EXPECT_EQ(expected[index], TimingLoopBodyPosition(*takes[index]->GetLoops().front()))
				<< "interval=" << interval << " length=" << lengths[index];
		}
	}
}
#include "engine/Station.h"
#include "midi/MidiBlockTiming.h"

class TriggerPredelayMidiSink : public midi::IMidiOutputSink
{
public:
    std::vector<midi::MidiEvent> Events;
    void OnEvent(unsigned int, const midi::MidiEvent& event) noexcept override { Events.push_back(event); }
};

class TriggerPredelayStationTest
{
public:
    static std::shared_ptr<engine::Station> MakeStation()
    {
        engine::StationParams params;
        params.Name = "trigger-predelay";
        params.Size = {100, 100};
        audio::MergeMixBehaviourParams merge;
        auto station = std::make_shared<engine::Station>(params, engine::Station::GetMixerParams(params.Size, merge));
        station->SetVisible(true);
        station->SetEnabled(true);
        station->SetAllowedMidiChannels({1});
        auto clock = std::make_shared<Timer>();
        clock->SetQuantisation(1000u, Timer::QUANTISE_MULTIPLE);
        clock->SetSeedSourceLength(48000u);
        station->SetClock(clock);
        station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Off);
        return station;
    }

    static actions::TriggerAction Start(std::uint32_t trigger, double delay)
    {
        actions::TriggerAction action;
        action.ActionType = actions::TriggerAction::TRIGGER_REC_START;
        action.MidiSample = trigger;
        action.PreDelayMs = delay;
        action.MidiInputDevices = {"Keys A", "Keys B"};
        io::UserConfig config;
        config.Audio.SampleRate = 48000u;
        action.SetUserConfig(config);
        return action;
    }
};

TEST(TriggerPredelayStation, PhysicalHistoryPublicationGapAndSignedWorldAlignment)
{
    for (const auto delay : {0.0, 80.0, -80.0})
    for (const auto trigger : {100000u, 0xfffff800u})
    {
        SCOPED_TRACE(delay);
        SCOPED_TRACE(trigger);
        const auto station = TriggerPredelayStationTest::MakeStation();
        const auto before = midi::MidiEvent::MakeNoteOn(trigger, 0u, 36u, 101u);
        station->ObservePhysicalMidiForRecording(before, "Keys A");
        const auto started = station->OnAction(TriggerPredelayStationTest::Start(trigger, delay));
        ASSERT_TRUE(started.IsEaten);
        const auto take = station->GetLoopTakes().back();
        // START has not reached the UI/audio topology yet.
        station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOff(trigger + 200u, 0u, 36u), "Keys A");
        station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOn(trigger + 400u, 0u, 38u, 99u), "Keys B");
        station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOff(trigger + 600u, 0u, 38u), "Keys B");
        actions::TriggerAction stop = TriggerPredelayStationTest::Start(trigger + 48000u, -delay);
        stop.ActionType = actions::TriggerAction::TRIGGER_REC_END;
        stop.TargetId = started.TargetId;
        stop.SampleCount = 48000u;
        ASSERT_TRUE(station->OnAction(stop).IsEaten);
        ASSERT_TRUE(take->MidiTriggerCapturePending());
        // Queued ingress may drain after structural STOP, exactly once.
        station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOn(trigger + 47000u, 0u, 40u, 90u), "Keys A");
        station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOff(trigger + 47500u, 0u, 40u), "Keys A");
        station->CompleteMidiIngressPump(trigger + 54000u);
        ASSERT_FALSE(take->MidiTriggerCapturePending());
        ASSERT_EQ(2u, take->GetMidiLoops().size());
        const auto expected = static_cast<std::uint32_t>((static_cast<std::int64_t>(delay * 48.0) + 48000) % 48000);
        unsigned int coincident = 0;
        for (const auto& loop : take->GetMidiLoops())
        {
            for (std::size_t i = 0u; i < loop->EventCount(); ++i)
            {
                midi::MidiEvent event;
                ASSERT_TRUE(loop->TryGetEvent(i, event));
                if (event.IsNoteOn() && event.data1 == 36u)
                {
                    ++coincident;
                    EXPECT_EQ(expected, event.sampleOffset);
                }
            }
        }
        EXPECT_EQ(1u, coincident);
        EXPECT_EQ(trigger, before.sampleOffset);
        EXPECT_EQ(static_cast<std::int64_t>(delay * 48.0), take->CaptureTiming().DelaySamps);
        // Publication can be much later than both close and the ADC history age.
        // MIDI has no ADC admission requirement and joins at the current phase.
        take->BeginCaptureAudioBoundary(trigger + 192000u, 900000u, 192000u);
        TriggerPredelayMidiSink sink;
        take->ReadMidiBlock(trigger + 192000u, 1u, sink);
        ASSERT_EQ(1u, sink.Events.size());
        EXPECT_EQ(36u, sink.Events[0].data1);
        EXPECT_EQ(trigger + 192000u, sink.Events[0].sampleOffset);

    }
}

TEST(TriggerPredelayStation, OutOfOrderEndpointsRetainShortPretriggerPair)
{
    const auto station = TriggerPredelayStationTest::MakeStation();
    constexpr auto trigger = 100000u;
    // Newer A is drained before older B. Never break history search on A.
    station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOn(trigger + 10u, 0u, 42u, 70u), "Keys A");
    station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOn(trigger - 3744u, 0u, 36u, 110u), "Keys B");
    station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOff(trigger - 3600u, 0u, 36u), "Keys B");
    const auto started = station->OnAction(TriggerPredelayStationTest::Start(trigger, 80.0));
    ASSERT_TRUE(started.IsEaten);
    const auto take = station->GetLoopTakes().back();
    auto stop = TriggerPredelayStationTest::Start(trigger + 48000u, 0.0);
    stop.ActionType = actions::TriggerAction::TRIGGER_REC_END;
    stop.TargetId = started.TargetId;
    stop.SampleCount = 48000u;
    ASSERT_TRUE(station->OnAction(stop).IsEaten);
    station->CompleteMidiIngressPump(trigger + 54000u);
    ASSERT_EQ(2u, take->GetMidiLoops().size());
    midi::MidiEvent on, off;
    ASSERT_TRUE(take->GetMidiLoops()[1]->TryGetEvent(0u, on));
    ASSERT_TRUE(take->GetMidiLoops()[1]->TryGetEvent(1u, off));
    EXPECT_TRUE(on.IsNoteOn());
    EXPECT_EQ(96u, on.sampleOffset);
    EXPECT_TRUE(off.IsNoteOff());
    EXPECT_EQ(240u, off.sampleOffset);
}

TEST(TriggerPredelayStation, TooShortSignedCaptureRejectedBeforePlayback)
{
    const auto station = TriggerPredelayStationTest::MakeStation();
    auto started = station->OnAction(TriggerPredelayStationTest::Start(100000u, -200.0));
    ASSERT_TRUE(started.IsEaten);
    auto stop = TriggerPredelayStationTest::Start(101000u, 0.0);
    stop.ActionType = actions::TriggerAction::TRIGGER_REC_END;
    stop.TargetId = started.TargetId;
    stop.SampleCount = 1000u;
    const auto take = station->GetLoopTakes().back();
    station->OnAction(stop);
    EXPECT_TRUE(take->IsCaptureCancelled());
}

#include "midi/MidiRouter.h"
#include "engine/RigSnapshot.h"

namespace midi
{
    // Inject only transport ingress; PumpMidi, recipient routing, history and
    // completion remain the production path. No timing logic is duplicated.
    class MidiRouterCaptureTestAccess
    {
    public:
        static void Setup(MidiRouter& router, const std::shared_ptr<engine::Station>& station)
        {
            auto inputs = std::make_shared<std::vector<std::shared_ptr<MidiRouter::MidiInputEndpoint>>>();
            for (std::uint8_t slot = 0; slot < 2u; ++slot)
            {
                auto endpoint = std::make_shared<MidiRouter::MidiInputEndpoint>();
                endpoint->DeviceSlot = slot;
                endpoint->ConfiguredName = slot ? "Keys B" : "Keys A";
                endpoint->Device = std::make_unique<MidiDevice>();
                inputs->push_back(endpoint);
            }
            router._midiInputs.store(inputs);
            router._liveMidiDispatchNotification = std::make_shared<MidiRouter::LiveMidiDispatchNotification>();
            auto rig = std::make_shared<engine::RigSnapshot>();
            rig->Revision = 1u;
            rig->InputDispatch.LiveMidi = {{"Keys A", {station}}, {"Keys B", {station}}};
            router.PublishRigInputDispatch(rig);
        }
        static void Push(MidiRouter& router, unsigned int slot, const MidiEvent& event, std::int64_t arrival = 0)
        {
            MidiRouter::MidiInputEndpoint::RigMidiIngressEvent ingress;
            ingress.Event = event;
            ingress.RigRevision = 1u;
            ingress.CallbackArrivalMicros = arrival;
            ingress.RoutingGeneration = 1u;
            ASSERT_TRUE(router._midiInputs.load()->at(slot)->Ingress.Push(ingress));
        }
        static std::int64_t ArmBoundary(const MidiRouter& router) { return router._triggerLearnStartMicros; }
        static void Serial(MidiRouter& router, const std::string& device, bool down)
        { ASSERT_TRUE(router._serialIngress.Push({{&device, 7u, down},1u,++router._serialEnqueueSequence})); }
        static void OverrideTriggers(MidiRouter& router)
        { router._channelOverrideTriggers.store(true); router._channelOverrideLive.store(false); router.SetForcedChannelOverride(3u, {}); }
    };
}

TEST(TriggerPredelayStation, RouterDrainsEveryEndpointBeforeClosingAndCapturesUpwardQuantisation)
{
    for (const auto delay : {0.0, 80.0})
    {
        auto station = TriggerPredelayStationTest::MakeStation();
        midi::MidiRouter router;
        midi::MidiRouterCaptureTestAccess::Setup(router, station);
        constexpr auto trigger = 100000u;
        const auto onset = trigger - (delay > 0.0 ? 3744u : 0u);
        midi::MidiRouterCaptureTestAccess::Push(router, 0u, midi::MidiEvent::MakeNoteOn(trigger + 10u, 0u, 42u, 70u));
        midi::MidiRouterCaptureTestAccess::Push(router, 1u, midi::MidiEvent::MakeNoteOn(onset, 0u, 36u, 100u));
        midi::MidiRouterCaptureTestAccess::Push(router, 1u, midi::MidiEvent::MakeNoteOff(onset + 144u, 0u, 36u));
        io::UserConfig config;
        config.Audio.SampleRate = 48000u;
        audio::AudioStreamParams params;
        params.SampleRate = 48000u;
        router.PumpMidi({station}, trigger + 64u, config, params);
        auto started = station->OnAction(TriggerPredelayStationTest::Start(trigger, delay));
        ASSERT_TRUE(started.IsEaten);
        auto take = station->GetLoopTakes().back();
        // Queued before STOP, drained only after structural END.
        midi::MidiRouterCaptureTestAccess::Push(router, 1u, midi::MidiEvent::MakeNoteOn(trigger + 1000u, 0u, 38u, 90u));
        midi::MidiRouterCaptureTestAccess::Push(router, 1u, midi::MidiEvent::MakeNoteOff(trigger + 1200u, 0u, 38u));
        auto stop = TriggerPredelayStationTest::Start(trigger + 47550u, 0.0);
        stop.ActionType = actions::TriggerAction::TRIGGER_REC_END;
        stop.TargetId = started.TargetId;
        stop.SampleCount = 47550u;
        ASSERT_TRUE(station->OnAction(stop).IsEaten);
        EXPECT_TRUE(take->MidiTriggerCapturePending());
        // D0 has U>S. Its after-STOP note must survive, its event at U must not.
        midi::MidiRouterCaptureTestAccess::Push(router, 1u, midi::MidiEvent::MakeNoteOn(trigger + 47800u, 0u, 40u, 80u));
        midi::MidiRouterCaptureTestAccess::Push(router, 1u, midi::MidiEvent::MakeNoteOff(trigger + 47900u, 0u, 40u));
        midi::MidiRouterCaptureTestAccess::Push(router, 1u, midi::MidiEvent::MakeNoteOn(trigger + 48000u, 0u, 44u, 80u));
        router.PumpMidi({station}, trigger + 53000u, config, params);
        EXPECT_FALSE(take->MidiTriggerCapturePending());
        unsigned int kicks = 0, queued = 0, future = 0, excluded = 0;
        for (const auto& loop : take->GetMidiLoops())
        for (std::size_t i = 0; i < loop->EventCount(); ++i)
        {
            midi::MidiEvent event;
            ASSERT_TRUE(loop->TryGetEvent(i, event));
            if (!event.IsNoteOn()) continue;
            if (event.data1 == 36u) { ++kicks; EXPECT_EQ(delay > 0.0 ? 96u : 0u, event.sampleOffset); }
            if (event.data1 == 38u) ++queued;
            if (event.data1 == 40u) ++future;
            if (event.data1 == 44u) ++excluded;
        }
        EXPECT_EQ(1u, kicks);
        EXPECT_EQ(1u, queued);
        EXPECT_EQ(delay > 0.0 ? 0u : 1u, future);
        EXPECT_EQ(0u, excluded);
    }
}

TEST(TriggerPredelayStation, ZeroReportedHardwareLatencyUsesConfiguredSourceAge)
{
    auto station = TriggerPredelayStationTest::MakeStation();
    auto action = TriggerPredelayStationTest::Start(100000u, 0.0);
    action.InputChannels = {0u};
    action.MidiInputDevices.clear();
    io::UserConfig config;
    config.Audio.SampleRate = 48000u;
    config.Audio.LatencyIn = 100000u;
    action.SetUserConfig(config);
    audio::AudioStreamParams params;
    params.SampleRate = 48000u;
    params.InputLatency = 0u;
    action.SetAudioParams(params);
    ASSERT_TRUE(station->OnAction(action).IsEaten);
    EXPECT_EQ(100000u, station->GetLoopTakes().back()->CaptureTiming().SourceAgeSamps);
}

TEST(TriggerPredelayStation, FirstAudioAndMixedMasterReserveCompensatedOrigin)
{
    for (const auto delay : {0.0, 80.0, -80.0})
    for (const auto withMidi : {false, true})
    {
        SCOPED_TRACE(delay);
        SCOPED_TRACE(withMidi);
        auto station = TriggerPredelayStationTest::MakeStation();
        auto clock = std::make_shared<Timer>();
        station->SetClock(clock);
        // Keep the first recorded master behind unrelated earlier material.
        if (delay == 0.0 && !withMidi)
        {
            station->AddTake(MakePlayingTimingTake("earlier-nonmaster",48000ul,0ul));
            station->CommitChanges();
        }
        constexpr auto trigger = 100000u;
        auto start = TriggerPredelayStationTest::Start(trigger, delay);
        start.InputChannels = {0u};
        if (!withMidi) start.MidiInputDevices.clear();
        const auto started = station->OnAction(start);
        ASSERT_TRUE(started.IsEaten);
        auto take = station->GetLoopTakes().back();
        take->CommitChanges();
        station->CommitChanges();
        // START publication can lag its admitted edge. The callback counter
        // then understates duration; first-master length must use edge stamps.
        constexpr auto handoffDelay = 4096u;
        auto current = trigger + handoffDelay;
        const auto advance = [&](std::uint32_t end) {
            std::vector<float> input(128u, 0.0f);
            while (current < end)
            {
                const auto count = (std::min)(128u, end - current);
                station->BeginCaptureAudioBoundary(current, current, 0u);
                base::AudioWriteRequest request;
                request.samples = input.data();
                request.numSamps = count;
                request.stride = 1u;
                request.fadeNew = 1.0f;
                request.source = base::Audible::AUDIOSOURCE_ADC;
                take->OnBlockWriteChannel(0u, request, 0);
                take->EndMultiWrite(count, true, base::Audible::AUDIOSOURCE_ADC);
                take->EndMultiPlay(count);
                current += count;
            }
        };
        advance(trigger + 48000u);
        station->BeginCaptureAudioBoundary(current, current, 0u);
        auto stop = start;
        stop.ActionType = actions::TriggerAction::TRIGGER_REC_END;
        stop.TargetId = started.TargetId;
        stop.MidiSample = trigger + 48000u;
        stop.SampleCount = 48000u - handoffDelay;
        ASSERT_TRUE(station->OnAction(stop).IsEaten);
        // Geometry and its compensated zero are committed together by the
        // callback after source coverage and topology publication are ready.
        EXPECT_EQ(0ul, clock->SeedSourceLength());
        const auto seed = clock->PendingRecordingSeedOnOwner();
        ASSERT_TRUE(seed);
        ASSERT_EQ(48000u, seed->LengthSamps);
        EXPECT_EQ(trigger - static_cast<std::uint32_t>(static_cast<std::int32_t>(delay * 48.0)),
            seed->RecordOrigin);
        EXPECT_GT(seed->LengthSamps, 0ul);
        EXPECT_TRUE(take->FirstMasterActivationPending());
        const auto deadline = trigger + seed->LengthSamps - take->CaptureTiming().SeamSamps
            + take->CaptureTiming().SourceAgeSamps;
        advance(deadline + 10000u);
        station->CompleteMidiIngressPump(current);
        for (const auto& job : take->CommitChanges()) take->OnAction(job);
        take->CommitChanges();
        station->CommitChanges();
        ASSERT_FALSE(take->IsCaptureCancelled());
        ASSERT_EQ(LoopTake::STATE_PLAYING, take->TakeState());
        station->BeginCaptureAudioBoundary(current, current, 0u);
        station->ApplyFirstMasterRecordingAtAudioBoundary(*clock, current);
        EXPECT_FALSE(take->FirstMasterActivationPending());
        ASSERT_EQ(seed->LengthSamps, clock->SeedSourceLength());
        const auto expected = static_cast<unsigned long>((static_cast<std::int64_t>(current - trigger)
            + take->CaptureTiming().DelaySamps + seed->LengthSamps) % seed->LengthSamps);
        EXPECT_EQ(expected, clock->SampOffset());
        EXPECT_EQ(expected, take->MidiPlayIndex());
        ASSERT_EQ(1u, take->GetLoops().size());
        EXPECT_EQ(expected, take->GetLoops().front()->BodyPlayIndex());
        if (delay == 0.0 && !withMidi)
        {
            station->CommitChanges(); // Consume the actual accepted master receipt.
            ASSERT_EQ(take,station->RecordedMasterTakeOnOwner());
            auto firstStation = TriggerPredelayStationTest::MakeStation();
            firstStation->AddTake(MakePlayingTimingTake("first-station-decoy",48000ul,0ul));
            firstStation->CommitChanges();
            const std::vector<std::shared_ptr<engine::Station>> stations{firstStation,station};
            engine::Quantiser quantiser; quantiser.SetClock(clock);
            quantiser.SetMidiGrain(clock->QuantiseSamps(),"accepted recording",stations);
            const auto master = take->GetLoops().front();
            ASSERT_FALSE(master->Id().empty());
            ASSERT_EQ(master->Id(),quantiser.MasterAudioLoopId());
            master->SetPlaybackOffsetSamps(-1500);
            quantiser.UpdateStationHints(nullptr,base::DEPTH_LOOP,false,stations);
            struct DirectoryCleanup
            {
                std::filesystem::path Path;
                ~DirectoryCleanup() { std::error_code error; std::filesystem::remove_all(Path,error); }
            } directory{TestPaths::FixtureDirectory() / ("recorded-master-" + std::to_string(GetCurrentProcessId()))};
            ASSERT_TRUE(std::filesystem::create_directory(directory.Path));
            std::mutex sceneMutex;
            audio::AudioStreamParams stream; stream.SampleRate = 48000u;
            ASSERT_TRUE(io::IoSessionExporter::ExportSessionToDirectory(stations,nullptr,quantiser,
                io::JamFile::GlobalMidiQuantState::Off,0.0,io::UserConfig{},stream,nullptr,
                sceneMutex,nullptr,directory.Path.wstring()));
            std::ifstream file(directory.Path / "session.jam"); std::stringstream text; text << file.rdbuf(); file.close();
            auto jam = io::JamFile::FromStream(std::move(text)); ASSERT_TRUE(jam);
            EXPECT_EQ(master->Id(),jam->MasterAudioLoopId);
            ASSERT_EQ(2u,jam->Stations.size()); ASSERT_EQ(2u,jam->Stations[1].LoopTakes.size());
            ASSERT_EQ(1u,jam->Stations[1].LoopTakes[1].Loops.size());
            EXPECT_EQ(master->Id(),jam->Stations[1].LoopTakes[1].Loops[0].Id);
            auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson)); ASSERT_TRUE(rig);
            rig->Triggers.clear(); rig->User.Midi.Devices.clear();
            auto loaded = engine::Scene::FromFile(engine::SceneParams({""},{},{640u,480u}),*jam,*rig,directory.Path.wstring());
            ASSERT_TRUE(loaded);
            const auto restoredStations = (*loaded)->SnapshotStations(); ASSERT_EQ(2u,restoredStations.size());
            const auto restoredClock = engine::SceneTimingRestoreTestAccess::Clock(**loaded);
            ASSERT_TRUE(restoredClock);
            EXPECT_EQ(jam->QuantiseSamps,restoredClock->QuantiseSamps());
            EXPECT_EQ(jam->MasterLengthSamps,restoredClock->SeedSourceLength());
            EXPECT_EQ(jam->AbsoluteSamplePos,restoredClock->AbsoluteSamplePos());
            EXPECT_EQ(-1500,restoredStations[0]->MasterAudioReferenceOffsetSamps());
            EXPECT_EQ(-1500,restoredStations[1]->MasterAudioReferenceOffsetSamps());
            (*loaded)->CommitChanges(); // Publish restored material through the normal scene boundary.
            ASSERT_EQ(2u,restoredStations[1]->GetLoopTakes().size());
            ASSERT_EQ(1u,restoredStations[1]->GetLoopTakes()[1]->GetLoops().size());
            const auto restoredMaster = restoredStations[1]->GetLoopTakes()[1]->GetLoops()[0];
            EXPECT_EQ(master->Id(),restoredMaster->Id());
            EXPECT_EQ(-1500,restoredMaster->PlaybackOffsetSamps());
            EXPECT_EQ(master->PlaybackBodyIndex(),restoredMaster->PlaybackBodyIndex());
            EXPECT_EQ(0,restoredStations[1]->GetLoopTakes()[0]->GetLoops()[0]->PlaybackOffsetSamps());
            (*loaded)->Shutdown();
            auto emptyJam = *jam;
            for (auto& savedStation : emptyJam.Stations) savedStation.LoopTakes.clear();
            auto empty = engine::Scene::FromFile(engine::SceneParams({""},{},{640u,480u}),emptyJam,*rig,directory.Path.wstring());
            ASSERT_TRUE(empty);
            const auto emptyClock = engine::SceneTimingRestoreTestAccess::Clock(**empty);
            ASSERT_TRUE(emptyClock);
            EXPECT_EQ(0u,emptyClock->QuantiseSamps());
            EXPECT_EQ(0ul,emptyClock->SeedSourceLength());
            EXPECT_EQ(0u,emptyClock->AbsoluteSamplePos());
            (*empty)->Shutdown();
        }
    }
}

TEST(TriggerPredelayStation, FirstAudioMasterPlaysReadyRangesBeforeTailCompletion)
{
    for (const auto delay : {80.0, -80.0})
    {
        SCOPED_TRACE(delay);
        auto station = TriggerPredelayStationTest::MakeStation();
        auto clock = std::make_shared<Timer>();
        station->SetClock(clock);
        constexpr auto trigger = 100000u;
        constexpr auto length = 192000u;
        constexpr auto block = 128u;
        auto start = TriggerPredelayStationTest::Start(trigger, delay);
        start.InputChannels = {0u};
        start.MidiInputDevices.clear();
        const auto started = station->OnAction(start);
        ASSERT_TRUE(started.IsEaten);
        auto take = station->GetLoopTakes().back();
        take->CommitChanges();
        station->CommitChanges();
        std::vector<float> input(block, 0.25f);
        for (auto current = trigger; current < trigger + length; current += block)
        {
            station->BeginCaptureAudioBoundary(current, current, 0u);
            base::AudioWriteRequest request;
            request.samples = input.data();
            request.numSamps = block;
            request.stride = 1u;
            request.fadeNew = 1.0f;
            request.source = base::Audible::AUDIOSOURCE_ADC;
            take->OnBlockWriteChannel(0u, request, 0);
            take->EndMultiWrite(block, true, base::Audible::AUDIOSOURCE_ADC);
            take->EndMultiPlay(block);
        }
        station->BeginCaptureAudioBoundary(trigger + length, trigger + length, 0u);
        auto stop = start;
        stop.ActionType = actions::TriggerAction::TRIGGER_REC_END;
        stop.TargetId = started.TargetId;
        stop.MidiSample = trigger + length;
        stop.SampleCount = length;
        ASSERT_TRUE(station->OnAction(stop).IsEaten);
        station->BeginCaptureAudioBoundary(trigger + length, trigger + length, 0u);
        station->ApplyFirstMasterRecordingAtAudioBoundary(*clock, trigger + length);
        ASSERT_EQ(LoopTake::STATE_PLAYINGRECORDING, take->TakeState());
        EXPECT_FALSE(take->FirstMasterActivationPending());
        EXPECT_EQ(length, clock->SeedSourceLength());
        ASSERT_EQ(1u, take->GetLoops().size());
        const auto loop = take->GetLoops().front();
        EXPECT_FALSE(loop->TriggerCaptureComplete());
        std::vector<float> output(block, 0.0f);
        if (delay > 0.0)
        {
            ASSERT_TRUE(take->AudioCaptureReady(block));
            ASSERT_EQ(block, loop->ReadBlock(output.data(), 0, block));
            EXPECT_FLOAT_EQ(0.25f, output.front());
        }
        else
        {
            EXPECT_FALSE(take->AudioCaptureReady(block));
            EXPECT_EQ(0u, loop->ReadBlock(output.data(), 0, block));
        }
    }
}

TEST(TriggerPredelayStation, NewFormatRejectsLookbackAcrossItsEpoch)
{
    auto station = TriggerPredelayStationTest::MakeStation();
    station->ObservePhysicalMidiForRecording(midi::MidiEvent::MakeNoteOn(10000u, 0u, 36u, 100u), "Keys A");
    station->SetSampleRate(48000.0f);
    station->BeginCaptureAudioBoundary(20000u, 90000u, 0u);
    auto action = TriggerPredelayStationTest::Start(20100u, 80.0);
    EXPECT_FALSE(station->OnAction(action).IsEaten);
    action.PreDelayMs = 0.0;
    ASSERT_TRUE(station->OnAction(action).IsEaten);
    const auto take = station->GetLoopTakes().back();
    midi::MidiEvent continuation;
    ASSERT_TRUE(take->GetMidiLoops()[0]->TryGetEvent(0u, continuation));
    EXPECT_EQ(0u, continuation.sampleOffset);
    EXPECT_EQ(36u, continuation.data1);
}


TEST(TriggerBindingLearn, FreshValidatedPassiveIngressFreezesOneCandidateAndInfersInverseEdges)
{
    auto station = TriggerPredelayStationTest::MakeStation();
    midi::MidiRouter router; midi::MidiRouterCaptureTestAccess::Setup(router, station);
    ASSERT_TRUE(router.OpenRigTriggerInput(1u));
    station->SetAllowedMidiChannels({1u,2u,3u});
    const auto started = station->OnAction(TriggerPredelayStationTest::Start(100u,0.0)); ASSERT_TRUE(started.IsEaten);
    const auto take = station->GetLoopTakes().back();
    io::UserConfig config; config.Audio.SampleRate = 48000u;
    audio::AudioStreamParams params; params.SampleRate = 48000u;
    router.RequestTriggerLearn({10u,1u,1u});
    midi::MidiRouterCaptureTestAccess::Push(router,0u,midi::MidiEvent::MakeNoteOn(100u,0u,60u,100u));
    midi::MidiRouterCaptureTestAccess::Push(router,0u,midi::MidiEvent::MakeNoteOff(110u,0u,60u));
    midi::MidiRouterCaptureTestAccess::Push(router,0u,midi::MidiEvent::MakeNoteOn(150u,1u,61u,100u));
    router.PumpMidi({station},200u,config,params);
    ASSERT_TRUE(router.TriggerLearnObservation()); EXPECT_TRUE(router.TriggerLearnObservation()->Armed);
    EXPECT_FALSE(router.TriggerLearnObservation()->Binding); // Queued pre-arm input still drained normally.
    const auto boundary = midi::MidiRouterCaptureTestAccess::ArmBoundary(router);
    midi::MidiRouterCaptureTestAccess::OverrideTriggers(router);
    midi::MidiRouterCaptureTestAccess::Push(router,0u,midi::MidiEvent::MakeNoteOn(201u,1u,61u,0u),boundary+1);
    midi::MidiRouterCaptureTestAccess::Push(router,0u,midi::MidiEvent::MakeNoteOn(202u,1u,62u,100u),boundary+2);
    midi::MidiRouterCaptureTestAccess::Push(router,0u,midi::MidiEvent::MakeNoteOff(203u,1u,62u),boundary+3);
    router.PumpMidi({station},220u,config,params);
    auto learned = router.TriggerLearnObservation(); ASSERT_TRUE(learned->Binding);
    EXPECT_EQ(61u, learned->Binding->Press.Code); EXPECT_EQ(0u, learned->Binding->Press.State);
    EXPECT_EQ(1u, learned->Binding->Release.State); EXPECT_EQ(2u, learned->Binding->Press.Channel);
    EXPECT_EQ(1u, learned->PhysicalChannel); EXPECT_FALSE(learned->Armed);
    router.RequestTriggerLearn({10u,2u,1u}); router.PumpMidi({station},230u,config,params);
    const std::string serial = "serial-test";
    midi::MidiRouterCaptureTestAccess::Serial(router,serial,true);
    router.PumpSerial({station},config,params); ASSERT_TRUE(router.TriggerLearnObservation()->Binding);
    EXPECT_EQ(io::RigFile::BindingSource::Serial,router.TriggerLearnObservation()->Binding->Source);
    router.RequestTriggerLearn({10u,3u,1u}); router.PumpMidi({station},240u,config,params);
    midi::MidiRouterCaptureTestAccess::Serial(router,serial,true); router.PumpSerial({station},config,params);
    EXPECT_FALSE(router.TriggerLearnObservation()->Binding); // Repeated reports cannot teach a fresh edge.
    midi::MidiRouterCaptureTestAccess::Serial(router,serial,false); router.PumpSerial({station},config,params);
    ASSERT_TRUE(router.TriggerLearnObservation()->Binding); EXPECT_EQ(0u,router.TriggerLearnObservation()->Binding->Press.State);
    router.RequestTriggerLearn({10u,4u,1u}); router.PumpMidi({station},250u,config,params);
    router.ObserveTriggerLearnKey(69u); EXPECT_FALSE(router.TriggerLearnObservation()->Binding);
    router.ObserveTriggerLearnKey(65u); ASSERT_TRUE(router.TriggerLearnObservation()->Binding);
    EXPECT_EQ(io::RigFile::KeyboardDevice,router.TriggerLearnObservation()->Binding->Device);
    router.RequestTriggerLearn({10u,0u,1u}); router.PumpMidi({station},260u,config,params);
    router.ObserveTriggerLearnKey(66u); EXPECT_FALSE(router.TriggerLearnObservation()->Binding);
    EXPECT_FALSE(router.TriggerLearnObservation()->Armed);
    actions::KeyAction ccLearn; ccLearn.KeyChar = 76u; ccLearn.KeyActionType = actions::KeyAction::KEY_DOWN;
    ccLearn.Modifiers = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_CTRL | base::Action::MODIFIER_SHIFT);
    router.HandleAutomationKey(ccLearn,{}, {},{}); EXPECT_FALSE(router.VstCcLearnActive());
    router.EndTriggerLearn(); EXPECT_FALSE(router.TriggerLearnObservation());
    router.HandleAutomationKey(ccLearn,{}, {},{}); ASSERT_TRUE(router.VstCcLearnActive());
    router.RequestTriggerLearn({11u,1u,1u}); router.PumpMidi({station},270u,config,params);
    EXPECT_FALSE(router.TriggerLearnObservation()->Error.empty()); EXPECT_FALSE(router.TriggerLearnObservation()->Armed);
    router.EndTriggerLearn(); router.HandleAutomationKey(ccLearn,{}, {},{}); EXPECT_FALSE(router.VstCcLearnActive());
    auto stop = TriggerPredelayStationTest::Start(48100u,0.0); stop.ActionType = actions::TriggerAction::TRIGGER_REC_END;
    stop.TargetId = started.TargetId; stop.SampleCount = 48000u; ASSERT_TRUE(station->OnAction(stop).IsEaten);
    router.PumpMidi({station},54000u,config,params); EXPECT_FALSE(take->MidiTriggerCapturePending());
    unsigned int attacks[3]{}, releases[3]{};
    for (const auto& loop : take->GetMidiLoops()) for (std::size_t i = 0u; i < loop->EventCount(); ++i)
    {
        midi::MidiEvent event; ASSERT_TRUE(loop->TryGetEvent(i,event));
        if (event.data1 >= 60u && event.data1 <= 62u)
        { if (event.IsNoteOn()) ++attacks[event.data1-60u]; if (event.IsNoteOff()) ++releases[event.data1-60u]; }
    }
    for (auto i = 0u; i < 3u; ++i) { EXPECT_EQ(1u,attacks[i]) << i; EXPECT_EQ(1u,releases[i]) << i; }
    for (const auto value : {0u,1u,127u})
    {
        const auto binding = io::TriggerBindingLearn::Midi("keys",midi::MidiEvent{0u,0xb0u,64u,static_cast<std::uint8_t>(value)});
        ASSERT_TRUE(binding); EXPECT_EQ(value != 0u,binding->Press.State); EXPECT_EQ(value == 0u,binding->Release.State);
    }
}

TEST(AudioLoopShift, HandleChangesAudioOnlyAndDoubleClickResetsAcceptedOffset)
{
	auto take = MakeTimingTestLoopTake("audio-shft");
	auto loop = MakeTimingLoop(1000ul, true);
	loop->SetBodyPlayIndex(300ul);
	take->AddLoop(loop); take->CommitChanges();
	take->SetMidiVisualPosition(300ul, 1000ul);
	auto station = std::make_shared<engine::Station>(engine::StationParams{}, engine::Station::GetMixerParams({100,100}, MergeMixBehaviourParams{}));
	station->AddTake(take); station->CommitChanges();
	std::vector<std::shared_ptr<engine::Station>> stations{station};
	graphics::CtrlHandleOverlay overlay;
	engine::Quantiser quantiser;
	quantiser.SetClock(std::make_shared<Timer>());
	ASSERT_TRUE(quantiser.TrySetMasterFromHover(loop,base::DEPTH_LOOP,stations,1000u,io::UserConfig{},false));
	engine::QuantiserController controller(overlay, quantiser, stations);
	engine::QuantisationInteractionContext context;
	context.SelectDepth = base::DEPTH_LOOP;
	context.CursorPos = {200,200}; context.ViewportSize = {800,600}; context.SampleRate = 1000u;
	context.SelectedAudioLoops = {loop}; context.LoopDepthHasAudioTarget = true;
	const engine::QuantiserController::ChildResolver resolve = [loop](const auto&) { return loop; };
	const auto now = Timer::GetTime();
	controller.OnCtrlModifierChanged(true, now, context, resolve);
	ASSERT_TRUE(overlay.ButtonCenter(0)); EXPECT_FALSE(overlay.ButtonCenter(1));
	float expected = 0; loop->SetBodyPlayIndex(250ul); ASSERT_EQ(1u, loop->ReadBlock(&expected,0,1u)); loop->SetBodyPlayIndex(300ul);
	actions::TouchAction press; press.SetActionTime(now); press.Position = *overlay.ButtonCenter(0);
	ASSERT_TRUE(controller.TryHandleTouchAction(press,1000u,true,context,resolve)->IsEaten);
	actions::TouchMoveAction move; move.Position = press.Position; move.Position.X += 50;
	ASSERT_TRUE(controller.TryHandleTouchMove(move,1000u)->IsEaten);
	EXPECT_EQ(50,station->MasterAudioReferenceOffsetSamps());
	auto laterStation = std::make_shared<engine::Station>(engine::StationParams{}, engine::Station::GetMixerParams({100,100}, MergeMixBehaviourParams{}));
	quantiser.InitialiseStationMidiGrid(laterStation);
	EXPECT_EQ(50,laterStation->MasterAudioReferenceOffsetSamps());
	EXPECT_EQ(50, loop->PlaybackOffsetSamps()); EXPECT_EQ(250ul, loop->PlaybackBodyIndex());
	EXPECT_EQ(300ul, loop->BodyPlayIndex()); EXPECT_EQ(300ul, take->MidiVisualPosition());
	EXPECT_EQ(0, take->MidiQuantisation().PhaseOffsetSamps);
	float actual = 0; ASSERT_EQ(1u, loop->ReadBlock(&actual,0,1u)); EXPECT_FLOAT_EQ(expected,actual);
	EXPECT_EQ(250u, take->MasterPhaseAtAudioBoundary(loop.get(),1000u));
	press.State = actions::TouchAction::TOUCH_UP; press.Position = move.Position;
	controller.TryHandleTouchAction(press,1000u,true,context,resolve);
	// A fresh click pair on SHFT uses the same OS time/distance criteria as sliders.
	press.State = actions::TouchAction::TOUCH_DOWN; press.Position = *overlay.ButtonCenter(0); press.SetActionTime(now + std::chrono::seconds(1));
	controller.TryHandleTouchAction(press,1000u,true,context,resolve);
	press.State = actions::TouchAction::TOUCH_UP; controller.TryHandleTouchAction(press,1000u,true,context,resolve);
	press.State = actions::TouchAction::TOUCH_DOWN; press.SetActionTime(now + std::chrono::milliseconds(1100));
	controller.TryHandleTouchAction(press,1000u,true,context,resolve);
	EXPECT_EQ(0,station->MasterAudioReferenceOffsetSamps());
	EXPECT_EQ(0,loop->PlaybackOffsetSamps()); EXPECT_EQ(300ul,loop->PlaybackBodyIndex());
}

TEST(AudioLoopShift, SignedOffsetsWrapByEachLengthAndSurviveTransportCorrections)
{
	for (const auto length : {777ul,1000ul,2000ul})
	{
		auto take = MakePlayingTimingTake("shift-wrap",length,100ul);
		auto loop = take->GetLoops().front();
		for (const auto offset : {0,17,-17,2000000017,-2000000017})
		{
			loop->SetPlaybackOffsetSamps(offset);
			EXPECT_EQ(ninjam::PositiveModulo(100ll-offset,length),loop->PlaybackBodyIndex());
			take->CaptureMappedSourceAnchors(5000);
			take->RestoreMappedSourceCoordinate(5100);
			EXPECT_EQ(ninjam::PositiveModulo(200ll-offset,length),loop->PlaybackBodyIndex());
			EXPECT_EQ(offset,loop->PlaybackOffsetSamps());
			loop->SetBodyPlayIndex(100ul); loop->InvalidateSceneAnchor();
		}
	}
}
