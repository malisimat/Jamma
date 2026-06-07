#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "MidiEvent.h"
#include "MidiLoop.h"
#include "MidiNote.h"

namespace midi
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

	struct MidiOverdubLoopState
	{
		std::array<MidiEvent, MidiLoop::DefaultCapacity> SourceEvents{};
		std::size_t SourceEventCount = 0u;
		std::uint32_t SourceLoopLengthSamps = 0u;
		std::uint32_t SourceStartSample = 0u;
		std::array<MidiPunchWindow, 128u> PunchWindows{};
		std::array<MidiNoteSnapshot, 128u> SharedHeldAtPunchStart{};
		std::size_t PunchWindowCount = 0u;
		std::uint32_t ActivePunchStart = (std::numeric_limits<std::uint32_t>::max)();
		std::size_t ActivePunchLiveEventStart = 0u;
		std::array<MidiEvent, MidiLoop::DefaultCapacity> LiveEvents{};
		std::size_t LiveEventCount = 0u;
		std::uint64_t LiveDropped = 0u;
		MidiNoteSnapshot LiveHeld{};
	};

	struct MidiOverdubSession
	{
		bool Active = false;
		std::vector<MidiOverdubLoopState> Loops;
		std::array<MidiEvent, MidiLoop::DefaultCapacity> BuildScratch{};
		std::array<MidiEvent, MidiLoop::DefaultCapacity> MergeScratch{};
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
