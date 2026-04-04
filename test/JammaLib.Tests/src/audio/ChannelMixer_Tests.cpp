
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "audio/ChannelMixer.h"
#include "engine/Trigger.h"
#include "base/AudioSink.h"

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
	bool MatchesBuffer(const std::vector<float>& buf)
	{
		auto numSamps = buf.size();
		for (auto samp = 0u; samp < numSamps; samp++)
		{
			if (samp >= Samples.size())
				return false;

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
		_writeIndex += numSamps;
	}
	unsigned int WriteIndex() const { return _writeIndex; }
	unsigned int SamplesRemaining() const
	{
		return (_writeIndex >= Samples.size()) ? 0 : (unsigned int)(Samples.size() - _writeIndex);
	}
	bool WasPlayed() { return _writeIndex >= Samples.size(); }
	bool MatchesBuffer(const std::vector<float>& buf)
	{
		auto numSamps = Samples.size();
		for (auto samp = 0u; samp < numSamps; samp++)
		{
			if (samp >= buf.size())
				return false;

			if (buf[samp] != Samples[samp])
				return false;
		}

		return true;
	}

	std::vector<float> Samples;

private:
	unsigned int _writeIndex;
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

	const float* SamplesAt() const
	{
		return _source->Samples.data() + _source->WriteIndex();
	}
	unsigned int SamplesRemaining() const { return _source->SamplesRemaining(); }
	bool WasPlayed() { return _source->WasPlayed(); }
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

	ChannelMixerParams chanParams;
	chanParams.InputBufferSize = bufSize;
	chanParams.OutputBufferSize = bufSize;
	chanParams.NumInputChannels = 1;
	chanParams.NumOutputChannels = 1;

	auto chanMixer = ChannelMixer(chanParams);
	auto sink = std::make_shared<MockedMultiSink>(bufSize);

	auto buf = std::vector<float>(bufSize);
	for (auto samp = 0; samp < bufSize; samp++)
	{
		buf[samp] = ((rand() % 2000) - 1000) / 1001.0f;
	}

	chanMixer.FromAdc(buf.data(), 1, bufSize);

	// Read all data in one call via WriteToSink.
	// After FromAdc writes bufSize to a bufSize buffer, _writeIndex wraps to 0,
	// so WriteToSink (which uses Delay(0)) reads from index 0 — correct.
	sink->Zero(bufSize, base::Audible::AUDIOSOURCE_ADC);
	chanMixer.WriteToSink(sink, bufSize);
	chanMixer.Source()->EndMultiPlay(bufSize);
	sink->EndMultiWrite(bufSize, true, base::Audible::AUDIOSOURCE_ADC);

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

	auto chanMixer = ChannelMixer(chanParams);
	auto source = std::make_shared<MockedMultiSource>(bufSize);

	auto numBlocks = (bufSize * 2) / blockSize;
	auto buf = std::vector<float>();

	for (int i = 0; i < numBlocks; i++)
	{
		chanMixer.Sink()->Zero(blockSize, base::Audible::AUDIOSOURCE_ADC);

		auto sampsToWrite = std::min((unsigned int)blockSize, source->SamplesRemaining());
		if (sampsToWrite > 0)
		{
			base::AudioWriteRequest request;
			request.samples = source->SamplesAt();
			request.numSamps = sampsToWrite;
			request.stride = 1;
			request.fadeCurrent = 0.0f;
			request.fadeNew = 1.0f;
			request.source = base::Audible::AUDIOSOURCE_ADC;
			chanMixer.Sink()->OnBlockWriteChannel(0, request, 0);
		}
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
