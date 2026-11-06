#include "gtest/gtest.h"
#include "engine/Loop.h"
#include "../../TestPaths.h"
#include "io/WavReadWriter.h"
#include "spectral/SpectralEnvelope.h"
#include "spectral/SpectralPlaybackSnapshot.h"
#include <array>
#include <cmath>
#include <chrono>
#include <iostream>
#include <string_view>

class LoopPlaybackRateTest : public ::testing::Test
{
protected:
    static std::unique_ptr<engine::Loop> Make(const std::vector<float>& body, unsigned int fade = 0)
    {
        engine::LoopParams params;
        params.FadeSamps = fade;
        audio::AudioMixerParams mixer;
        audio::WireMixBehaviourParams wire;
        wire.Channels = {0u};
        mixer.Behaviour = wire;
        auto loop = std::make_unique<engine::Loop>(params, mixer);
        loop->Record();
        std::vector<float> samples(constants::MaxLoopFadeSamps + body.size(), 0.0f);
        std::copy(body.begin(), body.end(), samples.begin() + constants::MaxLoopFadeSamps);
        base::AudioWriteRequest request;
        request.source = base::Audible::AUDIOSOURCE_ADC;
        for (unsigned int begin = 0; begin < samples.size();)
        {
            request.samples = samples.data() + begin;
            request.numSamps = (std::min)(constants::MaxBlockSize, static_cast<unsigned int>(samples.size() - begin));
            loop->OnBlockWrite(request, 0);
            loop->EndWrite(request.numSamps, true);
            loop->UpdateCapacity();
            begin += request.numSamps;
        }
        loop->Play(constants::MaxLoopFadeSamps, static_cast<unsigned long>(body.size()), false);
        return loop;
    }
    static audio::PlaybackRateBlock Rate(double rate, unsigned int frames)
    { return {rate, rate, 1.0, frames, 1u}; }
};

TEST_F(LoopPlaybackRateTest, FractionalReadsReplayAndCommitExactlyOnce)
{
    auto loop = Make({0.0f, 0.25f, 0.5f, 0.75f});
    loop->SetPlaybackRateBlockAtAudioBoundary(Rate(0.5, 3));
    std::array<float,3> first{}, second{};
    ASSERT_EQ(3u, loop->ReadBlock(first.data(), 0, 3));
    ASSERT_EQ(3u, loop->ReadBlock(second.data(), 0, 3));
    EXPECT_EQ(first, second);
    EXPECT_EQ(0ul, loop->BodyPlayIndex());
    EXPECT_FLOAT_EQ(0.0f, first[0]);
    EXPECT_FLOAT_EQ(0.125f, first[1]);
    EXPECT_FLOAT_EQ(0.25f, first[2]);
    loop->EndMultiPlay(3);
    EXPECT_EQ(1ul, loop->BodyPlayIndex());
    ASSERT_EQ(3u, loop->ReadBlock(first.data(), 0, 3));
    EXPECT_FLOAT_EQ(0.375f, first[0]);
    loop->EndMultiPlay(3);
    EXPECT_EQ(3ul, loop->BodyPlayIndex());
}

TEST_F(LoopPlaybackRateTest, EqualRecordingAndCurrentRateUsesExactOriginalSamples)
{
    const std::vector<float> body{0.0f, 0.25f, 0.5f, -0.75f};
    auto loop = Make(body);
    loop->SetPlaybackRateBlockAtAudioBoundary(Rate(0.5, 4), 0.5);
    std::array<float,4> output{};
    ASSERT_EQ(4u, loop->ReadBlock(output.data(), 0, 4));
    for (unsigned int i = 0; i < 4; ++i) EXPECT_EQ(body[i], output[i]);
    loop->EndMultiPlay(4);
    EXPECT_EQ(0ul, loop->BodyPlayIndex());
}

TEST_F(LoopPlaybackRateTest, ShortLoopLinearWrapPreservesDcThroughMultipleWraps)
{
    auto loop = Make({0.75f, 0.75f, 0.75f});
    loop->SetPlaybackRateBlockAtAudioBoundary(Rate(2.0, 16), 0.5);
    std::array<float,16> output{};
    ASSERT_EQ(16u, loop->ReadBlock(output.data(), 0, 16));
    for (const auto sample : output) EXPECT_NEAR(0.75f, sample, 1e-6f);
    loop->EndMultiPlay(16);
    EXPECT_EQ(1ul, loop->BodyPlayIndex());
}

TEST_F(LoopPlaybackRateTest, ShortLoopsUseLinearSamplesAcrossMultipleWrapsPerFrame)
{
    for (const auto length : {1u, 2u, 3u})
        for (const auto rate : {0.25, 1.0, 1.5, 3.7, 5.0})
    {
        SCOPED_TRACE(length);
        SCOPED_TRACE(rate);
        std::vector<float> body(length);
        for (unsigned int i = 0; i < length; ++i) body[i] = 0.25f + i * 0.125f;
        auto loop = Make(body);
        loop->SetPlaybackRateBlockAtAudioBoundary(Rate(rate, 128));
        std::array<float,128> output{};
        ASSERT_EQ(output.size(), loop->ReadBlock(output.data(), 0, 128));
        for (unsigned int i = 0; i < output.size(); ++i)
        {
            const auto position = std::fmod(i * rate, static_cast<double>(length));
            const auto first = static_cast<unsigned int>(position);
            const auto fraction = position - first;
            const auto expected = body[first] * (1.0 - fraction) + body[(first + 1u) % length] * fraction;
            EXPECT_NEAR(expected, output[i], 1e-6) << i;
        }
        loop->EndMultiPlay(128);
        EXPECT_NEAR(std::fmod(128 * rate, static_cast<double>(length)), loop->FractionalBodyPlayIndex(), 1e-10);
    }
}

TEST_F(LoopPlaybackRateTest, UnityKeepsUnequalLoopPhasesExactOverManyCallbacks)
{
    for (const auto recordingRate : {0.5, 1.0, 2.0})
    {
        SCOPED_TRACE(recordingRate);
        auto shortLoop = Make(std::vector<float>(97, 0.25f));
        auto doubleLoop = Make(std::vector<float>(194, 0.25f));
        auto tripleLoop = Make(std::vector<float>(291, 0.25f));
        for (auto* loop : {shortLoop.get(), doubleLoop.get(), tripleLoop.get()}) loop->SetBodyPlayIndex(11);
        std::uint64_t total = 11;
        for (unsigned int block = 0; block < 1024; ++block)
        {
            const auto frames = block % 2u ? 4096u : 127u;
            total += frames;
            for (auto* loop : {shortLoop.get(), doubleLoop.get(), tripleLoop.get()})
            {
                loop->SetPlaybackRateBlockAtAudioBoundary(Rate(recordingRate, frames), recordingRate);
                loop->EndMultiPlay(frames);
                EXPECT_EQ(total % loop->LoopLength(), loop->BodyPlayIndex());
                EXPECT_DOUBLE_EQ(static_cast<double>(loop->BodyPlayIndex()), loop->FractionalBodyPlayIndex());
            }
        }
    }
}

TEST_F(LoopPlaybackRateTest, CallbackPartitionsPreserveFractionalSourcePosition)
{
    const std::vector<float> body{0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.5f, -0.5f};
    auto one = Make(body), split = Make(body);
    one->SetPlaybackRateBlockAtAudioBoundary(Rate(0.5, 101), 2.0);
    one->EndMultiPlay(101);
    for (const auto frames : {3u, 17u, 1u, 80u})
    {
        split->SetPlaybackRateBlockAtAudioBoundary(Rate(0.5, frames), 2.0);
        split->EndMultiPlay(frames);
    }
    EXPECT_EQ(one->BodyPlayIndex(), split->BodyPlayIndex());
    float first = 0.0f, second = 0.0f;
    ASSERT_EQ(1u, one->ReadBlock(&first, 0, 1));
    ASSERT_EQ(1u, split->ReadBlock(&second, 0, 1));
    EXPECT_FLOAT_EQ(first, second);
}

TEST_F(LoopPlaybackRateTest, CaptureNormalizesDeviceFramesIntoSourceTime)
{
    engine::LoopParams params;
    params.FadeSamps = 0;
    audio::AudioMixerParams mixer;
    audio::WireMixBehaviourParams wire;
    wire.Channels = {0u}; mixer.Behaviour = wire;
    engine::Loop loop(params, mixer);
    loop.Record();
    loop.ConfigureTriggerCapture(constants::MaxLoopFadeSamps, true, false);
    ASSERT_TRUE(loop.SetTriggerCaptureLength(4));
    audio::PlaybackRateHistory history(4);
    ASSERT_TRUE(history.Append(0, Rate(0.5, 384)));
    loop.SetCaptureRateHistoryAtAudioBoundary(&history, 64, {32, 0.0}, 1.0);
    std::array<float,512> physicalSamples{};
    for (unsigned int i = 0; i < 384; ++i) physicalSamples[i] = static_cast<float>((static_cast<int>(i) - 64) * 0.1);
    audio::InputHistoryChannel physicalChannel{physicalSamples.data(), 512, 384};
    audio::InputHistoryView inputHistory;
    inputHistory.Channels = &physicalChannel; inputHistory.ChannelCount = 1;
    inputHistory.ValidCount = 384; inputHistory.IngressHead = 384;
    loop.SetCaptureInputHistoryAtAudioBoundary(inputHistory, 0);
    std::array<float,8> input{0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f};
    base::AudioWriteRequest request;
    request.samples = input.data(); request.numSamps = 8;
    loop.OnBlockWrite(request, 0);
    loop.EndWrite(8, true);
    ASSERT_TRUE(loop.TriggerCaptureCoverageValid());
    ASSERT_TRUE(loop.TriggerCaptureComplete());
    loop.Play(constants::MaxLoopFadeSamps, 4, true);
    loop.EndRecording();
    std::array<float,4> output{};
    ASSERT_EQ(4u, loop.ReadBlock(output.data(), 0, 4));
    // The selected source grid samples this physical ramp at every second frame.
    EXPECT_NEAR(0.0f, output[0], 1e-5);
    EXPECT_NEAR(0.2f, output[1], 1e-5);
    EXPECT_NEAR(0.4f, output[2], 1e-5);
    EXPECT_NEAR(0.6f, output[3], 1e-5);
}

TEST_F(LoopPlaybackRateTest, InheritedShiftKeepsOneSignedRouteAcrossUnequalChildren)
{
    auto shortLoop = Make({0.25f, 0.25f, 0.25f});
    auto longLoop = Make({0.25f, 0.25f, 0.25f, 0.25f, 0.25f});
    shortLoop->SetPlaybackRateBlockAtAudioBoundary(Rate(0.5, 64), 2.0);
    longLoop->SetPlaybackRateBlockAtAudioBoundary(Rate(0.5, 64), 2.0);
    shortLoop->SetInheritedPlaybackOffsetSamps(20);
    longLoop->SetInheritedPlaybackOffsetSamps(20);
    double previous = 0.0;
    for (unsigned int block = 0; block < 400; ++block)
    {
        shortLoop->SetPlaybackRateBlockAtAudioBoundary(Rate(0.5, 64), 2.0);
        longLoop->SetPlaybackRateBlockAtAudioBoundary(Rate(0.5, 64), 2.0);
        shortLoop->EndMultiPlay(64);
        longLoop->EndMultiPlay(64);
        const auto offset = shortLoop->AppliedPlaybackOffsetSamps();
        EXPECT_DOUBLE_EQ(offset, longLoop->AppliedPlaybackOffsetSamps());
        EXPECT_GE(offset, previous);
        EXPECT_LE(offset - previous, 64 * 0.25 * 0.25 + 1e-12);
        previous = offset;
    }
    EXPECT_DOUBLE_EQ(20.0, previous);
}

TEST_F(LoopPlaybackRateTest, QuarterRateCapturePreservesSelectedPhysicalSamples)
{
    engine::LoopParams params; params.FadeSamps = 0;
    audio::AudioMixerParams mixer;
    audio::WireMixBehaviourParams wire; wire.Channels = {0u}; mixer.Behaviour = wire;
    engine::Loop loop(params, mixer);
    loop.Record();
    loop.ConfigureTriggerCapture(constants::MaxLoopFadeSamps, true, false);
    ASSERT_TRUE(loop.SetTriggerCaptureLength(64));
    audio::PlaybackRateHistory history(2);
    ASSERT_TRUE(history.Append(0, Rate(0.5, 512)));
    loop.SetCaptureRateHistoryAtAudioBoundary(&history, 64, {32, 0.0}, 2.0);
    std::array<float,512> samples{};
    for (unsigned int i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.4 * i));
    audio::InputHistoryChannel channel{samples.data(), 512, 0};
    audio::InputHistoryView view;
    view.Channels = &channel; view.ChannelCount = 1;
    view.ValidCount = 512; view.IngressHead = 512;
    loop.SetCaptureInputHistoryAtAudioBoundary(view, 0);
    base::AudioWriteRequest request;
    request.samples = samples.data() + 64; request.numSamps = 256;
    loop.OnBlockWrite(request, 0); loop.EndWrite(256, true);
    ASSERT_TRUE(loop.TriggerCaptureCoverageValid());
    ASSERT_TRUE(loop.TriggerCaptureComplete());
    loop.Play(constants::MaxLoopFadeSamps, 64, true); loop.EndRecording();
    std::array<float,64> output{};
    ASSERT_EQ(64u, loop.ReadBlock(output.data(), 0, 64));
    for (unsigned int i = 0; i < output.size(); ++i)
        EXPECT_FLOAT_EQ(samples[64u + 4u * i], output[i]) << i;
}

TEST_F(LoopPlaybackRateTest, CaptureWaitsOnlyForMissingLinearEndpointAndRetainsEnvelope)
{
    engine::LoopParams params; params.FadeSamps = 0;
    audio::AudioMixerParams mixer;
    audio::WireMixBehaviourParams wire; wire.Channels = {0u}; mixer.Behaviour = wire;
    engine::Loop loop(params, mixer);
    loop.Record(); loop.ConfigureTriggerCapture(constants::MaxLoopFadeSamps, true, false);
    ASSERT_TRUE(loop.SetTriggerCaptureLength(4));
    audio::PlaybackRateHistory history(2);
    ASSERT_TRUE(history.Append(0, Rate(0.75, 256)));
    std::array<float,256> samples{}; samples.fill(0.25f);
    audio::InputHistoryChannel channel{samples.data(), 256, 67};
    audio::InputHistoryView view;
    view.Channels = &channel; view.ChannelCount = 1; view.ValidCount = 67; view.IngressHead = 67;
    loop.SetCaptureRateHistoryAtAudioBoundary(&history, 64, {48, 0.0}, 1.0);
    loop.SetCaptureInputHistoryAtAudioBoundary(view, 0);
    base::AudioWriteRequest request; request.samples = samples.data(); request.numSamps = 8;
    request.fadeNew = 0.5f;
    loop.OnBlockWrite(request, 0); loop.EndWrite(8, true);
    EXPECT_TRUE(loop.TriggerCaptureCoverageValid());
    EXPECT_FALSE(loop.TriggerCaptureComplete());
    channel.WriteHead = 72; view.ValidCount = 72; view.IngressHead = 72;
    loop.SetCaptureRateHistoryAtAudioBoundary(&history, 72, {48, 0.0}, 1.0);
    loop.SetCaptureInputHistoryAtAudioBoundary(view, 0); // no ADC request after punch-out
    loop.EndWrite(8, true);
    EXPECT_TRUE(loop.TriggerCaptureCoverageValid());
    EXPECT_TRUE(loop.TriggerCaptureComplete());
    loop.Play(constants::MaxLoopFadeSamps, 4, true); loop.EndRecording();
    std::array<float,4> output{};
    ASSERT_EQ(4u, loop.ReadBlock(output.data(), 0, 4));
    for (const auto sample : output) EXPECT_NEAR(0.125f, sample, 1e-6);
}

// Opt-in diagnostic for both Debug callback headroom and Release throughput.
// Ordinary unit runs omit machine timings; these are measurements, not thresholds.
TEST_F(LoopPlaybackRateTest, DISABLED_FixedFilterCpuBenchmark)
{
    constexpr unsigned int frames = 256;
#ifdef _DEBUG
    constexpr unsigned int passes = 16, warmup = 4;
    constexpr const char* configuration = "Debug";
#else
    constexpr unsigned int passes = 64, warmup = 8;
    constexpr const char* configuration = "Release";
#endif
    std::vector<float> body(1000);
    for (unsigned int i = 0; i < body.size(); ++i)
        body[i] = static_cast<float>(std::sin(i * 0.071));
    struct Case { double Speed; const char* Material; };
    for (const auto scenario : {Case{1.0, "raw"}, Case{0.5, "raw"}, Case{1.01, "raw"},
        Case{1.5, "raw"}, Case{2.0, "raw"}, Case{3.7, "raw"}, Case{4.0, "raw"},
        Case{5.0, "raw"}, Case{0.0, "raw"}, Case{5.0, "64-source seam16"}, Case{3.7, "spectral"}, Case{3.7, "spectral fade"}})
        for (const auto voices : {1u, 32u})
    {
        const auto speed = scenario.Speed;
        const bool shortSeam = std::string_view(scenario.Material) == "64-source seam16";
        const bool spectralFade = std::string_view(scenario.Material) == "spectral fade";
        const bool hasSpectral = spectralFade || std::string_view(scenario.Material) == "spectral";
        const auto block = speed == 0.0 ? audio::PlaybackRateBlock{1.0, 2.0,
            1.0 / -std::expm1(-1.0 / 480.0), frames, 1u} : Rate(speed, frames);
        std::vector<std::unique_ptr<engine::Loop>> loops;
        for (unsigned int voice = 0; voice < voices; ++voice)
        {
            loops.push_back(Make(shortSeam ? std::vector<float>(body.begin(), body.begin() + 64) : body,
                shortSeam ? 16u : 0u));
            if (hasSpectral)
            {
                auto& loop = loops.back();
                std::string reason;
                const auto capture = loop->RequestSpectralSource(reason);
                ASSERT_TRUE(capture) << reason;
                capture->Prepare();
                for (unsigned int i = 0; i < 100 && capture->Status() == spectral::SpectralSourceCapture::State::Pending; ++i)
                    loop->EndMultiPlay(64);
                const auto source = capture->Source();
                ASSERT_TRUE(source);
                auto pcm = std::make_shared<spectral::SpectralPcm>();
                pcm->Source = source;
                pcm->ProcessedSamples = std::make_shared<const std::vector<float>>(source->Samples);
                const auto envelope = spectral::SpectralEnvelope::FromGains(std::vector<float>(body.size(), 0.75f), reason);
                ASSERT_TRUE(envelope) << reason;
                const auto playback = spectral::SpectralPlaybackSnapshot::Create(pcm, envelope, 1u, 1u, 1u, reason);
                ASSERT_TRUE(playback) << reason;
                ASSERT_TRUE(loop->PublishSpectralPlayback(playback, reason)) << reason;
                loop->EndMultiPlay(0);
                if (!spectralFade) loop->EndMultiPlay(static_cast<unsigned int>(body.size()));
            }
            loops.back()->SetPlaybackRateBlockAtAudioBoundary(block);
        }
        std::array<float, frames> output{};
        std::array<double, passes> durations{};
        for (unsigned int pass = 0; pass < passes + warmup; ++pass)
        {
            const auto started = std::chrono::steady_clock::now();
            for (const auto& loop : loops)
            {
                // Replay the changing block to measure sustained rate smoothing.
                loop->SetPlaybackRateBlockAtAudioBoundary(block);
                ASSERT_EQ(frames, loop->ReadBlock(output.data(), 0, frames));
                // Hold the transition at its block head to stress two active
                // material endpoints on every measured read, not just warmup.
                loop->EndMultiPlay(spectralFade ? 0u : frames);
            }
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (pass >= warmup) durations[pass - warmup] = ms;
        }
        std::sort(durations.begin(), durations.end());
        std::cout << "Varispeed " << configuration << " rate=" << (speed == 0.0 ? "1-to-2 ramp" : std::to_string(speed)) << " material=" << scenario.Material << ": " << voices << " mono sources, " << frames
            << " frames, median=" << durations[passes / 2] << "ms p95=" << durations[(passes - 1) * 95 / 100] << "ms max=" << durations.back()
            << "ms; 48kHz callback deadline=" << frames / 48.0 << "ms\n";
    }
}

TEST_F(LoopPlaybackRateTest, WetResidualUsesLinearCaptureAndOriginalSourceIsCopiedOnce)
{
    auto source = Make(std::vector<float>(256, 0.25f));
    engine::LoopParams params; params.FadeSamps = 0;
    audio::AudioMixerParams mixer;
    audio::WireMixBehaviourParams wire; wire.Channels = {0u}; mixer.Behaviour = wire;
    engine::Loop loop(params, mixer);
    loop.Overdub(); loop.ConfigureTriggerCapture(constants::MaxLoopFadeSamps, false, true);
    ASSERT_TRUE(loop.SetTriggerCaptureLength(64));
    audio::PlaybackRateHistory history(2);
    ASSERT_TRUE(history.Append(0, Rate(0.75, 512)));
    loop.SetCaptureRateHistoryAtAudioBoundary(&history, 64, {48, 0.0}, 2.0);
    loop.SetCaptureSourceAtAudioBoundary(source.get(), 0.0);
    std::array<float,512> samples{};
    for (unsigned int i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.4 * i));
    base::AudioWriteRequest request;
    request.source = base::Audible::AUDIOSOURCE_BOUNCE;
    request.samples = samples.data() + 64; request.numSamps = 256;
    loop.OnBlockWrite(request, 0); loop.EndWrite(256, true);
    EXPECT_TRUE(loop.TriggerCaptureCoverageValid());
    ASSERT_TRUE(loop.TriggerCaptureComplete());
    loop.SetCaptureSourceAtAudioBoundary(nullptr, 0.0);
    loop.Play(constants::MaxLoopFadeSamps, 64, true); loop.EndRecording();
    std::array<float,64> output{};
    ASSERT_EQ(64u, loop.ReadBlock(output.data(), 0, 64));
    for (unsigned int i = 0; i < output.size(); ++i)
    {
        const auto physical = 64.0 + i * (2.0 / 0.75);
        const auto first = static_cast<unsigned int>(physical);
        const auto fraction = physical - first;
        const auto residual = samples[first] * (1.0 - fraction) + samples[first + 1u] * fraction;
        EXPECT_NEAR(0.25 + residual, output[i], 1e-6) << i;
    }
}

TEST_F(LoopPlaybackRateTest, FractionalPlaybackMatchesLinearReferenceAtEveryAdmittedSpeed)
{
    for (const auto rate : {0.25, 0.5, 0.99, 1.0, 1.001, 1.01, 1.5, 2.0, 3.7, 4.0, 5.0})
    {
        SCOPED_TRACE(rate);
        std::vector<float> body(1000);
        for (unsigned int i = 0; i < body.size(); ++i)
            body[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.499 * i));
        auto loop = Make(body);
        loop->SetPlaybackRateBlockAtAudioBoundary(Rate(rate, 1024));
        std::array<float,1024> output{};
        ASSERT_EQ(output.size(), loop->ReadBlock(output.data(), 0, 1024));
        for (unsigned int i = 0; i < output.size(); ++i)
        {
            const auto position = std::fmod(i * rate, static_cast<double>(body.size()));
            const auto first = static_cast<unsigned int>(position);
            const auto fraction = position - first;
            const auto expected = body[first] * (1.0 - fraction) + body[(first + 1u) % body.size()] * fraction;
            EXPECT_NEAR(expected, output[i], 1e-6) << i;
        }
    }
}

TEST_F(LoopPlaybackRateTest, RateRampStartsAtExistingPositionAndReplaysWithoutCursorMutation)
{
    std::vector<float> body(1000);
    for (unsigned int i = 0; i < body.size(); ++i)
        body[i] = static_cast<float>(std::cos(i * 0.071));
    auto loop = Make(body);
    loop->SetBodyPlayIndex(37);
    constexpr double damping = 480.0;
    loop->SetPlaybackRateBlockAtAudioBoundary({1.0, 2.0, damping, 256, 1u});
    std::array<float,256> first{}, repeated{};
    ASSERT_EQ(first.size(), loop->ReadBlock(first.data(), 0, 256));
    ASSERT_EQ(repeated.size(), loop->ReadBlock(repeated.data(), 0, 256));
    EXPECT_EQ(first, repeated);
    EXPECT_FLOAT_EQ(body[37], first[0]);
    EXPECT_EQ(37ul, loop->BodyPlayIndex());
    double position = 37.0, rate = 1.0;
    for (unsigned int i = 0; i < first.size(); ++i)
    {
        const auto index = static_cast<unsigned int>(position);
        const auto fraction = position - index;
        EXPECT_NEAR(body[index] * (1.0 - fraction) + body[index + 1u] * fraction, first[i], 1e-6) << i;
        rate += (2.0 - rate) / damping;
        position += rate;
    }
    loop->EndMultiPlay(256);
    EXPECT_NEAR(position, loop->FractionalBodyPlayIndex(), 1e-10);
}

TEST_F(LoopPlaybackRateTest, LinearPlaybackPreservesLowFrequencyLevel)
{
    for (const auto rate : {1.01, 2.0, 4.0})
    {
        SCOPED_TRACE(rate);
        std::vector<float> body(10000);
        for (unsigned int i = 0; i < body.size(); ++i)
            body[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.01 * i));
        auto loop = Make(body);
        loop->SetPlaybackRateBlockAtAudioBoundary(Rate(rate, 1000));
        std::array<float,1000> output{};
        ASSERT_EQ(1000u, loop->ReadBlock(output.data(), 0, 1000));
        double difference = 0.0;
        for (unsigned int i = 0; i < output.size(); ++i)
        {
            const auto reference = std::sin(2.0 * 3.14159265358979323846 * 0.01 * rate * i);
            difference += (output[i] - reference) * (output[i] - reference);
        }
        EXPECT_LT(std::sqrt(difference / output.size()), 1e-3);
    }
}

TEST_F(LoopPlaybackRateTest, InheritedOffsetTransferPreservesColdAndAlreadyAudiblePositions)
{
    std::vector<float> body(17);
    for (unsigned int i = 0; i < body.size(); ++i) body[i] = static_cast<float>(i) / 20.0f;
    for (const bool warm : {false, true})
    {
        SCOPED_TRACE(warm);
        auto loop = Make(body);
        loop->SetPlaybackOffsetSamps(2);
        loop->SetInheritedPlaybackOffsetSamps(7);
        float before = body[8];
        if (warm) ASSERT_EQ(1u, loop->ReadBlock(&before, 0, 1));
        loop->AdoptInheritedPlaybackOffsetAtAudioBoundary();
        float after = 0.0f;
        ASSERT_EQ(1u, loop->ReadBlock(&after, 0, 1));
        EXPECT_FLOAT_EQ(before, after);
        EXPECT_EQ(10ul, loop->BodyPlayIndex());
        EXPECT_EQ(2, loop->PlaybackOffsetSamps());
        EXPECT_EQ(2, loop->TotalPlaybackOffsetSamps());
        // Repeating adoption with zero inherited metadata is idempotent.
        loop->AdoptInheritedPlaybackOffsetAtAudioBoundary();
        ASSERT_EQ(1u, loop->ReadBlock(&after, 0, 1));
        EXPECT_FLOAT_EQ(before, after);
    }
}

TEST_F(LoopPlaybackRateTest, LinearCaptureRateChangeAndDeferredRetriesShareOneTrajectory)
{
    const auto capture = [&](bool defer)
    {
        constexpr unsigned int physicalFrames = 20000, sourceFrames = 16384;
        engine::LoopParams params; params.FadeSamps = 0;
        audio::AudioMixerParams mixer;
        audio::WireMixBehaviourParams wire; wire.Channels = {0u}; mixer.Behaviour = wire;
        auto loop = std::make_unique<engine::Loop>(params, mixer);
        loop->Record(); loop->ConfigureTriggerCapture(constants::MaxLoopFadeSamps, true, false);
        EXPECT_TRUE(loop->SetTriggerCaptureLength(sourceFrames));
        audio::PlaybackRateHistory history(2);
        EXPECT_TRUE(history.Append(0, Rate(1.0, 128)));
        EXPECT_TRUE(history.Append(128, Rate(0.99, physicalFrames - 128)));
        std::vector<float> samples(physicalFrames);
        for (unsigned int i = 0; i < physicalFrames; ++i)
            samples[i] = static_cast<float>(std::cos(2.0 * 3.14159265358979323846 * 0.499 * i));
        audio::InputHistoryChannel channel{samples.data(), physicalFrames, 0};
        audio::InputHistoryView view;
        view.Channels = &channel; view.ChannelCount = 1;
        for (unsigned int start = 0; start < physicalFrames;)
        {
            const auto count = start == 0 ? 128u : (std::min)(4096u, physicalFrames - start);
            const auto available = defer ? start + count : physicalFrames;
            channel.WriteHead = available % physicalFrames;
            view.ValidCount = available; view.IngressHead = available;
            loop->SetCaptureRateHistoryAtAudioBoundary(&history, start, {}, 1.0);
            loop->SetCaptureInputHistoryAtAudioBoundary(view, 0);
            base::AudioWriteRequest request;
            request.samples = samples.data() + start; request.numSamps = count;
            loop->OnBlockWrite(request, 0); loop->EndWrite(count, true);
            EXPECT_TRUE(loop->TriggerCaptureCoverageValid());
            start += count;
        }
        EXPECT_TRUE(loop->TriggerCaptureComplete());
        std::vector<float> output;
        if (!loop->TriggerCaptureComplete()) return output;
        loop->Play(constants::MaxLoopFadeSamps, sourceFrames, true); loop->EndRecording();
        output.resize(sourceFrames);
        for (unsigned int begin = 0; begin < sourceFrames; begin += 4096)
        {
            loop->SetBodyPlayIndex(begin);
            EXPECT_EQ(4096u, loop->ReadBlock(output.data() + begin, 0, 4096));
        }
        return output;
    };
    const auto immediate = capture(false), deferred = capture(true);
    ASSERT_EQ(16384u, immediate.size());
    ASSERT_EQ(immediate.size(), deferred.size());
    for (unsigned int i = 0; i < immediate.size(); ++i)
        EXPECT_NEAR(immediate[i], deferred[i], 1e-6) << i;
    for (unsigned int i = 0; i < immediate.size(); ++i)
    {
        const auto physical = i < 128u ? static_cast<double>(i) : 128.0 + (i - 128u) / 0.99;
        const auto first = static_cast<unsigned int>(physical);
        const auto fraction = physical - first;
        const auto left = static_cast<float>(std::cos(2.0 * 3.14159265358979323846 * 0.499 * first));
        const auto right = static_cast<float>(std::cos(2.0 * 3.14159265358979323846 * 0.499 * (first + 1u)));
        EXPECT_NEAR(left * (1.0 - fraction) + right * fraction, immediate[i], 1e-6) << i;
    }
}

TEST_F(LoopPlaybackRateTest, SaveReloadPreservesAudiblePhaseDuringLocalAndSharedShift)
{
    struct DirectoryCleanup
    {
        std::filesystem::path Path;
        ~DirectoryCleanup() { std::error_code error; std::filesystem::remove_all(Path, error); }
    } directory{TestPaths::FixtureDirectory() / ("varispeed-shift-" + std::to_string(GetCurrentProcessId()))};
    ASSERT_TRUE(std::filesystem::create_directory(directory.Path));
    std::vector<float> body(1024);
    for (unsigned int i = 0; i < body.size(); ++i) body[i] = i / 2048.0f;
    for (const auto childShift : {-101, 101})
    {
        auto loop = Make(body);
        loop->SetBodyPlayIndex(512);
        loop->SetInheritedPlaybackOffsetSamps(37);
        loop->AdoptInheritedPlaybackOffsetAtAudioBoundary();
        loop->SetPlaybackRateBlockAtAudioBoundary(Rate(1.0, 16));
        loop->SetPlaybackOffsetSamps(childShift);
        loop->SetPlaybackRateBlockAtAudioBoundary(Rate(1.0, 16));
        loop->EndMultiPlay(16);
        ASSERT_GT(std::abs(loop->AppliedPlaybackOffsetSamps() - childShift), 1.0);
        const auto audible = loop->FractionalBodyPlayIndex() - loop->AppliedPlaybackOffsetSamps();
        auto saved = loop->ToJamFile("body.wav");
        // Session exporter stores the requested shared shift on each child;
        // the restored take adopts it before the first source trajectory.
        saved.BodyPlayIndex = (saved.BodyPlayIndex + 37u) % body.size();
        const auto samples = loop->ExportSamples();
        ASSERT_TRUE(io::WavReadWriter().Write((directory.Path / "body.wav").wstring(),
            samples, static_cast<unsigned int>(samples.size()), 48000));
        engine::LoopParams params; params.FadeSamps = 0;
        auto loaded = engine::Loop::FromFile(params, saved, directory.Path.wstring());
        ASSERT_TRUE(loaded);
        (*loaded)->SetInheritedPlaybackOffsetSamps(37);
        (*loaded)->AdoptInheritedPlaybackOffsetAtAudioBoundary();
        (*loaded)->SetPlaybackRateBlockAtAudioBoundary(Rate(1.0, 1));
        float sample = 0.0f;
        ASSERT_EQ(1u, (*loaded)->ReadBlock(&sample, 0, 1));
        // The file format stores integer source positions and PCM16 audio.
        EXPECT_NEAR(audible / 2048.0, sample, 1.0 / 2048.0 + 1.0 / 32768.0);
        EXPECT_EQ(childShift, (*loaded)->PlaybackOffsetSamps());
    }
}


TEST_F(LoopPlaybackRateTest, LinearEndpointsRespectBankBoundaryAndChildWrap)
{
    constexpr unsigned long length = 1000000;
    constexpr double radians = 2.0 * 3.14159265358979323846 / 1000.0;
    std::vector<float> samples(length);
    for (unsigned long i = 0; i < length; ++i) samples[i] = static_cast<float>(std::sin(i * radians));
    auto loop = Make(samples);
    loop->SetPlaybackRateBlockAtAudioBoundary(Rate(3.7, 128));
    for (const auto start : {audio::BufferBank::_BufferBankSize - constants::MaxLoopFadeSamps - 32ul, length - 32ul})
    {
        loop->SetBodyPlayIndex(start);
        std::array<float, 128> output{};
        ASSERT_EQ(output.size(), loop->ReadBlock(output.data(), 0, static_cast<unsigned int>(output.size())));
        for (unsigned int i = 0; i < output.size(); ++i)
            EXPECT_NEAR(std::sin((start + i * 3.7) * radians), output[i], 1e-4) << i;
    }
}

TEST_F(LoopPlaybackRateTest, LinearPlaybackUsesPublishedSpectralMaterial)
{
    auto loop = Make(std::vector<float>(1024, 0.5f));
    std::string reason;
    const auto capture = loop->RequestSpectralSource(reason);
    ASSERT_TRUE(capture) << reason;
    capture->Prepare();
    for (unsigned int i = 0; i < 100 && capture->Status() == spectral::SpectralSourceCapture::State::Pending; ++i)
        loop->EndMultiPlay(64);
    const auto source = capture->Source();
    ASSERT_TRUE(source);
    auto pcm = std::make_shared<spectral::SpectralPcm>();
    pcm->Source = source;
    pcm->ProcessedSamples = std::make_shared<const std::vector<float>>(source->Samples.size(), 1.0f);
    const auto envelope = spectral::SpectralEnvelope::FromGains(std::vector<float>(1024, 0.25f), reason);
    ASSERT_TRUE(envelope) << reason;
    const auto playback = spectral::SpectralPlaybackSnapshot::Create(pcm, envelope, 1u, 1u, 1u, reason);
    ASSERT_TRUE(playback) << reason;
    ASSERT_TRUE(loop->PublishSpectralPlayback(playback, reason)) << reason;
    loop->EndMultiPlay(0);
    loop->SetPlaybackRateBlockAtAudioBoundary(Rate(3.7, 256));
    std::array<float, 256> transition{}, split{};
    ASSERT_EQ(transition.size(), loop->ReadBlock(transition.data(), 0, 256));
    ASSERT_EQ(71u, loop->ReadBlock(split.data(), 0, 71));
    loop->EndMultiPlay(71);
    ASSERT_EQ(185u, loop->ReadBlock(split.data() + 71, 0, 185));
    const auto duration = static_cast<unsigned int>(std::clamp(
        static_cast<double>(source->Layout.SampleRate) * spectral::SpectralConfig::PlaybackTransitionSeconds,
        1.0, static_cast<double>(spectral::SpectralConfig::MaxPlaybackTransitionSamps)));
    for (unsigned int i = 0; i < transition.size(); ++i)
    {
        EXPECT_NEAR(0.5 - 0.25 * (std::min)(i, duration) / duration, transition[i], 1e-6) << i;
        EXPECT_NEAR(transition[i], split[i], 1e-6) << i;
    }
    loop->EndMultiPlay(1024);
    loop->SetPlaybackRateBlockAtAudioBoundary(Rate(3.7, 256));
    std::array<float, 256> output{};
    ASSERT_EQ(output.size(), loop->ReadBlock(output.data(), 0, static_cast<unsigned int>(output.size())));
    for (const auto sample : output) EXPECT_NEAR(0.25f, sample, 1e-6);
}

TEST_F(LoopPlaybackRateTest, LinearSeamMatchesCapturedUnityMaterialAcrossRampPartitions)
{
    std::vector<float> body(64);
    for (unsigned int i = 0; i < body.size(); ++i)
        body[i] = static_cast<float>(std::sin(i * 0.17));
    auto seam = Make(body, 16);
    std::vector<float> composed(body.size());
    ASSERT_EQ(composed.size(), seam->ReadBlock(composed.data(), 0, static_cast<unsigned int>(composed.size())));
    // Capturing the unity seam's material gives an independent source-space
    // reference. Interpolation must use both composed endpoints, including
    // samples on opposite sides of a child wrap.
    auto reference = Make(composed);
    auto partitioned = Make(body, 16);
    audio::PlaybackRate wholeRate, splitRate;
    ASSERT_TRUE(wholeRate.Configure(48000));
    ASSERT_TRUE(splitRate.Configure(48000));
    ASSERT_TRUE(wholeRate.SetTarget(2.0, 1u));
    ASSERT_TRUE(splitRate.SetTarget(2.0, 1u));
    for (auto* loop : {seam.get(), reference.get(), partitioned.get()})
    {
        float unitySample;
        ASSERT_EQ(1u, loop->ReadBlock(&unitySample, 0, 1));
        loop->SetBodyPlayIndex(47);
        loop->SetPlaybackRateBlockAtAudioBoundary(wholeRate.DescribeBlock(128));
    }
    std::array<float, 128> actual{}, expected{}, split{};
    ASSERT_EQ(actual.size(), seam->ReadBlock(actual.data(), 0, 128));
    ASSERT_EQ(expected.size(), reference->ReadBlock(expected.data(), 0, 128));
    ASSERT_EQ(37u, partitioned->ReadBlock(split.data(), 0, 37));
    partitioned->EndMultiPlay(37);
    ASSERT_TRUE(splitRate.Commit(splitRate.DescribeBlock(37)));
    partitioned->SetPlaybackRateBlockAtAudioBoundary(splitRate.DescribeBlock(91));
    ASSERT_EQ(91u, partitioned->ReadBlock(split.data() + 37, 0, 91));
    for (unsigned int i = 0; i < actual.size(); ++i)
    {
        EXPECT_NEAR(expected[i], actual[i], 1e-6) << i;
        EXPECT_NEAR(actual[i], split[i], 1e-6) << i;
    }
}
