
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
using engine::Trigger;

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
	inline virtual int OnMixWrite(float samp,
		float fadeCurrent,
		float fadeNew,
		int indexOffset,
		base::Audible::AudioSourceType source) override
	{
		if ((_writeIndex + indexOffset) < Samples.size())
			Samples[_writeIndex + indexOffset] = (fadeNew * samp) + (fadeCurrent * Samples[_writeIndex + indexOffset]);

		return indexOffset + 1;
	};
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

			std::cout << "Comparing index " << samp << ": " << buf[samp] << " = " << Samples[samp] << "?" << std::endl;

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

class ChannelMixerMockedSource :
	public AudioSource
{
public:
	ChannelMixerMockedSource(unsigned int bufSize,
		AudioSourceParams params) :
		_writeIndex(0),
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
		auto index = _writeIndex;
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
		_writeIndex += numSamps;
	}
	bool WasPlayed() { return _writeIndex >= Samples.size(); }
	bool MatchesSink(const std::shared_ptr<ChannelMixerMockedSink> buf)
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
	bool MatchesBuffer(const std::vector<float>& buf)
	{
		auto numSamps = Samples.size();
		for (auto samp = 0u; samp < numSamps; samp++)
		{
			if (samp >= buf.size())
				return false;

			std::cout << "Comparing index " << samp << ": " << buf[samp] << " = " << Samples[samp] << "?" << std::endl;

			if (buf[samp] != Samples[samp])
				return false;
		}

		return true;
	}

private:
	unsigned int _writeIndex;
	std::vector<float> Samples;
};

class MockedMultiSource :
	public MultiAudioSource
{
public:
	MockedMultiSource(unsigned int bufSize)
	{
		AudioSourceParams params;
		_source = std::make_shared<ChannelMixerMockedSource>(bufSize, params);
	}

public:
	virtual unsigned int NumOutputChannels(base::Audible::AudioSourceType source) const override { return 1; };

	bool WasPlayed() { return _source->WasPlayed(); }
	bool MatchesSink(std::shared_ptr<ChannelMixerMockedSink> sink) { return _source->MatchesSink(sink); }
	bool MatchesBuffer(const std::vector<float>& buf) { return _source->MatchesBuffer(buf); }

protected:
	virtual const std::shared_ptr<AudioSource> _OutputChannel(unsigned int channel) override
	{
		if (channel == 0)
		{
			_source->SetSourceType(SourceType());
			return _source;
		}

		return nullptr;
	}
private:
	std::shared_ptr<ChannelMixerMockedSource> _source;
};

TEST(ChannelMixer, PlayWrapsAroundAndMatches) {
	auto bufSize = 100;
	auto blockSize = 11;

	ChannelMixerParams chanParams;
	chanParams.InputBufferSize = bufSize;
	chanParams.OutputBufferSize = bufSize;
	chanParams.NumInputChannels = 1;
	chanParams.NumOutputChannels = 1;

	engine::TriggerParams trigParams;
	trigParams.Index = 0;

	auto chanMixer = ChannelMixer(chanParams);
	auto trigger = std::make_shared<engine::Trigger>(trigParams);
	auto sink = std::make_shared<MockedMultiSink>(bufSize);

	auto buf = std::vector<float>(bufSize);
	for (auto samp = 0; samp < bufSize; samp++)
	{
		buf[samp] = ((rand() % 2000) - 1000) / 1001.0f;
	}

	chanMixer.FromAdc(buf.data(), 1, bufSize);

	auto numBlocks = (bufSize * 2) / blockSize;
	for (int i = 0; i < numBlocks; i++)
	{
		sink->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
		chanMixer.Source()->OnPlay(sink, trigger, 0u, blockSize);
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

	engine::TriggerParams trigParams;
	trigParams.Index = 0;

	auto chanMixer = ChannelMixer(chanParams);
	auto trigger = std::make_shared<engine::Trigger>(trigParams);
	auto source = std::make_shared<MockedMultiSource>(bufSize);

	auto numBlocks = (bufSize * 2) / blockSize;
	auto buf = std::vector<float>();

	for (int i = 0; i < numBlocks; i++)
	{
		chanMixer.Sink()->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);
		source->OnPlay(chanMixer.Sink(), trigger, 0u, blockSize);
		source->EndMultiPlay(blockSize);

		auto tempBuf = std::vector<float>(blockSize);
		chanMixer.ToDac(tempBuf.data(), 1, blockSize);
		chanMixer.Sink()->EndMultiWrite(blockSize, true, base::Audible::AUDIOSOURCE_ADC);

		for (auto v : tempBuf)
			buf.push_back(v);
	}

	ASSERT_TRUE(source->WasPlayed());
	ASSERT_TRUE(source->MatchesBuffer(buf));
}
