#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "audio/ChannelMixer.h"
#include "engine/Trigger.h"
#include "audio/AudioMixer.h"

using resources::ResourceLib;
using audio::ChannelMixer;
using audio::ChannelMixerParams;
using base::AudioSource;
using base::AudioSink;
using base::MultiAudioSource;
using base::MultiAudioSink;
using base::AudioSourceParams;
using base::AudioWriteRequest;

class ChannelMixerMockedSink :
    public AudioSink
{
public:
    ChannelMixerMockedSink(unsigned int bufSize) :
        Samples(bufSize, 0.0f)
    {
    }

public:
    virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) override
    {
        for (auto i = 0u; i < request.numSamps; i++)
        {
            auto destIndex = _writeIndex + writeOffset + i;
            if (destIndex < Samples.size())
            {
                auto samp = request.samples[i * request.stride];
                Samples[destIndex] = (request.fadeNew * samp) +
                    (request.fadeCurrent * Samples[destIndex]);
            }
        }
    }

    virtual void EndWrite(unsigned int numSamps, bool updateIndex) override
    {
        if (updateIndex)
            _writeIndex += numSamps;
    }

    bool IsFilled() const { return _writeIndex >= Samples.size(); }
    bool MatchesBuffer(const std::vector<float>& buf) const
    {
        for (auto samp = 0u; samp < buf.size(); samp++)
        {
            if (samp >= Samples.size())
                return false;

            if (buf[samp] != Samples[samp])
                return false;
        }

        return true;
    }

    std::vector<float> Samples;
};

class MockMultiSink :
    public MultiAudioSink
{
public:
    MockMultiSink(unsigned int bufSize)
        : _sink(std::make_shared<ChannelMixerMockedSink>(bufSize))
    {
    }

public:
    virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
    {
        return 1;
    }

    bool IsFilled() const { return _sink->IsFilled(); }
    bool MatchesBuffer(const std::vector<float>& buf) const { return _sink->MatchesBuffer(buf); }

protected:
    virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
        base::Audible::AudioSourceType source) override
    {
        if (channel == 0)
            return _sink;

        return std::shared_ptr<AudioSink>();
    }

private:
    std::shared_ptr<ChannelMixerMockedSink> _sink;
};

class MultiChannelCaptureSink :
    public MultiAudioSink
{
public:
    MultiChannelCaptureSink(unsigned int numChannels, unsigned int bufSize)
        : _sinks()
    {
        for (auto channel = 0u; channel < numChannels; ++channel)
            _sinks.push_back(std::make_shared<ChannelMixerMockedSink>(bufSize));
    }

public:
    virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
    {
        return static_cast<unsigned int>(_sinks.size());
    }

    const std::vector<float>& Samples(unsigned int channel) const
    {
        return _sinks.at(channel)->Samples;
    }

protected:
    virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
        base::Audible::AudioSourceType source) override
    {
        if (channel < _sinks.size())
            return _sinks[channel];

        return std::shared_ptr<AudioSink>();
    }

private:
    std::vector<std::shared_ptr<ChannelMixerMockedSink>> _sinks;
};

TEST(AudioMixerMeter, PublishesPostGainWhileHiddenAndDecaysToSilence)
{
    audio::AudioMixerParams params;
    audio::WireMixBehaviourParams wire; wire.Channels = {0u}; params.Behaviour = wire;
    audio::AudioMixer mixer(params);
    mixer.SetUnmutedLevel(0.5); mixer.Offset(4096u);
    mixer.SetVuVisible(false);
    const float input[] = {-0.8f, 0.1f, 0.2f, -0.4f};
    auto sink = std::make_shared<MultiChannelCaptureSink>(1u, 64u);
    mixer.WriteBlock(sink, input, 4u);
    EXPECT_FLOAT_EQ(0.4f, mixer.VuPeakLevel());
    EXPECT_FLOAT_EQ(0.4f, mixer.VuLevel());
    EXPECT_FLOAT_EQ(0.4f, mixer.VuHoldLevel());
    EXPECT_FLOAT_EQ(-0.4f, sink->Samples(0u)[0]);
    const float silence[64]{};
    for (unsigned int block = 0u; block < 1000u; ++block) mixer.WriteBlock(sink, silence, 64u);
    EXPECT_FLOAT_EQ(0.0f, mixer.VuPeakLevel());
    EXPECT_FLOAT_EQ(0.0f, mixer.VuLevel());
    EXPECT_FLOAT_EQ(0.0f, mixer.VuHoldLevel());
}

TEST(AudioMixerMeter, DistinctChannelsAndMuteUseTheirOwnSmoothedSignal)
{
    audio::AudioMixerParams params;
    audio::WireMixBehaviourParams wire; wire.Channels = {0u}; params.Behaviour = wire;
    audio::AudioMixer first(params), second(params);
    const float loud[] = {0.8f, -0.4f}, quiet[] = {0.125f, -0.0625f};
    first.WriteBlock(nullptr, loud, 2u); second.WriteBlock(nullptr, quiet, 2u);
    EXPECT_FLOAT_EQ(0.8f, first.VuPeakLevel()); EXPECT_FLOAT_EQ(0.125f, second.VuPeakLevel());
    second.Mute(); second.Offset(4096u); second.WriteBlock(nullptr, quiet, 2u);
    EXPECT_FLOAT_EQ(0.0f, second.VuPeakLevel()); EXPECT_FLOAT_EQ(0.8f, first.VuPeakLevel());
}

TEST(CaptureSourceMute, SharedClaimsPreserveUserMuteAndStereoFade)
{
    auto claims = std::make_shared<audio::CaptureSourceMuteControl>();
    audio::WireMixBehaviourParams wire;
    wire.Channels = { 0u, 1u };
    audio::AudioMixerParams params;
    params.Behaviour = wire;
    audio::AudioMixer mixer(params);
    mixer.SetCaptureSourceMuteControl(claims);
    auto sink = std::make_shared<MultiChannelCaptureSink>(2u, 32u);
    const float input[] = { 1.f, 1.f, 1.f, 1.f };
    claims->Acquire();
    claims->Acquire();
    mixer.WriteBlock(sink, input, 4u);
    EXPECT_NEAR(mixer.Level(), std::pow(0.99, 4), 1e-14);
    sink->EndMultiWrite(4u, true, base::Audible::AUDIOSOURCE_MIXER);
    claims->Release();
    mixer.WriteBlock(sink, input, 4u);
    EXPECT_NEAR(mixer.Level(), std::pow(0.99, 8), 1e-14);
    sink->EndMultiWrite(4u, true, base::Audible::AUDIOSOURCE_MIXER);
    mixer.Mute();
    claims->Release();
    mixer.WriteBlock(sink, input, 4u);
    EXPECT_TRUE(mixer.IsMuted());
    EXPECT_NEAR(mixer.Level(), std::pow(0.99, 12), 1e-14);
    sink->EndMultiWrite(4u, true, base::Audible::AUDIOSOURCE_MIXER);
    mixer.SetUnmutedLevel(0.5);
    mixer.UnMute();
    const auto before = mixer.Level();
    mixer.WriteBlock(sink, input, 4u);
    EXPECT_NEAR(mixer.Level(), 0.5 + (before - 0.5) * std::pow(0.99, 4), 1e-14);
    EXPECT_EQ(sink->Samples(0u), sink->Samples(1u));
    EXPECT_FLOAT_EQ(sink->Samples(0u)[4], static_cast<float>(std::pow(0.99, 4)));
    EXPECT_FLOAT_EQ(sink->Samples(0u)[12], static_cast<float>(before));
}

TEST(CaptureSourceMute, ConcurrentIndependentOwnersCannotReleasePersistentClaim)
{
    audio::CaptureSourceMuteControl claims;
    claims.Acquire();
    std::thread jobOwner([&claims] {
        for (unsigned int i = 0; i < 10000; ++i) {
            claims.Acquire();
            claims.Release();
        }
    });
    for (unsigned int i = 0; i < 10000; ++i) {
        claims.Acquire();
        claims.Release();
        EXPECT_TRUE(claims.IsMuted());
    }
    jobOwner.join();
    claims.Release();
    EXPECT_FALSE(claims.IsMuted());
}

TEST(ChannelMixer, PlayWrapsAroundAndMatches)
{
    const auto bufSize = 100u;
    const auto blockSize = 11u;

    ChannelMixerParams chanParams;
    chanParams.InputBufferSize = bufSize;
    chanParams.OutputBufferSize = bufSize;
    chanParams.NumInputChannels = 1;
    chanParams.NumOutputChannels = 1;

    auto chanMixer = ChannelMixer(chanParams);
    auto sink = std::make_shared<MockMultiSink>(bufSize);

    auto buf = std::vector<float>(bufSize);
    for (auto samp = 0u; samp < bufSize; samp++)
        buf[samp] = ((rand() % 2000) - 1000) / 1001.0f;

    chanMixer.FromAdc(buf.data(), 1, bufSize);

    const auto numBlocks = (bufSize * 2u) / blockSize;
    chanMixer.InitPlay(bufSize - blockSize, blockSize);

    for (auto i = 0u; i < numBlocks; i++)
    {
        sink->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
        chanMixer.WriteToSink(sink, blockSize);
        chanMixer.Source()->EndMultiPlay(blockSize);
        sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);
    }

    ASSERT_TRUE(sink->IsFilled());
    ASSERT_TRUE(sink->MatchesBuffer(buf));
}

TEST(ChannelMixer, WriteWrapsAroundAndMatches)
{
    const auto bufSize = 100u;
    const auto blockSize = 11u;

    ChannelMixerParams chanParams;
    chanParams.InputBufferSize = bufSize;
    chanParams.OutputBufferSize = bufSize;
    chanParams.NumInputChannels = 1;
    chanParams.NumOutputChannels = 1;

    auto chanMixer = ChannelMixer(chanParams);

    auto sourceData = std::vector<float>(bufSize);
    for (auto samp = 0u; samp < bufSize; samp++)
        sourceData[samp] = ((rand() % 2000) - 1000) / 1001.0f;

    const auto numBlocks = (bufSize * 2u) / blockSize;
    auto outputBuf = std::vector<float>();
    auto sourceOffset = 0u;

    for (auto i = 0u; i < numBlocks; i++)
    {
        chanMixer.Sink()->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);

        auto sampsThisBlock = (std::min)(blockSize, bufSize - sourceOffset);
        if (sampsThisBlock > 0)
        {
            AudioWriteRequest request;
            request.samples = &sourceData[sourceOffset];
            request.numSamps = sampsThisBlock;
            request.stride = 1;
            request.fadeCurrent = 0.0f;
            request.fadeNew = 1.0f;
            request.source = base::Audible::AUDIOSOURCE_ADC;
            chanMixer.Sink()->OnBlockWriteChannel(0, request, 0);
            sourceOffset += sampsThisBlock;
        }

        auto tempBuf = std::vector<float>(blockSize);
        chanMixer.ToDac(tempBuf.data(), 1, blockSize);
        chanMixer.Sink()->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);

        for (auto v : tempBuf)
            outputBuf.push_back(v);
    }

    for (auto samp = 0u; samp < bufSize; samp++)
    {
        ASSERT_EQ(sourceData[samp], outputBuf[samp])
            << "Mismatch at sample " << samp;
    }
}

TEST(ChannelMixer, ShrinkRemovesChannelsAndExpandRestoresThem)
{
    const auto bufSize = 16u;
    const auto blockSize = 4u;

    ChannelMixerParams chanParams;
    chanParams.InputBufferSize = bufSize;
    chanParams.OutputBufferSize = bufSize;
    chanParams.NumInputChannels = 4u;
    chanParams.NumOutputChannels = 4u;

    auto chanMixer = ChannelMixer(chanParams);
    EXPECT_EQ(4u, chanMixer.Source()->NumOutputChannels(base::Audible::AUDIOSOURCE_ADC));
    EXPECT_EQ(4u, chanMixer.Sink()->NumInputChannels(base::Audible::AUDIOSOURCE_MIXER));

    chanParams.NumInputChannels = 2u;
    chanParams.NumOutputChannels = 2u;
    chanMixer.SetParams(chanParams);
    EXPECT_EQ(2u, chanMixer.Source()->NumOutputChannels(base::Audible::AUDIOSOURCE_ADC));
    EXPECT_EQ(2u, chanMixer.Sink()->NumInputChannels(base::Audible::AUDIOSOURCE_MIXER));

    chanParams.NumInputChannels = 3u;
    chanParams.NumOutputChannels = 3u;
    chanMixer.SetParams(chanParams);
    EXPECT_EQ(3u, chanMixer.Source()->NumOutputChannels(base::Audible::AUDIOSOURCE_ADC));
    EXPECT_EQ(3u, chanMixer.Sink()->NumInputChannels(base::Audible::AUDIOSOURCE_MIXER));

    std::vector<float> inBuf = {
        1.0f, 10.0f, 100.0f,
        2.0f, 20.0f, 200.0f,
        3.0f, 30.0f, 300.0f,
        4.0f, 40.0f, 400.0f,
    };
    auto sink = std::make_shared<MultiChannelCaptureSink>(3u, blockSize);

    chanMixer.FromAdc(inBuf.data(), 3u, blockSize);
    chanMixer.InitPlay(0u, blockSize);
    chanMixer.WriteToSink(sink, blockSize);
    chanMixer.Source()->EndMultiPlay(blockSize);
    sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);

    EXPECT_EQ(std::vector<float>({ 100.0f, 200.0f, 300.0f, 400.0f }), sink->Samples(2u));
}

TEST(ChannelMixerHistory, PhysicalCoordinatesRespectLatencyRetentionAndBothWraps)
{
    for (const auto latency : { 0u, 3u, 37u })
    for (const auto capacity : { 4u, 11u })
    {
        SCOPED_TRACE(::testing::Message() << "latency=" << latency << " capacity=" << capacity);
        ChannelMixerParams params;
        params.NumInputChannels = 2u; params.NumOutputChannels = 1u;
        params.InputBufferSize = capacity; params.OutputBufferSize = 4u;
        ChannelMixer mixer(params);
        constexpr std::uint64_t origin = 0xfffffff9ull;
        for (auto block = 0u; block < 8u; ++block)
        {
            std::array<float, 8> input{};
            for (auto i = 0u; i < 4u; ++i)
            {
                input[i * 2u] = static_cast<float>(block * 4u + i + 1u);
                input[i * 2u + 1u] = -10.0f * input[i * 2u];
            }
            const auto start = origin + block * 4u;
            mixer.FromAdc(input.data(), 2u, 4u);
            const auto history = mixer.ObserveInputHistory(start, 4u, latency, true);
            const auto valid = (std::min)(capacity, (block + 1u) * 4u);
            EXPECT_EQ(valid, history.ValidCount);
            for (auto age = 1u; age <= valid; ++age)
            {
                const auto physical = static_cast<std::uint32_t>(start + 4u - latency - age);
                float sample = 0.0f;
                ASSERT_TRUE(history.Read(0u, physical, sample));
                EXPECT_FLOAT_EQ(static_cast<float>((block + 1u) * 4u - age + 1u), sample);
                ASSERT_TRUE(history.Read(1u, physical, sample));
                EXPECT_FLOAT_EQ(-10.0f * static_cast<float>((block + 1u) * 4u - age + 1u), sample);
            }
            float unchanged = 123.0f;
            const auto physicalHead = static_cast<std::uint32_t>(start + 4u - latency);
            EXPECT_FALSE(history.Read(0u, physicalHead, unchanged)); // exclusive ingress head
            EXPECT_FALSE(history.Read(0u, physicalHead + 1u, unchanged)); // future
            EXPECT_FALSE(history.Read(0u, physicalHead - valid - 1u, unchanged));
            EXPECT_FALSE(history.Read(2u, physicalHead - 1u, unchanged));
            EXPECT_FLOAT_EQ(123.0f, unchanged);
        }
    }
}

TEST(ChannelMixerHistory, GapsAndStreamReplacementNeverRelabelOldSamples)
{
    ChannelMixerParams params;
    params.NumInputChannels = 1u; params.NumOutputChannels = 1u;
    params.InputBufferSize = 12u; params.OutputBufferSize = 4u;
    ChannelMixer mixer(params);
    std::array<float, 4> oldInput{ 1.0f, 2.0f, 3.0f, 4.0f };
    mixer.FromAdc(oldInput.data(), 1u, 4u);
    auto history = mixer.ObserveInputHistory(100u, 4u, 7u, true);
    EXPECT_EQ(4u, history.ValidCount);
    EXPECT_FALSE(mixer.ObserveInputHistory(104u, 4u, 7u, false).ContainsOrigin(100u));
    std::array<float, 4> resumed{ 11.0f, 12.0f, 13.0f, 14.0f };
    mixer.FromAdc(resumed.data(), 1u, 4u);
    history = mixer.ObserveInputHistory(108u, 4u, 7u, true);
    EXPECT_EQ(4u, history.ValidCount);
    float sample = 0.0f;
    ASSERT_TRUE(history.Read(0u, 108u - 7u, sample));
    EXPECT_FLOAT_EQ(11.0f, sample);
    EXPECT_FALSE(history.Read(0u, 107u - 7u, sample));

    // A discontinuous callback ruler is also a gap, even with non-null input.
    mixer.FromAdc(resumed.data(), 1u, 4u);
    history = mixer.ObserveInputHistory(120u, 4u, 7u, true);
    EXPECT_EQ(4u, history.ValidCount);
    EXPECT_FALSE(history.Read(0u, 119u - 7u, sample));

    mixer.SetParams(params);
    mixer.FromAdc(resumed.data(), 1u, 4u);
    history = mixer.ObserveInputHistory(200u, 4u, 7u, true);
    EXPECT_EQ(4u, history.ValidCount);
    EXPECT_FALSE(history.ContainsOrigin(100u));
    EXPECT_TRUE(history.ContainsOrigin(200u));
    EXPECT_FALSE(history.Read(0u, 199u - 7u, sample));
}

TEST(ChannelMixerHistory, MissingChannelCannotPublishStaleStereoCoverage)
{
    ChannelMixerParams params;
    params.NumInputChannels = 2u; params.NumOutputChannels = 1u;
    params.InputBufferSize = 12u; params.OutputBufferSize = 4u;
    ChannelMixer mixer(params);
    std::array<float, 8> stereo{ 1, 10, 2, 20, 3, 30, 4, 40 };
    mixer.FromAdc(stereo.data(), 2u, 4u);
    mixer.ObserveInputHistory(100u, 4u, 3u, true);
    std::array<float, 4> mono{ 5, 6, 7, 8 };
    mixer.FromAdc(mono.data(), 1u, 4u);
    const auto missing = mixer.ObserveInputHistory(104u, 4u, 3u, true);
    float sample = 0.0f;
    EXPECT_FALSE(missing.Read(1u, 104u - 3u, sample));
    mixer.FromAdc(stereo.data(), 2u, 4u);
    const auto resumed = mixer.ObserveInputHistory(108u, 4u, 3u, true);
    ASSERT_TRUE(resumed.Read(1u, 108u - 3u, sample));
    EXPECT_FLOAT_EQ(10.0f, sample);
    EXPECT_FALSE(resumed.Read(1u, 107u - 3u, sample));
}

TEST(ChannelMixerHistory, FreshOriginsRemainEligibleInLongRunningStream)
{
    ChannelMixerParams params;
    params.NumInputChannels = 1u; params.NumOutputChannels = 1u;
    params.InputBufferSize = 4u; params.OutputBufferSize = 4u;
    ChannelMixer mixer(params);
    std::array<float, 4> input{ 1, 2, 3, 4 };
    mixer.FromAdc(input.data(), 1u, 4u);
    mixer.ObserveInputHistory(100u, 4u, 3u, true);
    constexpr std::uint64_t lateBoundary = 0x180000064ull;
    mixer.FromAdc(input.data(), 1u, 4u);
    const auto history = mixer.ObserveInputHistory(lateBoundary, 4u, 3u, true);
    EXPECT_TRUE(history.ContainsOrigin(static_cast<std::uint32_t>(lateBoundary)));
    EXPECT_FALSE(history.ContainsOrigin(static_cast<std::uint32_t>(lateBoundary + 5u)));
    float sample = 0.0f;
    ASSERT_TRUE(history.Read(0u, static_cast<std::uint32_t>(lateBoundary - 3u), sample));
    EXPECT_FLOAT_EQ(1.0f, sample);
}
