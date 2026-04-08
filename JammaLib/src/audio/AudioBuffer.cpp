#include "AudioBuffer.h"

using namespace audio;

AudioBuffer::AudioBuffer() :
	AudioSource({}),
	_playIndex(0),
	_sampsRecorded(0),
	_buffer(std::vector<float>(constants::MaxBlockSize, 0.0f))
{
}

AudioBuffer::AudioBuffer(unsigned int size) :
	AudioSource({}),
	_playIndex(0),
	_sampsRecorded(0),
	_buffer(std::vector<float>(size > constants::MaxBlockSize ? constants::MaxBlockSize : size, 0.0f))
{
}

AudioBuffer::~AudioBuffer()
{
}

void AudioBuffer::EndPlay(unsigned int numSamps)
{
	auto bufSize = (unsigned int)_buffer.size();
	_playIndex += numSamps;

	if (0 == _sampsRecorded)
	{
		_playIndex = 0;
		return;
	}

	/*if (_sampsRecorded < bufSize)
	{
		if (_playIndex > _writeIndex)
			_playIndex = _writeIndex;

		return;
	}*/

	while (bufSize <= _playIndex)
		_playIndex -= bufSize;
}

void AudioBuffer::EndWrite(unsigned int numSamps, bool updateIndex)
{
	_sampsRecorded += numSamps;
	if (_sampsRecorded > _buffer.size())
		_sampsRecorded = (unsigned int)_buffer.size();

	if (updateIndex)
		_SetWriteIndex(_writeIndex + numSamps);
}

void AudioBuffer::OnBlockWrite(const base::AudioWriteRequest& request, int writeOffset)
{
	auto bufSize = (unsigned int)_buffer.size();

	if (0 == bufSize)
	{
		_writeIndex = 0;
		return;
	}

	long startIdx = (long)_writeIndex + writeOffset;
	startIdx = ((startIdx % (long)bufSize) + (long)bufSize) % (long)bufSize;

	auto idx = (unsigned int)startIdx;

	// Fast path: pure copy with contiguous stride-1 data
	if (request.fadeCurrent == 0.0f && request.fadeNew == 1.0f && request.stride == 1)
	{
		auto firstSeg = std::min(request.numSamps, bufSize - idx);
		std::copy(request.samples, request.samples + firstSeg, _buffer.begin() + idx);

		if (firstSeg < request.numSamps)
			std::copy(request.samples + firstSeg, request.samples + request.numSamps, _buffer.begin());
	}
	else
	{
		for (unsigned int i = 0; i < request.numSamps; i++)
		{
			_buffer[idx] = (request.fadeNew * request.samples[i * request.stride]) +
				(request.fadeCurrent * _buffer[idx]);

			idx++;
			if (idx >= bufSize)
				idx -= bufSize;
		}
	}
}

void AudioBuffer::SetSize(unsigned int size)
{
	_buffer.resize(size > constants::MaxBlockSize ? constants::MaxBlockSize : size);
}

void AudioBuffer::_SetWriteIndex(unsigned int index)
{
	auto bufSize = (unsigned int)_buffer.size();

	if (0 == bufSize)
	{
		_writeIndex = 0;
		return;
	}

	while (index >= bufSize)
		index-= bufSize;

	_writeIndex = index;
}

unsigned int AudioBuffer::SampsRecorded() const
{
	return _sampsRecorded;
}

unsigned int AudioBuffer::BufSize() const
{
	return (unsigned int)_buffer.size();
}

const float& AudioBuffer::operator[](unsigned int index) const
{
	return _buffer[index % _buffer.size()];
}

std::vector<float>::iterator AudioBuffer::Start()
{
	return _buffer.begin();
}

std::vector<float>::iterator AudioBuffer::End()
{
	return _buffer.end();
}

unsigned int AudioBuffer::Delay(unsigned int sampsDelay)
{
	if (0 == _sampsRecorded)
		_playIndex = 0;
	else
	{
		auto bufSize = (unsigned int)_buffer.size();
		auto sampsBehind = sampsDelay > bufSize ? bufSize : sampsDelay;
		_playIndex = sampsBehind > _writeIndex ? (_writeIndex + bufSize) - sampsBehind : _writeIndex - sampsBehind;
	}
	
	return _playIndex;
}

unsigned int AudioBuffer::PlayIndex() const
{
	return static_cast<unsigned int>(_playIndex);
}

bool AudioBuffer::IsContiguous(unsigned int startIndex, unsigned int numSamps) const
{
	return (startIndex + numSamps) <= (unsigned int)_buffer.size();
}

const float* AudioBuffer::BlockRead(unsigned int startIndex) const
{
	return &_buffer[startIndex];
}

const float* AudioBuffer::PlaybackRead(float* tempBuf, unsigned int numSamps)
{
	auto playIndex = PlayIndex();

	if (IsContiguous(playIndex, numSamps))
		return BlockRead(playIndex);

	// Buffer wraps — copy into contiguous temp buffer
	for (auto samp = 0u; samp < numSamps; samp++)
		tempBuf[samp] = (*this)[samp + playIndex];

	return tempBuf;
}
