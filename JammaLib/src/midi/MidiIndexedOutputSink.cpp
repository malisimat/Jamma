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
		MidiEvent mapped = ev;
		mapped.sampleOffset = _outputBlockStart + (ev.sampleOffset - _midiBlockStart);
		if (mapped.IsNoteOn())
		{
			const auto scaledVelocity = static_cast<int>(
				static_cast<float>(mapped.data2) * _velocityScale + 0.5f);
			mapped.data2 = static_cast<std::uint8_t>(
				std::clamp(scaledVelocity, 0, 127));
		}
		_outputSink.OnEvent(_outputIndex, mapped);
	}
}