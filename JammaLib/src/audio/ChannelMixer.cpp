#include "ChannelMixer.h"

ChannelMixer::ChannelMixer(ChannelMixerParams chanMixParams) :
	MultiAudible(MultiAudibleParams{}),
	_inputBuffers({})
{
	SetParams(chanMixParams);
}

ChannelMixer::~ChannelMixer()
{
}

void ChannelMixer::SetParams(ChannelMixerParams chanMixParams)
{
	auto numInputs = (unsigned int)_inputBuffers.size();

	if (chanMixParams.NumInputChannels > numInputs)
	{
		for (auto i = 0U; i < chanMixParams.NumInputChannels - numInputs; i++)
		{
			_inputBuffers.push_back(AudioBuffer(chanMixParams.InputBufferSize));
		}
	}

	if (chanMixParams.NumInputChannels < numInputs)
		_inputBuffers.resize(numInputs);

	auto numOutputs = (unsigned int)_outputBuffers.size();

	if (chanMixParams.NumOutputChannels > numOutputs)
	{
		for (auto i = 0U; i < chanMixParams.NumOutputChannels - numOutputs; i++)
		{
			_outputBuffers.push_back(AudioBuffer(chanMixParams.OutputBufferSize));
		}
	}

	if (chanMixParams.NumOutputChannels < numOutputs)
		_outputBuffers.resize(numInputs);

	for (auto buf : _inputBuffers)
		buf.SetSize(chanMixParams.OutputBufferSize);

	for (auto buf : _inputBuffers)
		buf.SetSize(chanMixParams.OutputBufferSize);
}

void ChannelMixer::FromAdc(float* inBuf, unsigned int numChannels, unsigned int numSamps)
{
	if (numSamps < 1 || numChannels < 1)
		return;

	auto chan = 0U;
	for (auto buf : _inputBuffers)
	{
		chan++;

		if (numChannels > chan)
		{
			for (auto samp = 0U; samp < numSamps; samp++)
			{
				buf.Push(inBuf[samp*numChannels + chan]);
			}
		}
	}
}

void ChannelMixer::ToDac(float* outBuf, unsigned int numChannels, unsigned int numSamps)
{
	if (numSamps < 1 || numChannels < 1)
		return;

	auto chan = 0U;
	for (auto buf : _outputBuffers)
	{
		chan++;

		if ((numChannels > chan) && (buf.BufSize() > 0))
		{
			auto bufIter = buf.Delay(numSamps);
			for (auto samp = 0U; samp < numSamps; samp++)
			{
				if (bufIter == buf.End())
					bufIter = buf.Start();

				outBuf[samp*numChannels + chan] = *bufIter++;
			}
		}
	}
}