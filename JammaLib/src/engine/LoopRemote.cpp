#include "LoopRemote.h"
#include <algorithm>

using namespace engine;

LoopRemote::LoopRemote(LoopParams params,
	audio::AudioMixerParams mixerParams) :
	Loop(params, mixerParams),
	_measureLengthSamps(constants::DefaultSampleRate),
	_measurePositionSamps(0u)
{
	SetMeasureLength(_measureLengthSamps.load());
	SetMeasurePosition(0u);
}

void LoopRemote::SetMeasureLength(unsigned int measureLengthSamps)
{
	auto safeMeasureLength = std::max(1u, measureLengthSamps);
	if (safeMeasureLength == _measureLengthSamps.load())
		return;

	_measureLengthSamps.store(safeMeasureLength);

	auto targetLength = constants::MaxLoopFadeSamps + static_cast<unsigned long>(safeMeasureLength);
	_bufferBank.Resize(targetLength);
	_monitorBufferBank.Resize(targetLength);
	_bufferBank.SetLength(targetLength);
	_monitorBufferBank.SetLength(targetLength);

	_loopLength = safeMeasureLength;
	_playState = STATE_PLAYING;
	_playIndex = constants::MaxLoopFadeSamps;
	_UpdateLoopModel();
}

void LoopRemote::SetMeasurePosition(unsigned int positionSamps)
{
	const auto len = _measureLengthSamps.load();
	if (len == 0u)
	{
		_measurePositionSamps.store(0u);
		return;
	}

	const auto pos = positionSamps % len;
	_measurePositionSamps.store(pos);
	_loopLength = len;
	_playState = STATE_PLAYING;
	_playIndex = constants::MaxLoopFadeSamps + pos;
}

void LoopRemote::IngestSamples(const float* samples, unsigned int numSamps)
{
	const auto len = _measureLengthSamps.load();
	if (!samples || numSamps == 0u || len == 0u)
		return;

	const auto baseIndex = constants::MaxLoopFadeSamps;
	auto pos = _measurePositionSamps.load();
	for (auto samp = 0u; samp < numSamps; samp++)
	{
		auto writeIndex = baseIndex + ((pos + samp) % len);
		_bufferBank[writeIndex] = samples[samp];
		_monitorBufferBank[writeIndex] = samples[samp];
	}

	pos = (pos + numSamps) % len;
	_measurePositionSamps.store(pos);
	_playState = STATE_PLAYING;
	_loopLength = len;
	_playIndex = constants::MaxLoopFadeSamps + pos;

	// Do not rebuild LoopModel/VU geometry here: this path is used from audio ingest
	// and must remain real-time-safe. Refresh should be deferred to a non-audio thread.
}
