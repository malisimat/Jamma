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
	unsigned long PlayIndex() const { return _playIndex; }
	unsigned int RecordedSamples() const { return _sampsRecorded; }
};

class AudioBufferTestSink :
	public AudioSink
{
public:
	AudioBufferTestSink(unsigned int bufSize) :
		Samples({}),
		TraceWrites(false),
		TraceLabel("sink"),
		OnMixWriteCallCount(0),
		LastWriteIndices({}),
		LastWriteSamples({})
	{
		Samples = std::vector<float>(bufSize);

		for (auto i = 0u; i < bufSize; i++)
		{
			Samples[i] = 0.0f;
		}
	}

public:
	void SetTraceWrites(bool enabled, const std::string& label = "sink")
	{
		TraceWrites = enabled;
		TraceLabel = label;
	}

	unsigned long WriteIndex() const
	{
		return _writeIndex;
	}

	void ResetWriteTrace()
	{
		OnMixWriteCallCount = 0;
		LastWriteIndices.clear();
		LastWriteSamples.clear();
	}

	inline virtual int OnMixWrite(float samp,
		float fadeCurrent,
		float fadeNew,
		int indexOffset,
		Audible::AudioSourceType source) override
	{
		auto destIndex = _writeIndex + indexOffset;
		OnMixWriteCallCount++;
		LastWriteIndices.push_back(destIndex);
		LastWriteSamples.push_back(samp);

		if (TraceWrites)
		{
			std::cout
				<< "[" << TraceLabel << "] OnMixWrite"
				<< " writeBase=" << _writeIndex
				<< " indexOffset=" << indexOffset
				<< " destIndex=" << destIndex
				<< " sample=" << samp
				<< " sourceType=" << source
				<< std::endl;
		}

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
	bool TraceWrites;
	std::string TraceLabel;
	unsigned int OnMixWriteCallCount;
	std::vector<unsigned long> LastWriteIndices;
	std::vector<float> LastWriteSamples;
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
	bool MatchesSink(const std::shared_ptr<AudioBufferTestSink> buf, unsigned int expectedDelay)
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
	auto sink = std::make_shared<AudioBufferTestSink>(bufSize);

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
	auto sink = std::make_shared<AudioBufferTestSink>(bufSize);

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

	auto audioBuf = std::make_shared<InspectableAudioBuffer>(bufSize);
	AudioSourceParams params;
	auto source = std::make_shared<MockedSource>(bufSize, params);
	auto sink = std::make_shared<AudioBufferTestSink>(bufSize);
	sink->SetTraceWrites(true, "IsCorrectlyDelayed");

	audioBuf->Zero(bufSize);

	auto numBlocks = (bufSize / blockSize) + 1;
	for (int i = 0; i < numBlocks; i++)
	{
		std::cout << "\n--- Iteration " << i << " ---" << std::endl;
		std::cout << "Source head before write: " << DumpFloatSpan(source->GetSamples(), 0u, 8u) << std::endl;
		std::cout << "Buffer head before write: " << DumpFloatSpan(static_cast<const AudioBuffer&>(*audioBuf), 0u, 8u) << std::endl;

		// Play source to buffer
		source->OnPlay(audioBuf, 0u, blockSize);
		source->EndPlay(blockSize);
		audioBuf->EndWrite(blockSize, true);

		std::cout << "Buffer head after write:  " << DumpFloatSpan(static_cast<const AudioBuffer&>(*audioBuf), 0u, 8u) << std::endl;

		// Play buffer to mocked sink
		audioBuf->Delay(delaySamps + blockSize);
		std::cout << "Requested delay: " << (delaySamps + blockSize)
			<< " (assert checks observable offset of " << delaySamps << ")" << std::endl;
		std::cout << "AudioBuffer state before OnPlay:"
			<< " playIndex=" << audioBuf->PlayIndex()
			<< " writeIndex=" << audioBuf->WriteIndex()
			<< " recorded=" << audioBuf->RecordedSamples()
			<< std::endl;
		for (auto samp = 0u; samp < static_cast<unsigned int>(blockSize); ++samp)
		{
			auto readIndex = static_cast<unsigned int>((audioBuf->PlayIndex() + samp) % audioBuf->BufSize());
			auto destIndex = static_cast<unsigned int>(sink->WriteIndex() + samp);
			std::cout
				<< "  [read->write] srcIndex=" << readIndex
				<< " srcValue=" << (*audioBuf)[readIndex]
				<< " sinkIndex=" << destIndex
				<< std::endl;
		}
		sink->ResetWriteTrace();
		audioBuf->OnPlay(sink, 0, blockSize);
		audioBuf->EndPlay(blockSize);
		sink->EndWrite(blockSize, true);

		std::cout << "Sink OnMixWrite calls this block: " << sink->OnMixWriteCallCount << std::endl;
		if (!sink->LastWriteIndices.empty())
		{
			std::cout << "  Sink write indices: [";
			for (auto idx = 0u; idx < sink->LastWriteIndices.size(); ++idx)
			{
				if (idx > 0u)
					std::cout << ", ";
				std::cout << sink->LastWriteIndices[idx];
			}
			std::cout << "]" << std::endl;

			std::cout << "  Sink write samples: [";
			for (auto idx = 0u; idx < sink->LastWriteSamples.size(); ++idx)
			{
				if (idx > 0u)
					std::cout << ", ";
				std::cout << sink->LastWriteSamples[idx];
			}
			std::cout << "]" << std::endl;
		}

		std::cout << "Sink head after play:    " << DumpFloatSpan(sink->Samples, 0u, 8u) << std::endl;
		if (delaySamps < sink->Samples.size())
		{
			auto aroundDelayStart = delaySamps >= 2u ? delaySamps - 2u : 0u;
			std::cout << "Sink around delay idx:   "
				<< DumpFloatSpan(sink->Samples, aroundDelayStart, 8u)
				<< std::endl;
		}
	}

	std::cout << "\nFinal full source: "
		<< DumpFloatSpan(source->GetSamples(), 0u, static_cast<unsigned int>(source->GetSamples().size()))
		<< std::endl;
	std::cout << "Final full buffer: "
		<< DumpFloatSpan(static_cast<const AudioBuffer&>(*audioBuf), 0u, audioBuf->BufSize())
		<< std::endl;
	std::cout << "Final full sink:   "
		<< DumpFloatSpan(sink->Samples, 0u, static_cast<unsigned int>(sink->Samples.size()))
		<< std::endl;

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
	auto sink = std::make_shared<AudioBufferTestSink>(bufSize);

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
	auto sink = std::make_shared<AudioBufferTestSink>(blockSize);

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
