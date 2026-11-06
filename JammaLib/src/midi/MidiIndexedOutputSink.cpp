#include "MidiIndexedOutputSink.h"

#include <algorithm>

namespace midi
{
	MidiIndexedOutputSink::MidiIndexedOutputSink(IMidiOutputSink& outputSink,
		unsigned int outputIndex,
		std::uint32_t midiBlockStart,
		std::uint32_t outputBlockStart,
		float velocityScale) noexcept :
		_outputSink(outputSink),
		_outputIndex(outputIndex),
		_midiBlockStart(midiBlockStart),
		_outputBlockStart(outputBlockStart),
		_velocityScale(velocityScale)
	{
	}

	void MidiIndexedOutputSink::OnEvent(const MidiEvent& ev) noexcept
	{
		(void)TryOnEvent(ev);
	}

	void MidiIndexedOutputSink::SetPlaybackRate(const audio::PlaybackRateBlock& block,
		double recordingRate, double sourceFraction) noexcept
	{
		_rateBlock = &block;
		_recordingRate = recordingRate;
		_sourceFraction = sourceFraction;
		_rateCursor = block.Cursor();
		_sourceThrough = sourceFraction;
        _sourceFrameStart = sourceFraction;
		_rateFrame = 0u;
		_lastSourceOffset = 0;
	}

	std::uint32_t MidiIndexedOutputSink::_FrameForSource(std::uint32_t source) noexcept
	{
		const auto offset = static_cast<std::int32_t>(source - _midiBlockStart);
		if (!_rateBlock) return static_cast<std::uint32_t>(offset);
		if (offset <= _sourceFraction) { _sourceFrameStart = _sourceFraction; return 0u; }
		if (offset < _lastSourceOffset)
		{
			_rateCursor = _rateBlock->Cursor();
			_sourceThrough = _sourceFraction;
            _sourceFrameStart = _sourceFraction;
			_rateFrame = 0u;
		}
		_lastSourceOffset = offset;
		while (offset >= _sourceThrough && _rateFrame < _rateBlock->Frames)
		{
			_sourceFrameStart = _sourceThrough;
            _sourceThrough += _rateCursor->Next() / _recordingRate;
			++_rateFrame;
		}
		return _rateFrame ? _rateFrame - 1u : 0u;
	}

	bool MidiIndexedOutputSink::TryOnEvent(const MidiEvent& ev) noexcept
	{
		return TryOnVoiceEvent(ev, {});
	}

	bool MidiIndexedOutputSink::TryOnVoiceEvent(const MidiEvent& ev, const MidiVoiceEvent& voice) noexcept
	{
		MidiEvent mapped = ev;
		mapped.sampleOffset = _outputBlockStart + _FrameForSource(ev.sampleOffset);
		if (mapped.IsNoteOn())
		{
			const auto scaledVelocity = static_cast<int>(
				static_cast<float>(mapped.data2) * _velocityScale + 0.5f);
			mapped.data2 = static_cast<std::uint8_t>(
				std::clamp(scaledVelocity, 0, 127));
			// A positive source attack that becomes velocity zero would be a MIDI
			// release. Do not deliver it or mark the source note as sounding.
			if (!mapped.IsNoteOn()) return false;
		}
		auto provenance = voice;
        if (_rateBlock)
            provenance.SourceFrameResidual = static_cast<std::int32_t>(ev.sampleOffset - _midiBlockStart) - _sourceFrameStart;
        return _outputSink.TryOnVoiceEvent(_outputIndex, mapped, provenance);
	}
}
