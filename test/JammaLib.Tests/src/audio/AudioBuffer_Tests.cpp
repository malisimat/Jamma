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

	base::AudioWriteRequest request;
	request.samples = data.data();
	request.numSamps = blockSize;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(request, 0);
	audioBuf->EndWrite(blockSize, true);

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

	std::vector<float> initial(blockSize, 1.0f);
	base::AudioWriteRequest req1;
	req1.samples = initial.data();
	req1.numSamps = blockSize;
	req1.stride = 1;
	req1.fadeCurrent = 0.0f;
	req1.fadeNew = 1.0f;
	req1.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(req1, 0);

	std::vector<float> addData(blockSize, 0.5f);
	base::AudioWriteRequest req2;
	req2.samples = addData.data();
	req2.numSamps = blockSize;
	req2.stride = 1;
	req2.fadeCurrent = 1.0f;
	req2.fadeNew = 1.0f;
	req2.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(req2, 0);

	for (auto i = 0u; i < blockSize; i++)
	{
		ASSERT_FLOAT_EQ((*audioBuf)[i], 1.5f)
			<< "Mismatch at index " << i;
	}
}

TEST(AudioBuffer, BlockWriteWrapsAround) {
	auto bufSize = 32u;
	auto blockSize = 20u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);

	std::vector<float> padding(bufSize - 5, 0.0f);
	base::AudioWriteRequest padReq;
	padReq.samples = padding.data();
	padReq.numSamps = bufSize - 5;
	padReq.stride = 1;
	padReq.fadeCurrent = 0.0f;
	padReq.fadeNew = 1.0f;
	padReq.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(padReq, 0);
	audioBuf->EndWrite(bufSize - 5, true);

	std::vector<float> data(blockSize);
	for (auto i = 0u; i < blockSize; i++)
		data[i] = (float)(i + 1) * 0.01f;

	base::AudioWriteRequest request;
	request.samples = data.data();
	request.numSamps = blockSize;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(request, 0);

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
	auto bufSize = 100u;
	auto blockSize = 17u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	std::vector<float> srcData(bufSize);
	for (auto i = 0u; i < bufSize; i++)
		srcData[i] = ((rand() % 2000) - 1000) / 1001.0f;

	base::AudioWriteRequest request;
	request.samples = srcData.data();
	request.numSamps = bufSize;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(request, 0);
	audioBuf->EndWrite(bufSize, true);

	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);
	auto numBlocks = (bufSize * 2) / blockSize;
	for (auto i = 0u; i < numBlocks; i++)
	{
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(sink->IsFilled());

	for (auto i = 0u; i < bufSize; i++)
	{
		ASSERT_FLOAT_EQ(sink->Samples[i], srcData[i])
			<< "Mismatch at index " << i;
	}
}
