#pragma once

#include <cstddef>
#include <cstdint>

#include "MidiEvent.h"

namespace engine
{
	struct MidiPunchWindow
	{
		std::uint32_t StartSample;
		std::uint32_t EndSample;
	};

	struct MidiOverdubRenderParams
	{
		const MidiEvent* SourceEvents = nullptr;
		std::size_t SourceEventCount = 0u;
		std::uint32_t SourceLoopLengthSamps = 0u;
		std::uint32_t TargetLoopLengthSamps = 0u;
		const MidiPunchWindow* PunchWindows = nullptr;
		std::size_t PunchWindowCount = 0u;
	};

	// Build source-derived base events for a MIDI overdub target.
	//
	// Behavior:
	// - Repeat source events across the target timeline.
	// - Remove material inside punch windows.
	// - Split crossing notes at punch boundaries and emit canonical NoteOn/NoteOff pairs.
	// - Treat velocity-zero NoteOn as NoteOff.
	// - Sort events into playback-safe order (same-sample NoteOff before NoteOn).
	//
	// Returns the number of written events (<= outCapacity).
	std::size_t BuildMidiOverdubBaseEvents(const MidiOverdubRenderParams& params,
		MidiEvent* outEvents,
		std::size_t outCapacity) noexcept;
}
