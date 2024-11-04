#include "ChannelMixer.h"

using namespace audio;
using namespace base;

ChannelMixer::ChannelMixer(ChannelMixerParams chanMixParams) :
	_adcMixer(std::make_shared<ChannelMixer::AdcChannelMixer>()),
	_dacMixer(std::make_shared<ChannelMixer::DacChannelMixer>())
{
	SetParams(chanMixParams);
}

ChannelMixer::~ChannelMixer()
{
}

void ChannelMixer::SetParams(ChannelMixerParams chanMixParams)
{
	_adcMixer->SetNumChannels(chanMixParams.NumInputChannels, chanMixParams.InputBufferSize);
	_dacMixer->SetNumChannels(chanMixParams.NumOutputChannels, chanMixParams.OutputBufferSize);
}

void ChannelMixer::FromAdc(float* inBuf, unsigned int numChannels, unsigned int numSamps)
{
	if (numSamps < 1 || numChannels < 1)
		return;

	for (auto chan = 0u; chan < _adcMixer->NumOutputChannels(); chan++)
	{
		auto buf = _adcMixer->Channel(chan);
		auto source = _adcMixer->SourceType();

		if ((buf) && (numChannels > chan))
		{
			auto currentOffset = 0;

			for (auto samp = 0u; samp < numSamps; samp++)
			{
				currentOffset = buf->OnMixWrite(inBuf[samp*numChannels + chan],
					0.0f,
					1.0f,
					currentOffset,
					source);
			}

			buf->EndWrite(numSamps, true);
		}
	}
}

void ChannelMixer::InitPlay(unsigned int delaySamps, unsigned int blockSize)
{
	for (auto chan = 0u; chan < _adcMixer->NumOutputChannels(); chan++)
	{
		auto buf = _adcMixer->Channel(chan);

		if (buf)
			buf->Delay(delaySamps + blockSize);
	}
}

void ChannelMixer::ToDac(float* outBuf, unsigned int numChannels, unsigned int numSamps)
{
	if (numSamps < 1 || numChannels < 1)
		return;

	for (auto chan = 0u; chan < _dacMixer->NumInputChannels(); chan++)
	{
		auto buf = _dacMixer->Channel(chan);

		if (buf)
		{
			if ((numChannels > chan) && (buf->BufSize() > 0))
			{
				auto bufIter = buf->Delay(0);
				for (auto samp = 0u; samp < numSamps; samp++)
				{
					if (bufIter == buf->End())
						bufIter = buf->Start();

					outBuf[samp * numChannels + chan] = *bufIter++;
				}
			}
		}
	}
}

void ChannelMixer::BufferMixer::SetNumChannels(unsigned int numChans, unsigned int bufSize)
{
	auto numInputs = (unsigned int)_buffers.size();

	if (numChans > numInputs)
	{
		for (auto i = 0u; i < numChans - numInputs; i++)
			_buffers.push_back(std::make_unique<AudioBuffer>(bufSize));
	}

	if (numChans < numInputs)
		_buffers.resize(numInputs);

	for (auto& buf : _buffers)
		buf->SetSize(bufSize);
}

void ChannelMixer::AdcChannelMixer::EndMultiPlay(unsigned int numSamps)
{
	for (unsigned int chan = 0; chan < NumOutputChannels(); chan++)
	{
		auto channel = OutputChannel(chan);
		channel->EndPlay(numSamps);
	}
}

unsigned int ChannelMixer::AdcChannelMixer::NumOutputChannels() const
{
	return (unsigned int)_buffers.size();
}

void ChannelMixer::DacChannelMixer::EndMultiWrite(unsigned int numSamps)
{
	EndMultiWrite(numSamps, true);
}

void ChannelMixer::DacChannelMixer::EndMultiWrite(unsigned int numSamps, bool updateIndex)
{
	for (unsigned int chan = 0; chan < NumInputChannels(); chan++)
	{
		auto channel = InputChannel(chan);
		channel->EndWrite(numSamps, updateIndex);
	}
}

unsigned int ChannelMixer::DacChannelMixer::NumInputChannels() const
{
	return (unsigned int)_buffers.size();
}

const std::shared_ptr<AudioBuffer> ChannelMixer::BufferMixer::Channel(unsigned int channel)
{
	if (channel < _buffers.size())
		return _buffers[channel];

	return std::shared_ptr<AudioBuffer>();
}

const std::shared_ptr<AudioSource> ChannelMixer::AdcChannelMixer::OutputChannel(unsigned int channel)
{
	if (channel < _buffers.size())
	{
		auto chan = _buffers[channel];
		chan->SetSourceType(SourceType());
		return chan;
	}

	auto chan = std::shared_ptr<AudioSource>();
	chan->SetSourceType(SourceType());
	return chan;
}

const std::shared_ptr<AudioSink> ChannelMixer::DacChannelMixer::InputChannel(unsigned int channel)
{
	if (channel < _buffers.size())
		return _buffers[channel];

	return std::shared_ptr<AudioSink>();
}

const std::shared_ptr<MultiAudioSource> ChannelMixer::Source()
{
	return _adcMixer;
}

const std::shared_ptr<MultiAudioSink> ChannelMixer::Sink()
{
	return _dacMixer;
}