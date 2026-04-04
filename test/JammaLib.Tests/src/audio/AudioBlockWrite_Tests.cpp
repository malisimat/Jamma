#include "gtest/gtest.h"
#include "audio/AudioBuffer.h"
#include "audio/ChannelMixer.h"

using audio::AudioBuffer;
using audio::ChannelMixer;
using audio::ChannelMixerParams;
using base::AudioSource;
using base::AudioSink;
using base::MultiAudioSink;
using base::AudioSourceParams;
using base::AudioWriteRequest;

// Test helper: a sink that stores samples written via OnBlockWrite
class BlockWriteMockedSink :
	public AudioSink
{
public:
	BlockWriteMockedSink(unsigned int bufSize) :
		Samples(bufSize, 0.0f),
		BlockWriteCount(0)
	{
	}

public:
	virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) override
	{
		BlockWriteCount++;
		for (auto i = 0u; i < request.numSamps; i++)
		{
			auto destIndex = _writeIndex + writeOffset + i;
			if (destIndex < Samples.size())
				Samples[destIndex] = (request.fadeNew * request.samples[i * request.stride]) +
					(request.fadeCurrent * Samples[destIndex]);
		}
	}
	virtual void EndWrite(unsigned int numSamps, bool updateIndex) override
	{
		if (updateIndex)
			_writeIndex += numSamps;
	}

	std::vector<float> Samples;
	unsigned int BlockWriteCount;
};

class BlockWriteMockedMultiSink :
	public MultiAudioSink
{
public:
	BlockWriteMockedMultiSink(unsigned int bufSize, unsigned int numChannels)
	{
		for (unsigned int i = 0; i < numChannels; i++)
			_sinks.push_back(std::make_shared<BlockWriteMockedSink>(bufSize));
	}

public:
	virtual unsigned int NumInputChannels(base::Audible::AudioSourceType source) const override
	{
		return (unsigned int)_sinks.size();
	}

	const std::shared_ptr<BlockWriteMockedSink> GetChannel(unsigned int channel) const
	{
		if (channel < _sinks.size())
			return _sinks[channel];
		return nullptr;
	}

protected:
	virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
		base::Audible::AudioSourceType source) override
	{
		if (channel < _sinks.size())
			return _sinks[channel];
		return nullptr;
	}

private:
	std::vector<std::shared_ptr<BlockWriteMockedSink>> _sinks;
};

// ------------------------------------------------------------------
// Test: Block write to AudioBuffer with contiguous data
// ------------------------------------------------------------------
TEST(AudioBlockWrite, ContiguousBlockWrite) {
	auto bufSize = 100u;
	auto numSamps = 50u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

	std::vector<float> source(numSamps);
	for (unsigned int i = 0; i < numSamps; i++)
		source[i] = (float)i * 0.02f;

	AudioWriteRequest request;
	request.samples = source.data();
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(request, 0);
	audioBuf->EndWrite(numSamps, true);

	for (unsigned int i = 0; i < numSamps; i++)
	{
		EXPECT_FLOAT_EQ((*audioBuf)[i], (float)i * 0.02f);
	}
}

// ------------------------------------------------------------------
// Test: Block write to AudioBuffer with interleaved (strided) data
// ------------------------------------------------------------------
TEST(AudioBlockWrite, StridedBlockWrite) {
	auto bufSize = 100u;
	auto numChannels = 2u;
	auto numSamps = 50u;

	std::vector<float> interleaved(numSamps * numChannels);
	for (unsigned int i = 0; i < numSamps; i++)
	{
		interleaved[i * numChannels + 0] = (float)i * 0.01f;
		interleaved[i * numChannels + 1] = (float)i * -0.01f;
	}

	// Write channel 0 using stride
	auto buf0 = std::make_shared<AudioBuffer>(bufSize);

	AudioWriteRequest request;
	request.samples = &interleaved[0];
	request.numSamps = numSamps;
	request.stride = numChannels;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	buf0->OnBlockWrite(request, 0);
	buf0->EndWrite(numSamps, true);

	// Write channel 1 using stride
	auto buf1 = std::make_shared<AudioBuffer>(bufSize);

	request.samples = &interleaved[1];
	buf1->OnBlockWrite(request, 0);
	buf1->EndWrite(numSamps, true);

	for (unsigned int i = 0; i < numSamps; i++)
	{
		EXPECT_FLOAT_EQ((*buf0)[i], (float)i * 0.01f);
	}

	for (unsigned int i = 0; i < numSamps; i++)
	{
		EXPECT_FLOAT_EQ((*buf1)[i], (float)i * -0.01f);
	}
}

// ------------------------------------------------------------------
// Test: Block write with fade mixing
// ------------------------------------------------------------------
TEST(AudioBlockWrite, BlockWriteWithFade) {
	auto bufSize = 100u;
	auto numSamps = 10u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

	// Write initial data (all 1.0)
	std::vector<float> initial(numSamps, 1.0f);
	AudioWriteRequest req1;
	req1.samples = initial.data();
	req1.numSamps = numSamps;
	req1.stride = 1;
	req1.fadeCurrent = 0.0f;
	req1.fadeNew = 1.0f;
	req1.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(req1, 0);

	// Overwrite with fade: 0.5 * existing + 0.5 * new(0.0)
	std::vector<float> overlay(numSamps, 0.0f);
	AudioWriteRequest req2;
	req2.samples = overlay.data();
	req2.numSamps = numSamps;
	req2.stride = 1;
	req2.fadeCurrent = 0.5f;
	req2.fadeNew = 0.5f;
	req2.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(req2, 0);

	// Result should be 0.5 * 0.0 + 0.5 * 1.0 = 0.5
	for (unsigned int i = 0; i < numSamps; i++)
	{
		EXPECT_FLOAT_EQ((*audioBuf)[i], 0.5f);
	}
}

// ------------------------------------------------------------------
// Helper: reads numSamps from audioBuf at readIndex into sink.
// Handles ring-buffer wrap-around.
// ------------------------------------------------------------------
static void ReadAudioBufferToSink(audio::AudioBuffer& audioBuf,
	const std::shared_ptr<BlockWriteMockedSink>& sink,
	unsigned int numSamps,
	unsigned int readIndex)
{
	auto bufSize = audioBuf.BufSize();

	if (audioBuf.IsContiguous(readIndex, numSamps))
	{
		AudioWriteRequest request;
		request.samples = audioBuf.BlockRead(readIndex);
		request.numSamps = numSamps;
		request.stride = 1;
		request.fadeCurrent = 0.0f;
		request.fadeNew = 1.0f;
		request.source = audioBuf.SourceType();
		sink->OnBlockWrite(request, 0);
	}
	else
	{
		auto firstPart = bufSize - readIndex;
		auto secondPart = numSamps - firstPart;

		AudioWriteRequest r1;
		r1.samples = audioBuf.BlockRead(readIndex);
		r1.numSamps = firstPart;
		r1.stride = 1;
		r1.fadeCurrent = 0.0f;
		r1.fadeNew = 1.0f;
		r1.source = audioBuf.SourceType();
		sink->OnBlockWrite(r1, 0);

		AudioWriteRequest r2;
		r2.samples = audioBuf.BlockRead(0);
		r2.numSamps = secondPart;
		r2.stride = 1;
		r2.fadeCurrent = 0.0f;
		r2.fadeNew = 1.0f;
		r2.source = audioBuf.SourceType();
		sink->OnBlockWrite(r2, (int)firstPart);
	}

	audioBuf.EndPlay(numSamps);
}

// ------------------------------------------------------------------
// Test: AudioBuffer block read uses OnBlockWrite (destination-centric)
// ------------------------------------------------------------------
TEST(AudioBlockWrite, OnPlayUsesBlockWrite) {
	auto bufSize = 100u;
	auto blockSize = 11u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	auto sink = std::make_shared<BlockWriteMockedSink>(bufSize);

	std::vector<float> source(bufSize);
	for (unsigned int i = 0; i < bufSize; i++)
		source[i] = (float)i / (float)bufSize;

	AudioWriteRequest fillReq;
	fillReq.samples = source.data();
	fillReq.numSamps = bufSize;
	fillReq.stride = 1;
	fillReq.fadeCurrent = 0.0f;
	fillReq.fadeNew = 1.0f;
	fillReq.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(fillReq, 0);
	audioBuf->EndWrite(bufSize, true);

	auto numBlocks = bufSize / blockSize;
	auto playIndex = audioBuf->Delay(blockSize);
	for (unsigned int i = 0; i < numBlocks; i++)
	{
		ReadAudioBufferToSink(*audioBuf, sink, blockSize, playIndex);
		playIndex = (playIndex + blockSize) % audioBuf->BufSize();
		sink->EndWrite(blockSize, true);
		audioBuf->EndWrite(blockSize, true);
	}

	ASSERT_GT(sink->BlockWriteCount, 0u);

	auto sampsWritten = numBlocks * blockSize;
	for (unsigned int i = 0; i < sampsWritten; i++)
	{
		EXPECT_FLOAT_EQ(sink->Samples[i], source[i]);
	}
}

// ------------------------------------------------------------------
// Test: AudioBuffer block read handles wrap-around via block write
// ------------------------------------------------------------------
TEST(AudioBlockWrite, OnPlayWrapsAroundViaBlockWrite) {
	auto bufSize = 100u;
	auto blockSize = 11u;

	auto audioBuf = AudioBuffer(bufSize);
	auto sink = std::make_shared<BlockWriteMockedSink>(bufSize);

	audioBuf.EndWrite(blockSize, false);

	auto numBlocks = (bufSize * 2) / blockSize;
	auto playIndex = 0u;

	for (unsigned int i = 0; i < numBlocks; i++)
	{
		ReadAudioBufferToSink(audioBuf, sink, blockSize, playIndex);
		playIndex = (playIndex + blockSize) % audioBuf.BufSize();
		sink->EndWrite(blockSize, true);
	}

	ASSERT_GT(sink->BlockWriteCount, 0u);
}

// ------------------------------------------------------------------
// Test: Block write through multi-channel sink
// ------------------------------------------------------------------
TEST(AudioBlockWrite, MultiChannelBlockWrite) {
	auto bufSize = 100u;
	auto numChannels = 2u;
	auto numSamps = 50u;

	auto multiSink = std::make_shared<BlockWriteMockedMultiSink>(bufSize, numChannels);

	std::vector<float> ch0Data(numSamps);
	for (unsigned int i = 0; i < numSamps; i++)
		ch0Data[i] = (float)i * 0.01f;

	AudioWriteRequest req0;
	req0.samples = ch0Data.data();
	req0.numSamps = numSamps;
	req0.stride = 1;
	req0.fadeCurrent = 0.0f;
	req0.fadeNew = 1.0f;
	req0.source = base::Audible::AUDIOSOURCE_ADC;

	multiSink->OnBlockWriteChannel(0, req0, 0);

	std::vector<float> ch1Data(numSamps);
	for (unsigned int i = 0; i < numSamps; i++)
		ch1Data[i] = (float)i * -0.01f;

	AudioWriteRequest req1;
	req1.samples = ch1Data.data();
	req1.numSamps = numSamps;
	req1.stride = 1;
	req1.fadeCurrent = 0.0f;
	req1.fadeNew = 1.0f;
	req1.source = base::Audible::AUDIOSOURCE_ADC;

	multiSink->OnBlockWriteChannel(1, req1, 0);

	auto ch0Sink = multiSink->GetChannel(0);
	for (unsigned int i = 0; i < numSamps; i++)
	{
		EXPECT_FLOAT_EQ(ch0Sink->Samples[i], (float)i * 0.01f);
	}

	auto ch1Sink = multiSink->GetChannel(1);
	for (unsigned int i = 0; i < numSamps; i++)
	{
		EXPECT_FLOAT_EQ(ch1Sink->Samples[i], (float)i * -0.01f);
	}
}

// ------------------------------------------------------------------
// Test: Block write produces correct output (replaces OnMixWrite comparison)
// ------------------------------------------------------------------
TEST(AudioBlockWrite, BlockWriteProducesCorrectOutput) {
	auto bufSize = 100u;
	auto numSamps = 50u;

	std::vector<float> source(numSamps);
	for (unsigned int i = 0; i < numSamps; i++)
		source[i] = ((rand() % 2000) - 1000) / 1001.0f;

	// Write using OnBlockWrite (block-level)
	auto buf = std::make_shared<AudioBuffer>(bufSize);
	AudioWriteRequest request;
	request.samples = source.data();
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	buf->OnBlockWrite(request, 0);

	for (unsigned int i = 0; i < numSamps; i++)
	{
		EXPECT_FLOAT_EQ((*buf)[i], source[i]);
	}
}
