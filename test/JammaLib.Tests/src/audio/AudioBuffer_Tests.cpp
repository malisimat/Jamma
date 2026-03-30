#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "audio/AudioBuffer.h"

using resources::ResourceLib;
using audio::AudioBuffer;
using base::AudioSource;
using base::AudioSink;
using base::AudioSourceParams;

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
	inline virtual int OnMixWrite(float samp,
		float fadeCurrent,
		float fadeNew,
		int indexOffset,
		Audible::AudioSourceType source) override
	{
		auto destIndex = _writeIndex + indexOffset;

		if (destIndex < Samples.size())
			Samples[destIndex] = samp;

		return indexOffset + 1;
	};
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

class AudioBufferMockedSource :
	public AudioSource
{
public:
	AudioBufferMockedSource(unsigned int bufSize,
		AudioSourceParams params) :
		_index(0),
		Samples({}),
		AudioSource(params)
	{
		Samples = std::vector<float>(bufSize);

		for (auto i = 0u; i < bufSize; i++)
		{
			Samples[i] = ((rand() % 2000) - 1000) / 1001.0f;
		}
	}

public:
	virtual void OnPlay(const std::shared_ptr<base::AudioSink> dest,
		int indexOffset,
		unsigned int numSamps)
	{
		auto index = _index;
		auto source = AUDIOSOURCE_ADC;

		for (auto i = 0u; i < numSamps; i++)
		{
			if (index < Samples.size())
				dest->OnMixWrite(Samples[index], 0.0f, 1.0f, i, source);

			index++;
		}
	}
	virtual void EndPlay(unsigned int numSamps)
	{
		_index += numSamps;
	}
	const std::vector<float>& GetSamples() const { return Samples; }
	bool WasPlayed() { return _index >= Samples.size(); }
	bool MatchesSink(const std::shared_ptr<AudioBufferMockedSink> buf, unsigned int expectedDelay)
	{
		auto numSamps = buf->Samples.size();
		for (auto samp = 0u; samp < numSamps; samp++)
		{
			if (samp >= Samples.size())
				return false;

			if ((samp + expectedDelay) < buf->Samples.size())
			{
				unsigned int sinkIndex = samp + expectedDelay;
				if (buf->Samples[sinkIndex] != Samples[samp])
				{
					std::cout
						<< "Mismatch at source=" << samp
						<< " sink=" << sinkIndex
						<< " expected=" << Samples[samp]
						<< " actual=" << buf->Samples[sinkIndex]
						<< " expectedDelay=" << expectedDelay
						<< std::endl;

					return false;
				}
			}
		}

		return true;
	}

protected:
	unsigned int _index;
	std::vector<float> Samples;
};

TEST(AudioBuffer, PlayWrapsAround) {
	auto bufSize = 100;
	auto blockSize = 11;

	auto audioBuf = AudioBuffer(bufSize);
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

	// Trick buffer into thinking it has recorded something
	audioBuf.EndWrite(blockSize, false);

	auto numBlocks = (bufSize * 2) / blockSize;

	for (int i = 0; i < numBlocks; i++)
	{
		audioBuf.OnPlay(sink, 0, blockSize);
		audioBuf.EndPlay(blockSize);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(sink->IsFilled());
}

TEST(AudioBuffer, WriteWrapsAround) {
	auto bufSize = 100;
	auto blockSize = 11;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(bufSize, params);

	auto numBlocks = (bufSize * 2) / blockSize;

	for (int i = 0; i < numBlocks; i++)
	{
		source->OnPlay(audioBuf, 0u, blockSize);
		source->EndPlay(blockSize);
		audioBuf->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(source->WasPlayed());
}

TEST(AudioBuffer, WriteMatchesRead) {
	auto bufSize = 100;
	auto blockSize = 11;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(bufSize, params);
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

	audioBuf->Zero(bufSize);

	auto numBlocks = (bufSize / blockSize) + 1;
	for (int i = 0; i < numBlocks; i++)
	{
		// Play source to buffer
		source->OnPlay(audioBuf, 0u, blockSize);
		source->EndPlay(blockSize);
		audioBuf->EndWrite(blockSize, true);

		// Play buffer to mocked sink
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(source->MatchesSink(sink, 0));
}

TEST(AudioBuffer, IsCorrectlyDelayed) {
	auto bufSize = 100;
	auto blockSize = 11;
	auto delaySamps = 42u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(bufSize, params);
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

	audioBuf->Zero(bufSize);

	auto numBlocks = (bufSize / blockSize) + 1;
	for (int i = 0; i < numBlocks; i++)
	{
		source->OnPlay(audioBuf, 0u, blockSize);
		source->EndPlay(blockSize);
		audioBuf->EndWrite(blockSize, true);

		audioBuf->Delay(delaySamps + blockSize);
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(source->MatchesSink(sink, delaySamps));
}

TEST(AudioBuffer, ClampsToMaxBufSize) {
	auto bufSize = 100;
	auto blockSize = 11;
	auto delaySamps = bufSize + 10;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(bufSize, params);
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

	audioBuf->Zero(bufSize);

	auto numBlocks = (bufSize / blockSize) + 1;
	for (int i = 0; i < numBlocks; i++)
	{
		// Play source to buffer
		source->OnPlay(audioBuf, 0u, blockSize);
		source->EndPlay(blockSize);
		audioBuf->EndWrite(blockSize, true);

		// Play buffer to mocked sink
		audioBuf->Delay(delaySamps + blockSize);
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(source->MatchesSink(sink, bufSize));
}

TEST(AudioBuffer, ExcessiveDelayPlaysNicely) {
	auto bufSize = (int)constants::MaxBlockSize + 10;
	auto blockSize = 11;
	auto delaySamps = bufSize;

	auto audioBuf = std::make_shared<AudioBuffer>(constants::MaxBlockSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(blockSize, params);
	auto sink = std::make_shared<AudioBufferMockedSink>(blockSize);

	audioBuf->Zero(bufSize);

	auto numBlocks = 2;
	for (int i = 0; i < numBlocks; i++)
	{
		// Play source to buffer
		source->OnPlay(audioBuf, 0u, blockSize);
		source->EndPlay(blockSize);
		audioBuf->EndWrite(blockSize, true);

		// Play buffer to mocked sink
		audioBuf->Delay(delaySamps + blockSize);
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(sink->IsZero());
}

TEST(AudioBuffer, BlockWritePureCopy) {
	auto bufSize = 64u;
	auto blockSize = 16u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

	std::vector<float> data(blockSize);
	for (auto i = 0u; i < blockSize; i++)
		data[i] = (float)(i + 1) * 0.1f;

	// Pure copy: fadeCurrent=0, fadeNew=1
	audioBuf->OnBlockWrite(data.data(), blockSize, 0, 0.0f, 1.0f,
		base::Audible::AUDIOSOURCE_ADC);
	audioBuf->EndWrite(blockSize, true);

	// Verify samples were copied
	for (auto i = 0u; i < blockSize; i++)
	{
		ASSERT_FLOAT_EQ((*audioBuf)[i], data[i])
			<< "Mismatch at index " << i;
	}
}

TEST(AudioBuffer, BlockWriteAdditiveMix) {
	auto bufSize = 64u;
	auto blockSize = 16u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

	// Write initial data
	std::vector<float> initial(blockSize);
	for (auto i = 0u; i < blockSize; i++)
		initial[i] = 1.0f;

	audioBuf->OnBlockWrite(initial.data(), blockSize, 0, 0.0f, 1.0f,
		base::Audible::AUDIOSOURCE_ADC);

	// Additive mix: fadeCurrent=1, fadeNew=1
	std::vector<float> addData(blockSize);
	for (auto i = 0u; i < blockSize; i++)
		addData[i] = 0.5f;

	audioBuf->OnBlockWrite(addData.data(), blockSize, 0, 1.0f, 1.0f,
		base::Audible::AUDIOSOURCE_ADC);

	// Verify: each sample should be initial + addData = 1.0 + 0.5 = 1.5
	for (auto i = 0u; i < blockSize; i++)
	{
		ASSERT_FLOAT_EQ((*audioBuf)[i], 1.5f)
			<< "Mismatch at index " << i;
	}
}

TEST(AudioBuffer, WriteInterleavedSingleChannel) {
	auto bufSize = 64u;
	auto numSamps = 16u;
	auto numChannels = 2u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

	// Interleaved stereo data: [L0, R0, L1, R1, ...]
	std::vector<float> interleaved(numSamps * numChannels);
	for (auto i = 0u; i < numSamps; i++)
	{
		interleaved[i * numChannels + 0] = (float)(i + 1) * 0.1f;  // L
		interleaved[i * numChannels + 1] = (float)(i + 1) * -0.1f; // R
	}

	// Write left channel (channel 0)
	audioBuf->WriteInterleaved(interleaved.data(), numSamps, numChannels, 0);
	audioBuf->EndWrite(numSamps, true);

	// Verify left channel was extracted correctly
	for (auto i = 0u; i < numSamps; i++)
	{
		float expected = (float)(i + 1) * 0.1f;
		ASSERT_FLOAT_EQ((*audioBuf)[i], expected)
			<< "Mismatch at index " << i;
	}
}

TEST(AudioBuffer, BlockWriteWrapsAround) {
	auto bufSize = 32u;
	auto blockSize = 20u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

	// Advance write index near end of buffer
	std::vector<float> padding(bufSize - 5, 0.0f);
	audioBuf->OnBlockWrite(padding.data(), bufSize - 5, 0, 0.0f, 1.0f,
		base::Audible::AUDIOSOURCE_ADC);
	audioBuf->EndWrite(bufSize - 5, true);

	// Now write a block that wraps around
	std::vector<float> data(blockSize);
	for (auto i = 0u; i < blockSize; i++)
		data[i] = (float)(i + 1) * 0.01f;

	audioBuf->OnBlockWrite(data.data(), blockSize, 0, 0.0f, 1.0f,
		base::Audible::AUDIOSOURCE_ADC);

	// Verify: first 5 samples at positions [27..31], rest at [0..14]
	for (auto i = 0u; i < 5; i++)
	{
		ASSERT_FLOAT_EQ((*audioBuf)[bufSize - 5 + i], data[i])
			<< "Mismatch at pre-wrap index " << i;
	}
	for (auto i = 5u; i < blockSize; i++)
	{
		ASSERT_FLOAT_EQ((*audioBuf)[i - 5], data[i])
			<< "Mismatch at post-wrap index " << i;
	}
}

TEST(AudioBuffer, BlockPlayMatchesSamplePlay) {
	// Verifies that the block-based OnPlay produces identical results
	// to the old sample-by-sample OnPlay
	auto bufSize = 100u;
	auto blockSize = 17u;

	// Setup: write known data into audio buffer
	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	std::vector<float> srcData(bufSize);
	for (auto i = 0u; i < bufSize; i++)
		srcData[i] = ((rand() % 2000) - 1000) / 1001.0f;

	audioBuf->OnBlockWrite(srcData.data(), bufSize, 0, 0.0f, 1.0f,
		base::Audible::AUDIOSOURCE_ADC);
	audioBuf->EndWrite(bufSize, true);

	// Play through block path to sink
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);
	auto numBlocks = (bufSize * 2) / blockSize;
	for (auto i = 0u; i < numBlocks; i++)
	{
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(sink->IsFilled());

	// Verify played data matches source
	for (auto i = 0u; i < bufSize; i++)
	{
		ASSERT_FLOAT_EQ(sink->Samples[i], srcData[i])
			<< "Mismatch at index " << i;
	}
}
