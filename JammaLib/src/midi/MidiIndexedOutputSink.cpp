#include "MidiIndexedOutputSink.h"

namespace midi
{
	MidiIndexedOutputSink::MidiIndexedOutputSink(IMidiOutputSink& outputSink,
		unsigned int outputIndex,
		std::uint32_t midiBlockStart,
		std::uint32_t outputBlockStart) noexcept :
		_outputSink(outputSink),
		_outputIndex(outputIndex),
		_midiBlockStart(midiBlockStart),
		_outputBlockStart(outputBlockStart)
	{
	}

	void MidiIndexedOutputSink::OnEvent(const MidiEvent& ev) noexcept
	{
		MidiEvent mapped = ev;
		mapped.sampleOffset = _outputBlockStart + (ev.sampleOffset - _midiBlockStart);
		_outputSink.OnEvent(_outputIndex, mapped);
	}
}