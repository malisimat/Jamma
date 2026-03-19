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

void AudioBuffer::OnPlay(const std::shared_ptr<base::AudioSink> dest,
	int indexOffset,
	unsigned int numSamps)
{
	if (0 == _sampsRecorded)
		return;

	auto bufSize = (unsigned int)_buffer.size();

	auto index = _playIndex + (long)indexOffset;
	while (index >= bufSize)
		index -= bufSize;

	auto destIndex = 0;
	auto sourceType = SourceType();

	for (auto i = 0u; i < numSamps; i++)
	{
		destIndex = dest->OnMixWrite(_buffer[index], 1.0f, 1.0f, destIndex, sourceType);

		index++;
		if (index >= bufSize)
			index -= bufSize;
	}
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

inline int AudioBuffer::OnMixWrite(float samp,
	float fadeCurrent,
	float fadeNew,
	int indexOffset,
	Audible::AudioSourceType source)
{
	auto bufSize = (unsigned int)_buffer.size();

	if (0 == bufSize)
	{
		_writeIndex = 0;
		return 0;
	}

	while (bufSize <= _writeIndex + indexOffset)
		indexOffset -= (int)_buffer.size();

	_buffer[_writeIndex + indexOffset] = (fadeNew * samp) + (fadeCurrent * _buffer[_writeIndex + indexOffset]);

	return indexOffset + 1;
}

void AudioBuffer::EndWrite(unsigned int numSamps, bool updateIndex)
{
	_sampsRecorded += numSamps;
	if (_sampsRecorded > _buffer.size())
		_sampsRecorded = (unsigned int)_buffer.size();

	if (updateIndex)
		_SetWriteIndex(_writeIndex + numSamps);
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
