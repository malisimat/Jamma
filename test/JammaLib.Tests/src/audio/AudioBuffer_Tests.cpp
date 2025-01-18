
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "audio/AudioBuffer.h"

using resources::ResourceLib;
using audio::AudioBuffer;
using base::AudioSource;
using base::AudioSink;
using base::AudioSourceParams;

class MockedSink :
	public AudioSink
{
public:
	MockedSink(unsigned int bufSize) :
		Samples({})
	{
		Samples = std::vector<float>(bufSize);

		for (auto i = 0u; i < bufSize; i++)
		{
			Samples[i] = 0.0f;
		}
	}

public:
	inline virtual int OnWrite(float samp, int indexOffset)
	{
		if ((_writeIndex + indexOffset) < Samples.size())
			Samples[_writeIndex + indexOffset] = samp;

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

class MockedSource :
	public AudioSource
{
public:
	MockedSource(unsigned int bufSize,
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
		auto source = AUDIOSOURCE_INPUT;

		for (auto i = 0u; i < numSamps; i++)
		{
			if (index < Samples.size())
				dest->OnMixWrite(Samples[index], 1.0f, 1.0f, i, source);

			index++;
		}
	}
	virtual void EndPlay(unsigned int numSamps)
	{
		_index += numSamps;
	}
	bool WasPlayed() { return _index >= Samples.size(); }
	bool MatchesSink(const std::shared_ptr<MockedSink> buf, unsigned int expectedDelay)
	{
		auto numSamps = buf->Samples.size();
		for (auto samp = 0u; samp < numSamps; samp++)
		{
			if (samp >= Samples.size())
				return false;

			if ((samp + expectedDelay) < buf->Samples.size())
			{
				unsigned int sinkIndex = samp + expectedDelay;

				std::cout << "Comparing index " << sinkIndex << "/" << samp << ": " << buf->Samples[sinkIndex] << " = " << Samples[samp] << "?" << std::endl;

				if (buf->Samples[sinkIndex] != Samples[samp])
					return false;
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
	auto sink = std::make_shared<MockedSink>(bufSize);

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
	auto source = std::make_shared<MockedSource>(bufSize, params);

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
	auto source = std::make_shared<MockedSource>(bufSize, params);
	auto sink = std::make_shared<MockedSink>(bufSize);

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
	auto source = std::make_shared<MockedSource>(bufSize, params);
	auto sink = std::make_shared<MockedSink>(bufSize);

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

	ASSERT_TRUE(source->MatchesSink(sink, delaySamps));
}

TEST(AudioBuffer, ClampsToMaxBufSize) {
	auto bufSize = 100;
	auto blockSize = 11;
	auto delaySamps = bufSize + 10;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<MockedSource>(bufSize, params);
	auto sink = std::make_shared<MockedSink>(bufSize);

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
	auto source = std::make_shared<MockedSource>(blockSize, params);
	auto sink = std::make_shared<MockedSink>(blockSize);

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
