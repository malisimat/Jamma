#include "MidiLoop.h"

using namespace engine;

MidiLoop::MidiLoop() noexcept
	: _eventCount(0),
	  _loopLengthSamps(0),
	  _dropped(0),
	  _revision(0),
	  _state(MidiLoopState::Empty)
{
}

void MidiLoop::StartRecord() noexcept
{
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	_state = MidiLoopState::Recording;
	_held.reset();
	++_revision;
}

bool MidiLoop::RecordEvent(const MidiEvent& ev) noexcept
{
	if (_state != MidiLoopState::Recording)
		return false;

	if (_eventCount >= _events.size())
	{
		++_dropped;
		return false;
	}

	_events[_eventCount++] = ev;
	++_revision;
	return true;
}

void MidiLoop::EndRecord(std::uint32_t loopLengthSamps) noexcept
{
	_loopLengthSamps = loopLengthSamps;
	_state = MidiLoopState::Playing;
	_held.reset();
	++_revision;
}

void MidiLoop::Reset() noexcept
{
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	_state = MidiLoopState::Empty;
	_held.reset();
	++_revision;
}

bool MidiLoop::TryGetEvent(std::size_t index, MidiEvent& ev) const noexcept
{
	if (index >= _eventCount)
		return false;

	ev = _events[index];
	return true;
}

void MidiLoop::ReadBlock(std::uint32_t globalSample,
                         std::uint32_t numSamples,
                         IMidiSink& sink) noexcept
{
	if (_state != MidiLoopState::Playing || 0u == _loopLengthSamps || 0u == numSamples)
		return;

	std::uint32_t remaining = numSamples;
	std::uint32_t cursor = globalSample;

	while (remaining > 0u)
	{
		const std::uint32_t loopOffset =
			static_cast<std::uint32_t>(cursor % _loopLengthSamps);
		const std::uint32_t roomInLoop = _loopLengthSamps - loopOffset;
		const std::uint32_t segment = (remaining < roomInLoop) ? remaining : roomInLoop;

		// globalBase is the absolute sample corresponding to loopOffset.
		const std::uint32_t globalBase = cursor - loopOffset;
		EmitEventsInRange(loopOffset, loopOffset + segment, globalBase, sink);

		const bool wraps = (segment == roomInLoop) && (remaining > roomInLoop);
		if (wraps)
		{
			// Wrap boundary: flush held notes at the exact wrap sample.
			const std::uint32_t wrapSample = globalBase + _loopLengthSamps;
			FlushHeldNotes(wrapSample, sink);
		}

		cursor += segment;
		remaining -= segment;
	}
}

void MidiLoop::EmitEventsInRange(std::uint32_t lo,
                                 std::uint32_t hi,
                                 std::uint32_t globalBase,
                                 IMidiSink& sink) noexcept
{
	// Linear scan: small N expected, and storage is contiguous.
	for (std::size_t i = 0; i < _eventCount; ++i)
	{
		const MidiEvent& src = _events[i];
		const std::uint32_t off = src.sampleOffset;
		if (off >= _loopLengthSamps)
			continue;
		if (off < lo || off >= hi)
			continue;

		MidiEvent out = src;
		out.sampleOffset = globalBase + off;
		sink.OnEvent(out);

		// Track held notes for wrap flushing.
		if (src.IsNoteOn())
			_held.set(NoteSlot(src.Channel(), src.data1));
		else if (src.IsNoteOff())
			_held.reset(NoteSlot(src.Channel(), src.data1));
	}
}

void MidiLoop::FlushHeldNotes(std::uint32_t atGlobalSample, IMidiSink& sink) noexcept
{
	if (_held.none())
		return;

	for (std::uint8_t channel = 0; channel < 16; ++channel)
	{
		for (std::uint8_t note = 0; note < 128; ++note)
		{
			const std::size_t slot = NoteSlot(channel, note);
			if (!_held.test(slot))
				continue;

			MidiEvent off = MidiEvent::MakeNoteOff(
				static_cast<std::uint32_t>(atGlobalSample), channel, note);
			sink.OnEvent(off);
			_held.reset(slot);
		}
	}
}
