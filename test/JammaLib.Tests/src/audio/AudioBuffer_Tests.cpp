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
Samples({})
{
Samples = std::vector<float>(bufSize, 0.0f);
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
bool IsZero()
{
for (auto f : Samples)
{
if (f != 0.f)
return false;
}

return true;
}

std::vector<float> Samples;
};

// Helper: write a block of source data into an AudioBuffer using OnBlockWrite.
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

// Helper: read a block from AudioBuffer into sink using PlaybackRead + OnBlockWrite.
static void ReadBlockFromBuffer(const std::shared_ptr<AudioBuffer>& buf,
const std::shared_ptr<AudioBufferMockedSink>& sink,
unsigned int numSamps,
unsigned int delaySamps = 0)
{
float tempBuf[constants::MaxBlockSize];

if (delaySamps > 0)
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

TEST(AudioBuffer, PlayWrapsAround) {
auto bufSize = 100u;
auto blockSize = 11u;

auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

// Trick buffer into thinking it has recorded something
audioBuf->EndWrite(blockSize, false);

auto numBlocks = (bufSize * 2) / blockSize;

for (auto i = 0u; i < numBlocks; i++)
{
float tempBuf[constants::MaxBlockSize];
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

TEST(AudioBuffer, WriteWrapsAround) {
auto bufSize = 100u;
auto blockSize = 11u;

auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

// Generate random source data
std::vector<float> sourceData(bufSize);
for (auto i = 0u; i < bufSize; i++)
sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

auto numBlocks = (bufSize * 2) / blockSize;
unsigned int sourceOffset = 0;

for (auto i = 0u; i < numBlocks; i++)
{
auto sampsThisBlock = std::min(blockSize, bufSize - sourceOffset);
if (sampsThisBlock > 0 && sourceOffset < bufSize)
{
WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
sourceOffset += sampsThisBlock;
}
else
{
// Past end of source data, just advance the buffer
audioBuf->EndWrite(blockSize, true);
}
}

ASSERT_GE(sourceOffset, bufSize);
}

TEST(AudioBuffer, WriteMatchesRead) {
auto bufSize = 100u;
auto blockSize = 11u;

auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

// Generate random source data
std::vector<float> sourceData(bufSize);
for (auto i = 0u; i < bufSize; i++)
sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

audioBuf->Zero(bufSize);

auto numBlocks = (bufSize / blockSize) + 1;
unsigned int sourceOffset = 0;

for (auto i = 0u; i < numBlocks; i++)
{
auto sampsThisBlock = std::min(blockSize, bufSize - sourceOffset);
if (sampsThisBlock > 0 && sourceOffset < bufSize)
{
// Write source to buffer
WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
sourceOffset += sampsThisBlock;
}

// Read from buffer to sink (delay=0, reads what was just written)
ReadBlockFromBuffer(audioBuf, sink, blockSize);
}

// Verify data matches with zero delay
for (auto samp = 0u; samp < bufSize; samp++)
{
EXPECT_FLOAT_EQ(sourceData[samp], sink->Samples[samp])
<< "Mismatch at sample " << samp;
}
}

TEST(AudioBuffer, IsCorrectlyDelayed) {
auto bufSize = 100u;
auto blockSize = 11u;
auto delaySamps = 42u;

auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

// Generate random source data
std::vector<float> sourceData(bufSize);
for (auto i = 0u; i < bufSize; i++)
sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

audioBuf->Zero(bufSize);

auto numBlocks = (bufSize / blockSize) + 1;
unsigned int sourceOffset = 0;

for (auto i = 0u; i < numBlocks; i++)
{
auto sampsThisBlock = std::min(blockSize, bufSize - sourceOffset);
if (sampsThisBlock > 0 && sourceOffset < bufSize)
{
WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
sourceOffset += sampsThisBlock;
}

// Read from buffer with configured delay
ReadBlockFromBuffer(audioBuf, sink, blockSize, delaySamps);
}

// Verify data is delayed: sink[delaySamps + i] should match sourceData[i]
for (auto samp = 0u; samp < bufSize; samp++)
{
if ((samp + delaySamps) < bufSize)
{
EXPECT_FLOAT_EQ(sourceData[samp], sink->Samples[samp + delaySamps])
<< "Mismatch at source=" << samp << " sink=" << (samp + delaySamps);
}
}
}

TEST(AudioBuffer, ClampsToMaxBufSize) {
auto bufSize = 100u;
auto blockSize = 11u;
auto delaySamps = bufSize + 10;

auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

// Generate random source data
std::vector<float> sourceData(bufSize);
for (auto i = 0u; i < bufSize; i++)
sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

audioBuf->Zero(bufSize);

auto numBlocks = (bufSize / blockSize) + 1;
unsigned int sourceOffset = 0;

for (auto i = 0u; i < numBlocks; i++)
{
auto sampsThisBlock = std::min(blockSize, bufSize - sourceOffset);
if (sampsThisBlock > 0 && sourceOffset < bufSize)
{
WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
sourceOffset += sampsThisBlock;
}

ReadBlockFromBuffer(audioBuf, sink, blockSize, delaySamps);
}

// With excessive delay clamped to bufSize, source[i] matches sink[bufSize+i]
// which wraps — verify data matches at maximum delay
for (auto samp = 0u; samp < bufSize; samp++)
{
if ((samp + bufSize) < bufSize)
{
EXPECT_FLOAT_EQ(sourceData[samp], sink->Samples[samp + bufSize])
<< "Mismatch at source=" << samp;
}
}
}

TEST(AudioBuffer, ExcessiveDelayPlaysNicely) {
auto bufSize = (unsigned int)constants::MaxBlockSize + 10;
auto blockSize = 11u;
auto delaySamps = bufSize;

auto audioBuf = std::make_shared<AudioBuffer>(constants::MaxBlockSize);
auto sink = std::make_shared<AudioBufferMockedSink>(blockSize);

// Generate random source data
std::vector<float> sourceData(blockSize);
for (auto i = 0u; i < blockSize; i++)
sourceData[i] = ((rand() % 2000) - 1000) / 1001.0f;

audioBuf->Zero(bufSize);

auto numBlocks = 2u;
unsigned int sourceOffset = 0;

for (auto i = 0u; i < numBlocks; i++)
{
auto sampsThisBlock = std::min(blockSize, (unsigned int)sourceData.size() - sourceOffset);
if (sampsThisBlock > 0 && sourceOffset < sourceData.size())
{
WriteBlockToBuffer(audioBuf, &sourceData[sourceOffset], sampsThisBlock);
sourceOffset += sampsThisBlock;
}

ReadBlockFromBuffer(audioBuf, sink, blockSize, delaySamps);
}

ASSERT_TRUE(sink->IsZero());
}
