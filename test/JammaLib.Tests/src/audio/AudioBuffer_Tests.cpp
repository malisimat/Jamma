#include <algorithm>

#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "audio/AudioBuffer.h"

using resources::ResourceLib;
using audio::AudioBuffer;
using base::AudioSource;
using base::AudioSink;
using base::AudioSourceParams;
using base::AudioWriteRequest;

class AudioBufferMockedSink :
    public AudioSink
{
public:
    AudioBufferMockedSink(unsigned int bufSize) :
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
    bool IsZero() const
    {
        for (auto f : Samples)
        {
            if (f != 0.0f)
                return false;
        }

        return true;
    }

    std::vector<float> Samples;
};

static void WriteBlockToBuffer(const std::shared_ptr<AudioBuffer>& buf,
    const float* srcData,
    unsigned int numSamps)
{
    AudioWriteRequest request;
    request.samples = srcData;
    request.numSamps = numSamps;
    request.stride = 1;
    request.fadeCurrent = 0.0f;
    request.fadeNew = 1.0f;
    request.source = base::Audible::AUDIOSOURCE_ADC;
    buf->OnBlockWrite(request, 0);
    buf->EndWrite(numSamps, true);
}

static void ReadBlockFromBuffer(const std::shared_ptr<AudioBuffer>& buf,
    const std::shared_ptr<AudioBufferMockedSink>& sink,
    unsigned int numSamps,
    unsigned int delaySamps = 0)
{
    float tempBuf[constants::MaxBlockSize];

    // Match AudioBuffer playback semantics: read the block ending
    // numSamps before the current write index, with any extra delay applied.
    buf->Delay(delaySamps + numSamps);

    auto ptr = buf->PlaybackRead(tempBuf, numSamps);

    AudioWriteRequest request;
    request.samples = ptr;
    request.numSamps = numSamps;
    request.stride = 1;
    request.fadeCurrent = 0.0f;
    request.fadeNew = 1.0f;
    request.source = base::Audible::AUDIOSOURCE_ADC;
    sink->OnBlockWrite(request, 0);

    buf->EndPlay(numSamps);
    sink->EndWrite(numSamps, true);
}

TEST(AudioBuffer, PlayWrapsAround)
{
    const auto bufSize = 100u;
    const auto blockSize = 11u;

    auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
    auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

    audioBuf->EndWrite(blockSize, false);

    const auto numBlocks = (bufSize * 2u) / blockSize;

    for (auto i = 0u; i < numBlocks; i++)
    {
        float tempBuf[constants::MaxBlockSize];
        audioBuf->Delay(blockSize);
        auto ptr = audioBuf->PlaybackRead(tempBuf, blockSize);

        AudioWriteRequest request;
        request.samples = ptr;
        request.numSamps = blockSize;
        request.stride = 1;
        request.fadeCurrent = 0.0f;
        request.fadeNew = 1.0f;
        request.source = base::Audible::AUDIOSOURCE_ADC;
        sink->OnBlockWrite(request, 0);

        audioBuf->EndPlay(blockSize);
        sink->EndWrite(blockSize, true);
    }

    ASSERT_TRUE(sink->IsFilled());
}

TEST(AudioBuffer, WriteWrapsAround)
{
    const auto bufSize = 100u;
    const auto blockSize = 11u;

    auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

    std::vector<float> sourceData(bufSize);
    for (auto i = 0u; i < bufSize; i++)
        sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

    const auto numBlocks = (bufSize * 2u) / blockSize;
    auto sourceOffset = 0u;

    for (auto i = 0u; i < numBlocks; i++)
    {
        auto sampsThisBlock = (std::min)(blockSize, bufSize - sourceOffset);
        if (sampsThisBlock > 0 && sourceOffset < bufSize)
        {
            WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
            sourceOffset += sampsThisBlock;
        }
        else
        {
            audioBuf->EndWrite(blockSize, true);
        }
    }

    ASSERT_GE(sourceOffset, bufSize);
}

TEST(AudioBuffer, WriteMatchesRead)
{
    const auto bufSize = 100u;
    const auto blockSize = 11u;

    auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
    auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

    std::vector<float> sourceData(bufSize);
    for (auto i = 0u; i < bufSize; i++)
        sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

    audioBuf->Zero(bufSize);

    const auto numBlocks = (bufSize / blockSize) + 1u;
    auto sourceOffset = 0u;

    for (auto i = 0u; i < numBlocks; i++)
    {
        auto sampsThisBlock = (std::min)(blockSize, bufSize - sourceOffset);
        if (sampsThisBlock > 0 && sourceOffset < bufSize)
        {
            WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
            sourceOffset += sampsThisBlock;
        }

        ReadBlockFromBuffer(audioBuf, sink, blockSize);
    }

    for (auto samp = 0u; samp < bufSize; samp++)
    {
        EXPECT_FLOAT_EQ(sourceData[samp], sink->Samples[samp])
            << "Mismatch at sample " << samp;
    }
}

TEST(AudioBuffer, IsCorrectlyDelayed)
{
    const auto bufSize = 100u;
    const auto blockSize = 11u;
    const auto delaySamps = 42u;

    auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
    auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

    std::vector<float> sourceData(bufSize);
    for (auto i = 0u; i < bufSize; i++)
        sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

    audioBuf->Zero(bufSize);

    const auto numBlocks = (bufSize / blockSize) + 1u;
    auto sourceOffset = 0u;

    for (auto i = 0u; i < numBlocks; i++)
    {
        auto sampsThisBlock = (std::min)(blockSize, bufSize - sourceOffset);
        if (sampsThisBlock > 0 && sourceOffset < bufSize)
        {
            WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
            sourceOffset += sampsThisBlock;
        }

        ReadBlockFromBuffer(audioBuf, sink, blockSize, delaySamps);
    }

    for (auto samp = 0u; samp < bufSize; samp++)
    {
        if ((samp + delaySamps) < bufSize)
        {
            EXPECT_FLOAT_EQ(sourceData[samp], sink->Samples[samp + delaySamps])
                << "Mismatch at source=" << samp << " sink=" << (samp + delaySamps);
        }
    }
}

TEST(AudioBuffer, ClampsToMaxBufSize)
{
    const auto bufSize = 100u;
    const auto blockSize = 11u;
    const auto delaySamps = bufSize + 10u;

    auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
    auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

    std::vector<float> sourceData(bufSize);
    for (auto i = 0u; i < bufSize; i++)
        sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

    audioBuf->Zero(bufSize);

    const auto numBlocks = (bufSize / blockSize) + 1u;
    auto sourceOffset = 0u;

    for (auto i = 0u; i < numBlocks; i++)
    {
        auto sampsThisBlock = (std::min)(blockSize, bufSize - sourceOffset);
        if (sampsThisBlock > 0 && sourceOffset < bufSize)
        {
            WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
            sourceOffset += sampsThisBlock;
        }

        ReadBlockFromBuffer(audioBuf, sink, blockSize, delaySamps);
    }

    ASSERT_TRUE(sink->IsFilled());
}

TEST(AudioBuffer, ExcessiveDelayPlaysNicely)
{
    const auto bufSize = static_cast<unsigned int>(constants::MaxBlockSize) + 10u;
    const auto blockSize = 11u;
    const auto delaySamps = bufSize;

    auto audioBuf = std::make_shared<AudioBuffer>(constants::MaxBlockSize);
    auto sink = std::make_shared<AudioBufferMockedSink>(blockSize);

    std::vector<float> sourceData(blockSize);
    for (auto i = 0u; i < blockSize; i++)
        sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

    audioBuf->Zero(bufSize);

    const auto numBlocks = 2u;
    auto sourceOffset = 0u;

    for (auto i = 0u; i < numBlocks; i++)
    {
        auto sampsThisBlock = (std::min)(blockSize,
            static_cast<unsigned int>(sourceData.size()) - sourceOffset);
        if (sampsThisBlock > 0 && sourceOffset < sourceData.size())
        {
            WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
            sourceOffset += sampsThisBlock;
        }

        ReadBlockFromBuffer(audioBuf, sink, blockSize, delaySamps);
    }

    ASSERT_TRUE(sink->IsZero());
}
