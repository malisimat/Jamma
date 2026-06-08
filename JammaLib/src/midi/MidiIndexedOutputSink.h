#pragma once

#include <cstdint>

#include "MidiLoop.h"

namespace midi
{
	class MidiIndexedOutputSink final : public IMidiSink
	{
	public:
		MidiIndexedOutputSink(IMidiOutputSink& outputSink,
			unsigned int outputIndex,
			std::uint32_t midiBlockStart,
			std::uint32_t outputBlockStart) noexcept;

		void OnEvent(const MidiEvent& ev) noexcept override;

	private:
		IMidiOutputSink& _outputSink;
		unsigned int _outputIndex;
		std::uint32_t _midiBlockStart;
		std::uint32_t _outputBlockStart;
	};
}