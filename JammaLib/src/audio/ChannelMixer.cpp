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
    _historyChannels.resize(chanMixParams.NumInputChannels);
    _historyValid = 0;
    _historyStarted = false;
    _historyIngestedFrames = 0;
}

void ChannelMixer::FromAdc(float* inBuf, unsigned int numChannels, unsigned int numSamps)
{
	_historyIngestedFrames = inBuf && numChannels == _historyChannels.size() ? numSamps : 0u;
	if (!inBuf || numSamps < 1 || numChannels < 1)
		return;

	for (auto chan = 0u; chan < _adcMixer->NumOutputChannels(Audible::AUDIOSOURCE_ADC); chan++)
	{
		const auto buf = _adcMixer->Channel(chan);

		if ((buf) && (numChannels > chan))
		{
			AudioWriteRequest request;
			request.samples = &inBuf[chan];
			request.numSamps = numSamps;
			request.stride = numChannels;
			request.fadeCurrent = 0.0f;
			request.fadeNew = 1.0f;
			request.source = _adcMixer->SourceType();

			buf->OnBlockWrite(request, 0);
			buf->EndWrite(numSamps, true);
		}
	}
}

InputHistoryView ChannelMixer::ObserveInputHistory(std::uint64_t start, unsigned int count,
    unsigned int latency, bool ingested) noexcept
{
    ingested = ingested && count > 0 && count == _historyIngestedFrames;
    _historyIngestedFrames = 0;
    if (!_historyStarted) { _historyEpoch = start; _historyStarted = true; }
    if (!ingested || start != _historyHead) _historyValid = 0;
    _historyHead = start + count;
    if (!ingested) return {};
    unsigned int capacity = 0;
    for (auto channel = 0u; channel < _historyChannels.size(); ++channel)
    {
        _historyChannels[channel] = _adcMixer->HistoryChannel(channel);
        capacity = channel == 0 ? _historyChannels[channel].Capacity :
            (std::min)(capacity, _historyChannels[channel].Capacity);
    }
    _historyValid = static_cast<unsigned int>((std::min)(static_cast<std::uint64_t>(capacity),
        static_cast<std::uint64_t>(_historyValid) + count));
    return {_historyChannels.data(), static_cast<unsigned int>(_historyChannels.size()),
        _historyValid, latency, _historyHead, _historyEpoch};
}

void ChannelMixer::InitPlay(unsigned int delaySamps, unsigned int blockSize)
{
	delaySamps+= blockSize;

	for (auto chan = 0u; chan < _adcMixer->NumOutputChannels(Audible::AUDIOSOURCE_ADC); chan++)
	{
		const auto buf = _adcMixer->Channel(chan);

		if (buf)
			buf->Delay(delaySamps);
	}
}

void ChannelMixer::WriteToSink(const std::shared_ptr<MultiAudioSink> dest, unsigned int numSamps)
{
	auto sourceType = _adcMixer->SourceType();

	for (auto chan = 0u; chan < _adcMixer->NumOutputChannels(sourceType); chan++)
	{
		const auto buf = _adcMixer->Channel(chan);

		if (buf && buf->SampsRecorded() > 0)
		{
			auto bufSize = buf->BufSize();
			if (0 == bufSize)
				continue;

			auto playIndex = buf->PlayIndex();

			AudioWriteRequest request;
			request.fadeCurrent = 1.0f;
			request.fadeNew = 1.0f;
			request.source = sourceType;
			request.stride = 1;

			if (buf->IsContiguous(playIndex, numSamps))
			{
				request.samples = buf->BlockRead(playIndex);
				request.numSamps = numSamps;
				dest->OnBlockWriteChannel(chan, request, 0);
			}
			else
			{
				// Handle wrap-around: split into two contiguous writes
				auto sampsToEnd = bufSize - playIndex;
				auto firstChunk = (numSamps < sampsToEnd) ? numSamps : sampsToEnd;

				request.samples = buf->BlockRead(playIndex);
				request.numSamps = firstChunk;
				dest->OnBlockWriteChannel(chan, request, 0);

				if (firstChunk < numSamps)
				{
					request.samples = buf->BlockRead(0);
					request.numSamps = numSamps - firstChunk;
					dest->OnBlockWriteChannel(chan, request, (int)firstChunk);
				}
			}
		}
	}
}

void ChannelMixer::ToDac(float* outBuf, unsigned int numChannels, unsigned int numSamps)
{
	if (numSamps < 1 || numChannels < 1)
		return;

	for (auto chan = 0u; chan < _dacMixer->NumInputChannels(Audible::AUDIOSOURCE_MIXER); chan++)
	{
		auto buf = _dacMixer->Channel(chan);

		if (buf)
		{
			if ((numChannels > chan) && (buf->BufSize() > 0))
			{
				auto playIndex = buf->Delay(0);

				if (buf->IsContiguous(playIndex, numSamps))
				{
					auto ptr = buf->BlockRead(playIndex);
					auto offset = chan;
					for (auto samp = 0u; samp < numSamps; samp++)
					{
						outBuf[offset] = ptr[samp];
						offset += numChannels;
					}
				}
				else
				{
					auto offset = chan;
					for (auto samp = 0u; samp < numSamps; samp++)
					{
						outBuf[offset] = (*buf)[samp + playIndex];
						offset += numChannels;
					}
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
		_buffers.resize(numChans);

	for (auto& buf : _buffers)
	{
		buf->SetSize(bufSize);
		// Device-latency scratch must not replay audio from the previous stream.
		buf->Reset();
	}
}

void ChannelMixer::AdcChannelMixer::EndMultiPlay(unsigned int numSamps)
{
	for (unsigned int chan = 0; chan < NumOutputChannels(_sourceParams.SourceType); chan++)
	{
		auto& channel = _OutputChannel(chan);
		if (channel)
			channel->EndPlay(numSamps);
	}
}

unsigned int ChannelMixer::AdcChannelMixer::NumOutputChannels(Audible::AudioSourceType source) const
{
	return (unsigned int)_buffers.size();
}

void ChannelMixer::DacChannelMixer::EndMultiWrite(unsigned int numSamps,
	Audible::AudioSourceType source)
{
	EndMultiWrite(numSamps, true, source);
}

void ChannelMixer::DacChannelMixer::EndMultiWrite(unsigned int numSamps,
	bool updateIndex,
	Audible::AudioSourceType source)
{
	for (unsigned int chan = 0; chan < NumInputChannels(source); chan++)
	{
		const auto& channel = _InputChannel(chan, source);
		channel->EndWrite(numSamps, updateIndex);
	}
}

unsigned int ChannelMixer::DacChannelMixer::NumInputChannels(Audible::AudioSourceType source) const
{
	return (unsigned int)_buffers.size();
}

const std::shared_ptr<AudioBuffer> ChannelMixer::BufferMixer::Channel(unsigned int channel)
{
	if (channel < _buffers.size())
		return _buffers[channel];

	return std::shared_ptr<AudioBuffer>();
}

const std::shared_ptr<AudioSource> ChannelMixer::AdcChannelMixer::_OutputChannel(unsigned int channel)
{
	if (channel < _buffers.size())
	{
		auto& chan = _buffers[channel];
		if (chan)
		{
			chan->SetSourceType(SourceType());
			return chan;
		}
	}

	return nullptr;
}

const std::shared_ptr<AudioSink> ChannelMixer::DacChannelMixer::_InputChannel(unsigned int channel,
	Audible::AudioSourceType source)
{
	if (channel < _buffers.size())
		return _buffers[channel];

	return nullptr;
}

const std::shared_ptr<MultiAudioSource> ChannelMixer::Source()
{
	return _adcMixer;
}

const std::shared_ptr<MultiAudioSink> ChannelMixer::Sink()
{
	return _dacMixer;
}
