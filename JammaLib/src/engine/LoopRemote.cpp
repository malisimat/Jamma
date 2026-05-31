#include "LoopRemote.h"
#include <algorithm>

using namespace engine;

LoopRemote::LoopRemote(LoopParams params,
	audio::AudioMixerParams mixerParams) :
	Loop(params, mixerParams),
	_modelDirty(false),
	_measureLengthSamps(0u),
	_measurePositionSamps(0u),
	_visualLengthSamps(0u),
	_lastVisualUpdate(std::chrono::steady_clock::now())
{
	SetMeasureLength(constants::DefaultSampleRate);
	SetVisualLength(constants::DefaultSampleRate);
	SetMeasurePosition(0u);
	// Render the remote loop once so something is visible.
	_ForceUpdateLoopModel();
}

void LoopRemote::SetVisualLength(unsigned int visualLengthSamps)
{
	auto safeVisualLength = std::max(1u, visualLengthSamps);
	if (safeVisualLength == _visualLengthSamps.load())
		return;

	_visualLengthSamps.store(safeVisualLength);
	_modelDirty.store(true);
}

void LoopRemote::Update()
{
	if (!_modelDirty.exchange(false))
		return;

	// Throttle visual model updates to _MaxVisualUpdateHz to avoid excessive
	// decimation/upload cost when multiple remote users are active.
	// The vertex shader handles displacement from the waveform texture so
	// no geometry rebuild is needed, but decimation is still O(loopLength).
	auto now = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration<double>(now - _lastVisualUpdate).count();
	if (elapsed < (1.0 / _MaxVisualUpdateHz))
	{
		// Re-set the dirty flag so the update is retried next tick.
		_modelDirty.store(true);
		_bufferBank.UpdateCapacity();
		_monitorBufferBank.UpdateCapacity();
		return;
	}

	_lastVisualUpdate = now;
	Loop::Update();
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
	_modelDirty.store(true);
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

unsigned long LoopRemote::_ModelDisplayLength(bool isRecording, unsigned long actualLoopLength) const
{
	if (isRecording)
		return actualLoopLength;

	return std::max<unsigned long>(actualLoopLength, _visualLengthSamps.load());
}

void LoopRemote::IngestSamples(const float* samples, unsigned int numSamps)
{
	// Snapshot len/pos once: standard lock-free ring-buffer pattern.
	// std::atomic loads prevent torn reads; minor position drift across a
	// block boundary if the job thread calls SetMeasurePosition concurrently
	// is inherent to lock-free audio and acceptable for ninjam.
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

	// _playState/_loopLength/_playIndex are inherited plain fields from Loop.
	// They are not atomic, but are word-sized and only ever assigned (not
	// read-modify-written) from both threads, so no torn read is possible
	// on x86-64/MSVC. Making them atomic in Loop is a wider architecture
	// change beyond the scope of this PR.
	_playState = STATE_PLAYING;
	_loopLength = len;
	_playIndex = constants::MaxLoopFadeSamps + pos;

	// Do not rebuild LoopModel/VU geometry here: this path is used from audio ingest
	// and must remain real-time-safe. Remote visuals are refreshed only when the
	// interval size changes, not for every arriving audio block.
}

