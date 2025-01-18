
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "engine/Loop.h"

using resources::ResourceLib;
using engine::Loop;
using engine::LoopParams;
using audio::WireMixBehaviourParams;
using audio::AudioMixerParams;
using base::AudioSource;
using base::AudioSink;
using base::MultiAudioSink;
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

	std::vector<float> Samples;
};

class MockedMultiSink :
	public MultiAudioSink
{
public:
	MockedMultiSink(unsigned int bufSize)
	{
		_sink = std::make_shared<MockedSink>(bufSize);
	}

public:
	virtual unsigned int NumInputChannels() const { return 1; };

	bool IsFilled() { return _sink->IsFilled(); }

protected:
	virtual const std::shared_ptr<AudioSink> InputChannel(unsigned int channel)
	{
		if (channel == 0)
			return _sink;

		return std::shared_ptr<AudioSink>();
	}
private:
	std::shared_ptr<MockedSink> _sink;
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
		unsigned int numSamps)
	{
		auto index = _index;
		auto source = AUDIOSOURCE_INPUT;

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
	bool WasPlayed() { return _index >= Samples.size(); }
	bool MatchesSink(const std::shared_ptr<MockedSink> buf)
	{
		auto numSamps = buf->Samples.size();
		for (auto samp = 0u; samp < numSamps; samp++)
		{
			if (samp >= Samples.size())
				return false;

			std::cout << "Comparing index " << samp << ": " << buf->Samples[samp] << " = " << Samples[samp] << "?" << std::endl;

			if (buf->Samples[samp] != Samples[samp])
				return false;
		}

		return true;
	}

private:
	unsigned int _index;
	std::vector<float> Samples;
};

TEST(Loop, PlayWrapsAround) {
	auto bufSize = 100;
	auto blockSize = 11;

	auto sink = std::make_shared<MockedMultiSink>(bufSize);

	WireMixBehaviourParams mixBehaviour;
	mixBehaviour.Channels = { 0 };
	AudioMixerParams mixerParams;
	mixerParams.Size = { 160, 320 };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = mixBehaviour;

	LoopParams loopParams;
	loopParams.Wav = "hh";
	loopParams.Size = { 80, 80 };
	loopParams.Position = { 10, 22 };

	engine::TriggerParams trigParams;
	trigParams.Index = 0;

	auto trigger = std::make_shared<engine::Trigger>(trigParams);

	auto loop = Loop(loopParams, mixerParams);

	auto numBlocks = (bufSize * 2) / blockSize;

	for (int i = 0; i < numBlocks; i++)
	{
		sink->Zero(blockSize);
		loop.OnPlay(sink, trigger, 0u, blockSize);
		loop.EndMultiPlay(blockSize);
		sink->EndMultiWrite(blockSize, true);
	}

	ASSERT_TRUE(sink->IsFilled());
}
