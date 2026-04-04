#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "audio/AudioBuffer.h"
#include "base/AudioSink.h"

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
	virtual void OnBlockWrite(const base::AudioWriteRequest& request, int writeOffset) override
	{
		for (auto i = 0u; i < request.numSamps; i++)
		{
			auto destIndex = _writeIndex + writeOffset + i;
			if (destIndex < Samples.size())
				Samples[destIndex] = (request.fadeNew * request.samples[i * request.stride]) +
					(request.fadeCurrent * Samples[destIndex]);
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

class AudioBufferMockedSource :
	public AudioSource
{
public:
	AudioBufferMockedSource(unsigned int bufSize,
		AudioSourceParams params) :
		_index(0),
		AudioSource(params)
	{
		Samples = std::vector<float>(bufSize);

		for (auto i = 0u; i < bufSize; i++)
		{
			Samples[i] = ((rand() % 2000) - 1000) / 1001.0f;
		}
	}

public:
	virtual void EndPlay(unsigned int numSamps)
	{
		_index += numSamps;
	}
	const float* SamplesAt() const { return Samples.data() + _index; }
	unsigned int SamplesRemaining() const
	{
		return (_index >= Samples.size()) ? 0 : (unsigned int)(Samples.size() - _index);
	}
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

	std::vector<float> Samples;

protected:
	unsigned int _index;
};

// Helper: read numSamps from audioBuf at readIndex into sink via block write.
// Handles ring-buffer wrap-around when the read range crosses the buffer end.
static void ReadBufferToSinkAt(audio::AudioBuffer& audioBuf,
	const std::shared_ptr<AudioSink>& sink,
	unsigned int numSamps,
	unsigned int readIndex)
{
	auto bufSize = audioBuf.BufSize();

	if (audioBuf.IsContiguous(readIndex, numSamps))
	{
		base::AudioWriteRequest request;
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

		base::AudioWriteRequest r1;
		r1.samples = audioBuf.BlockRead(readIndex);
		r1.numSamps = firstPart;
		r1.stride = 1;
		r1.fadeCurrent = 0.0f;
		r1.fadeNew = 1.0f;
		r1.source = audioBuf.SourceType();
		sink->OnBlockWrite(r1, 0);

		base::AudioWriteRequest r2;
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

// Helper: write source samples into audioBuf via block write.
static void WriteSourceToBuffer(const std::shared_ptr<AudioBufferMockedSource>& source,
	const std::shared_ptr<AudioBuffer>& audioBuf,
	unsigned int numSamps)
{
	auto sampsToWrite = std::min(numSamps, source->SamplesRemaining());

	base::AudioWriteRequest request;
	request.samples = source->SamplesAt();
	request.numSamps = sampsToWrite;
	request.stride = 1;
	request.fadeCurrent = 0.0f;
	request.fadeNew = 1.0f;
	request.source = base::Audible::AUDIOSOURCE_ADC;

	audioBuf->OnBlockWrite(request, 0);
	source->EndPlay(numSamps);
	audioBuf->EndWrite(numSamps, true);
}

TEST(AudioBuffer, PlayWrapsAround) {
	auto bufSize = 100u;
	auto blockSize = 11u;

	auto audioBuf = AudioBuffer(bufSize);
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

	// Trick buffer into thinking it has recorded something
	audioBuf.EndWrite(blockSize, false);

	auto numBlocks = (bufSize * 2) / blockSize;
	auto playIndex = 0u;

	for (auto i = 0u; i < numBlocks; i++)
	{
		ReadBufferToSinkAt(audioBuf, sink, blockSize, playIndex);
		playIndex = (playIndex + blockSize) % audioBuf.BufSize();
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(sink->IsFilled());
}

TEST(AudioBuffer, WriteWrapsAround) {
	auto bufSize = 100u;
	auto blockSize = 11u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(bufSize, params);

	auto numBlocks = (bufSize * 2) / blockSize;

	for (auto i = 0u; i < numBlocks; i++)
	{
		WriteSourceToBuffer(source, audioBuf, blockSize);
	}

	ASSERT_TRUE(source->WasPlayed());
}

TEST(AudioBuffer, WriteMatchesRead) {
	auto bufSize = 100u;
	auto blockSize = 11u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(bufSize, params);
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

	audioBuf->Zero(bufSize);

	auto numBlocks = (bufSize / blockSize) + 1;
	for (auto i = 0u; i < numBlocks; i++)
	{
		// Write source to buffer
		WriteSourceToBuffer(source, audioBuf, blockSize);

		// Read buffer to mocked sink
		auto playIdx = audioBuf->Delay(blockSize);
		ReadBufferToSinkAt(*audioBuf, sink, blockSize, playIdx);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(source->MatchesSink(sink, 0));
}

TEST(AudioBuffer, IsCorrectlyDelayed) {
	auto bufSize = 100u;
	auto blockSize = 11u;
	auto delaySamps = 42u;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(bufSize, params);
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

	audioBuf->Zero(bufSize);

	auto numBlocks = (bufSize / blockSize) + 1;
	for (auto i = 0u; i < numBlocks; i++)
	{
		WriteSourceToBuffer(source, audioBuf, blockSize);

		auto playIdx = audioBuf->Delay(delaySamps + blockSize);
		ReadBufferToSinkAt(*audioBuf, sink, blockSize, playIdx);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(source->MatchesSink(sink, delaySamps));
}

TEST(AudioBuffer, ClampsToMaxBufSize) {
	auto bufSize = 100u;
	auto blockSize = 11u;
	auto delaySamps = bufSize + 10;

	auto audioBuf = std::make_shared<AudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(bufSize, params);
	auto sink = std::make_shared<AudioBufferMockedSink>(bufSize);

	audioBuf->Zero(bufSize);

	auto numBlocks = (bufSize / blockSize) + 1;
	for (auto i = 0u; i < numBlocks; i++)
	{
		// Write source to buffer
		WriteSourceToBuffer(source, audioBuf, blockSize);

		// Read buffer to mocked sink with excessive delay
		auto playIdx = audioBuf->Delay(delaySamps + blockSize);
		ReadBufferToSinkAt(*audioBuf, sink, blockSize, playIdx);
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(source->MatchesSink(sink, bufSize));
}

TEST(AudioBuffer, ExcessiveDelayPlaysNicely) {
	auto bufSize = (unsigned int)constants::MaxBlockSize + 10;
	auto blockSize = 11u;
	auto delaySamps = bufSize;

	auto audioBuf = std::make_shared<AudioBuffer>(constants::MaxBlockSize);
	AudioSourceParams params;
	auto source = std::make_shared<AudioBufferMockedSource>(blockSize, params);
	auto sink = std::make_shared<AudioBufferMockedSink>(blockSize);

	audioBuf->Zero(bufSize);

	auto numBlocks = 2u;
	for (auto i = 0u; i < numBlocks; i++)
	{
		// Write source to buffer
		WriteSourceToBuffer(source, audioBuf, blockSize);

		// Read buffer to mocked sink with excessive delay
		auto playIdx = audioBuf->Delay(delaySamps + blockSize);
		ReadBufferToSinkAt(*audioBuf, sink, blockSize, playIdx);
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
	auto playIndex = 0u;
	for (auto i = 0u; i < numBlocks; i++)
	{
		ReadBufferToSinkAt(*audioBuf, sink, blockSize, playIndex);
		playIndex = (playIndex + blockSize) % audioBuf->BufSize();
		sink->EndWrite(blockSize, true);
	}

	ASSERT_TRUE(sink->IsFilled());

	for (auto i = 0u; i < bufSize; i++)
	{
		ASSERT_FLOAT_EQ(sink->Samples[i], srcData[i])
			<< "Mismatch at index " << i;
	}
}
