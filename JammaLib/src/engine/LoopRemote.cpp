#include "LoopRemote.h"
#include <algorithm>

using namespace engine;

LoopRemote::LoopRemote(LoopParams params,
	audio::AudioMixerParams mixerParams) :
	Loop(params, mixerParams),
	_measureLengthSamps(constants::DefaultSampleRate),
	_measurePositionSamps(0u)
{
	SetMeasureLength(_measureLengthSamps);
	SetMeasurePosition(0u);
}

void LoopRemote::SetMeasureLength(unsigned int measureLengthSamps)
{
	auto safeMeasureLength = std::max(1u, measureLengthSamps);
	if (safeMeasureLength == _measureLengthSamps)
		return;

	_measureLengthSamps = safeMeasureLength;

	auto targetLength = constants::MaxLoopFadeSamps + static_cast<unsigned long>(_measureLengthSamps);
	_bufferBank.Resize(targetLength);
	_monitorBufferBank.Resize(targetLength);
	_bufferBank.SetLength(targetLength);
	_monitorBufferBank.SetLength(targetLength);

	_loopLength = _measureLengthSamps;
	_playState = STATE_PLAYING;
	_playIndex = constants::MaxLoopFadeSamps;
	_UpdateLoopModel();
}

void LoopRemote::SetMeasurePosition(unsigned int positionSamps)
{
	if (_measureLengthSamps == 0u)
	{
		_measurePositionSamps = 0u;
		return;
	}

	_measurePositionSamps = positionSamps % _measureLengthSamps;
	_loopLength = _measureLengthSamps;
	_playState = STATE_PLAYING;
	_playIndex = constants::MaxLoopFadeSamps + _measurePositionSamps;
}

void LoopRemote::IngestSamples(const float* samples, unsigned int numSamps)
{
	if (!samples || numSamps == 0u || _measureLengthSamps == 0u)
		return;

	const auto baseIndex = constants::MaxLoopFadeSamps;
	for (auto samp = 0u; samp < numSamps; samp++)
	{
		auto writeIndex = baseIndex + ((_measurePositionSamps + samp) % _measureLengthSamps);
		_bufferBank[writeIndex] = samples[samp];
		_monitorBufferBank[writeIndex] = samples[samp];
	}

	_measurePositionSamps = (_measurePositionSamps + numSamps) % _measureLengthSamps;
	_playState = STATE_PLAYING;
	_loopLength = _measureLengthSamps;
	_playIndex = constants::MaxLoopFadeSamps + _measurePositionSamps;
	_UpdateLoopModel();
}
