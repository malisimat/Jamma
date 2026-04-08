
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "audio/ChannelMixer.h"
#include "engine/Trigger.h"

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
Samples({})
{
Samples = std::vector<float>(bufSize);

for (auto i = 0u; i < bufSize; i++)
{
Samples[i] = 0.0f;
}
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
Samples[destIndex] = (request.fadeNew * samp) + (request.fadeCurrent * Samples[destIndex]);
}
}
}
virtual void EndWrite(unsigned int numSamps, bool updateIndex)
{
if (updateIndex)
_writeIndex += numSamps;
}

bool IsFilled() { return _writeIndex >= Samples.size(); }
bool MatchesBuffer(const std::vector<float>& buf)
{
auto numSamps = buf.size();
for (auto samp = 0u; samp < numSamps; samp++)
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

class MockedMultiSink :
public MultiAudioSink
{
public:
MockedMultiSink(unsigned int bufSize)
{
_sink = std::make_shared<ChannelMixerMockedSink>(bufSize);
}

public:
virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override { return 1; };

bool IsFilled() { return _sink->IsFilled(); }
bool MatchesBuffer(const std::vector<float>& buf) { return _sink->MatchesBuffer(buf); }

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

TEST(ChannelMixer, PlayWrapsAroundAndMatches) {
auto bufSize = 100;
auto blockSize = 11;

ChannelMixerParams chanParams;
chanParams.InputBufferSize = bufSize;
chanParams.OutputBufferSize = bufSize;
chanParams.NumInputChannels = 1;
chanParams.NumOutputChannels = 1;

auto chanMixer = ChannelMixer(chanParams);
auto sink = std::make_shared<MockedMultiSink>(bufSize);

auto buf = std::vector<float>(bufSize);
for (auto samp = 0; samp < bufSize; samp++)
{
buf[samp] = ((rand() % 2000) - 1000) / 1001.0f;
}

chanMixer.FromAdc(buf.data(), 1, bufSize);

auto numBlocks = (bufSize * 2) / blockSize;

// Set initial play position to read from the beginning of recorded data
chanMixer.InitPlay(bufSize - blockSize, blockSize);

for (int i = 0; i < numBlocks; i++)
{
sink->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
chanMixer.WriteToSink(sink, blockSize);
chanMixer.Source()->EndMultiPlay(blockSize);
sink->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);
}

ASSERT_TRUE(sink->IsFilled());
ASSERT_TRUE(sink->MatchesBuffer(buf));
}

TEST(ChannelMixer, WriteWrapsAroundAndMatches) {
auto bufSize = 100;
auto blockSize = 11;

ChannelMixerParams chanParams;
chanParams.InputBufferSize = bufSize;
chanParams.OutputBufferSize = bufSize;
chanParams.NumInputChannels = 1;
chanParams.NumOutputChannels = 1;

auto chanMixer = ChannelMixer(chanParams);

// Generate random source data
auto sourceData = std::vector<float>(bufSize);
for (auto samp = 0; samp < bufSize; samp++)
{
sourceData[samp] = ((rand() % 2000) - 1000) / 1001.0f;
}

auto numBlocks = (bufSize * 2) / blockSize;
auto outputBuf = std::vector<float>();
unsigned int sourceOffset = 0;

for (int i = 0; i < numBlocks; i++)
{
chanMixer.Sink()->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);

// Write source data to DAC buffer using block API
auto sampsThisBlock = static_cast<unsigned int>(
std::min(blockSize, static_cast<int>(bufSize - sourceOffset)));
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
chanMixer.Sink()->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);

auto tempBuf = std::vector<float>(blockSize);
chanMixer.ToDac(tempBuf.data(), 1, blockSize);

for (auto v : tempBuf)
outputBuf.push_back(v);
}

// Verify all source data was written and read back correctly
for (auto samp = 0u; samp < static_cast<unsigned int>(bufSize); samp++)
{
ASSERT_EQ(sourceData[samp], outputBuf[samp])
<< "Mismatch at sample " << samp;
}
}
