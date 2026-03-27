#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "audio/AudioBuffer.h"
#include <algorithm>
#include <sstream>

using resources::ResourceLib;
using audio::AudioBuffer;
using base::AudioSource;
using base::AudioSink;
using base::AudioSourceParams;

namespace
{
	inline unsigned int SpanSize(const AudioBuffer& data)
	{
		return data.BufSize();
	}

	template<typename T>
	unsigned int SpanSize(const T& data)
	{
		return static_cast<unsigned int>(data.size());
	}

	template<typename T>
	std::string DumpFloatSpan(const T& data, unsigned int start, unsigned int count)
	{
		std::ostringstream oss;
		oss << "[";

		const auto dataSize = SpanSize(data);
		const auto safeStart = (std::min)(start, dataSize);
		const auto safeEnd = (std::min)(safeStart + count, dataSize);

		for (auto i = safeStart; i < safeEnd; ++i)
		{
			if (i > safeStart)
				oss << ", ";

			oss << data[i];
		}

		oss << "]";
		return oss.str();
	}
}

class InspectableAudioBuffer :
	public AudioBuffer
{
public:
	InspectableAudioBuffer(unsigned int size) : AudioSource({}), AudioBuffer(size) {}
	unsigned long WriteIndex() const { return _writeIndex; }
};

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
	inline virtual int OnMixWrite(float samp,
		float fadeCurrent,
		float fadeNew,
		int indexOffset,
		Audible::AudioSourceType source) override
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

	auto audioBuf = std::make_shared<InspectableAudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<MockedSource>(bufSize, params);
	auto sink = std::make_shared<MockedSink>(bufSize);

	ASSERT_EQ(0ul, audioBuf->WriteIndex());

	audioBuf->Zero(bufSize);

	auto numBlocks = (bufSize / blockSize) + 1;
	for (int i = 0; i < numBlocks; i++)
	{
		// Play source to buffer
		source->OnPlay(audioBuf, 0u, blockSize);
		source->EndPlay(blockSize);
		audioBuf->EndWrite(blockSize, true);

		if (0 == i)
		{
			std::cout << "Buffer[0..3] after first write: "
				<< (*audioBuf)[0] << ", "
				<< (*audioBuf)[1] << ", "
				<< (*audioBuf)[2] << ", "
				<< (*audioBuf)[3] << std::endl;
			std::cout << "SampsRecorded after first write: " << audioBuf->SampsRecorded() << std::endl;
		}

		// Play buffer to mocked sink
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);

		if (0 == i)
			std::cout << "Sink[0..3] after first play: "
				<< sink->Samples[0] << ", "
				<< sink->Samples[1] << ", "
				<< sink->Samples[2] << ", "
				<< sink->Samples[3] << std::endl;
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
		std::cout << "\n--- Iteration " << i << " ---" << std::endl;
		std::cout << "Source head before write: " << DumpFloatSpan(source->GetSamples(), 0u, 8u) << std::endl;
		std::cout << "Buffer head before write: " << DumpFloatSpan(*audioBuf, 0u, 8u) << std::endl;

		// Play source to buffer
		source->OnPlay(audioBuf, 0u, blockSize);
		source->EndPlay(blockSize);
		audioBuf->EndWrite(blockSize, true);

		std::cout << "Buffer head after write:  " << DumpFloatSpan(*audioBuf, 0u, 8u) << std::endl;

		// Play buffer to mocked sink
		audioBuf->Delay(delaySamps + blockSize);
		std::cout << "Requested delay: " << (delaySamps + blockSize)
			<< " (assert checks observable offset of " << delaySamps << ")" << std::endl;
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);

		std::cout << "Sink head after play:    " << DumpFloatSpan(sink->Samples, 0u, 8u) << std::endl;
		if (delaySamps < sink->Samples.size())
		{
			auto aroundDelayStart = delaySamps >= 2u ? delaySamps - 2u : 0u;
			std::cout << "Sink around delay idx:   "
				<< DumpFloatSpan(sink->Samples, aroundDelayStart, 8u)
				<< std::endl;
		}
	}

	const auto matched = source->MatchesSink(sink, delaySamps);
	if (!matched)
	{
		auto tailStart = sink->Samples.size() > 16u ? static_cast<unsigned int>(sink->Samples.size() - 16u) : 0u;

		std::cout << "\nFinal source head: " << DumpFloatSpan(source->GetSamples(), 0u, 16u) << std::endl;
		std::cout << "Final sink head:   " << DumpFloatSpan(sink->Samples, 0u, 16u) << std::endl;
		std::cout << "Final sink tail:   "
			<< DumpFloatSpan(sink->Samples, tailStart, 16u)
			<< std::endl;
	}

	ASSERT_TRUE(matched);
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
