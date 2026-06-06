#include "MidiLoop.h"

#include "../graphics/MidiModel.h"
#include "MidiNote.h"
#include "MidiQuantisation.h"
#include "../include/Constants.h"

using namespace engine;

namespace
{
	static constexpr std::uint32_t MidiModelUpdateIntervalSamps = constants::DefaultSampleRate / 30u;

}

MidiLoop::MidiLoop() noexcept
	: _eventCount(0),
	  _loopLengthSamps(0),
	  _dropped(0),
	  _revision(0),
	  _modelRevision(0),
	  _modelLengthSamps(0),
	  _state(MidiLoopState::Empty),
	  _model(nullptr),
	  _quantisedEvents(nullptr)
{
}

void MidiLoop::StartRecord() noexcept
{
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	_state = MidiLoopState::Recording;
	_held.reset();
	_quantisedEvents.store(nullptr, std::memory_order_release);
	++_revision;
}

bool MidiLoop::RecordEvent(const MidiEvent& ev) noexcept
{
	if (_state != MidiLoopState::Recording)
		return false;

	return AppendEventForBuild(ev);
}

bool MidiLoop::AppendEventForBuild(const MidiEvent& ev) noexcept
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

void MidiLoop::ReplaceRecordedEvents(const MidiEvent* events,
	std::size_t count,
	std::uint32_t loopLengthSamps)
{
	_eventCount = 0u;
	_dropped = 0u;
	_held.reset();

	if (events && count > 0u)
	{
		const auto keepCount = (count < _events.size()) ? count : _events.size();
		for (std::size_t i = 0u; i < keepCount; ++i)
			_events[i] = events[i];

		_eventCount = keepCount;
		if (count > _events.size())
			_dropped = static_cast<std::uint64_t>(count - _events.size());
	}

	_loopLengthSamps = loopLengthSamps;
	_state = MidiLoopState::Playing;
	++_revision;

	if (_quantisation.Enabled)
		PublishQuantisedEvents();
	else
		_quantisedEvents.store(nullptr, std::memory_order_release);
}

void MidiLoop::FinalizeOverdubBase(std::uint32_t loopLengthSamps)
{
	_loopLengthSamps = loopLengthSamps;
	_state = MidiLoopState::Playing;
	_held.reset();
	++_revision;

	if (_quantisation.Enabled)
		PublishQuantisedEvents();
}

void MidiLoop::EndRecord(std::uint32_t loopLengthSamps)
{
	CanonicaliseMidiPlaybackOrder(_events.data(), _eventCount);

	_loopLengthSamps = loopLengthSamps;
	_state = MidiLoopState::Playing;
	_held.reset();
	++_revision;

	// Recording just finalised the loop window. If quantisation was already armed
	// for this take, rebuild the parallel buffer against the new length now so the
	// first playback block can read it without further work.
	if (_quantisation.Enabled)
		PublishQuantisedEvents();
}

void MidiLoop::Reset() noexcept
{
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	_state = MidiLoopState::Empty;
	_held.reset();
	_quantisedEvents.store(nullptr, std::memory_order_release);
	++_revision;
}

bool MidiLoop::TryGetEvent(std::size_t index, MidiEvent& ev) const noexcept
{
	if (index >= _eventCount)
		return false;

	ev = _events[index];
	return true;
}

void MidiLoop::AttachModel(std::shared_ptr<MidiModel> model) noexcept
{
	_model = std::move(model);
	_modelRevision = 0u;
	_modelLengthSamps = 0u;
}

bool MidiLoop::UpdateModelFromEvents(std::uint32_t displayLengthSamps, bool force)
{
	return BuildModelFromEvents(displayLengthSamps, force, false);
}

bool MidiLoop::QueueModelUpdateFromEvents(std::uint32_t displayLengthSamps, bool force)
{
	return BuildModelFromEvents(displayLengthSamps, force, true);
}

bool MidiLoop::BuildModelFromEvents(std::uint32_t displayLengthSamps, bool force, bool queueUpdate)
{
	if (!_model)
		return false;

	const auto modelLength = (MidiLoopState::Playing == _state && _loopLengthSamps > 0u) ?
		_loopLengthSamps :
		displayLengthSamps;
	const auto effectiveLength = (modelLength > 0u) ? modelLength : _loopLengthSamps;
	const auto lengthDelta = (_modelLengthSamps > effectiveLength) ?
		(_modelLengthSamps - effectiveLength) :
		(effectiveLength - _modelLengthSamps);
	const auto revisionChanged = _modelRevision != _revision;

	if (!force)
	{
		if (!revisionChanged && 0u == _eventCount)
			return false;

		if (!revisionChanged)
		{
			if (effectiveLength == _modelLengthSamps)
				return false;

			if (MidiLoopState::Recording != _state || lengthDelta < MidiModelUpdateIntervalSamps)
				return false;
		}
	}

	// Visualisation must reflect what playback emits, so use the same published
	// immutable quantised buffer that the audio thread reads from.
	const auto* quantisedEvents = _quantisedEvents.load(std::memory_order_acquire);
	const MidiEvent* eventSource = quantisedEvents ? quantisedEvents->Events.data() : _events.data();

	auto spans = ExtractMidiNoteSpans(eventSource, _eventCount, effectiveLength);
	if (queueUpdate)
		_model->QueueModelUpdate(spans, effectiveLength);
	else
		_model->UpdateModel(spans, effectiveLength);
	_modelRevision = _revision;
	_modelLengthSamps = effectiveLength;

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

		// If we are at the very start of a loop iteration, flush any notes that are
		// still held from the previous iteration. This covers the case where the
		// previous ReadBlock call ended exactly at the loop boundary, so the
		// within-block wrap condition (remaining > roomInLoop) was never true and
		// FlushHeldNotes was never called. Without this, note-ons replay at the top
		// of each new iteration while the matching note-offs are never sent.
		if (loopOffset == 0u)
			FlushHeldNotes(cursor, sink);

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
	// Snapshot which event source playback is using for this scan. Quantised
	// buffers are immutable and retained by the owning MidiLoop, so this raw
	// pointer stays valid without shared_ptr control-block work on the audio path.
	const auto* quantisedEvents = _quantisedEvents.load(std::memory_order_acquire);
	const MidiEvent* events = quantisedEvents ? quantisedEvents->Events.data() : _events.data();

	// Linear scan: small N expected, and storage is contiguous.
	for (std::size_t i = 0; i < _eventCount; ++i)
	{
		const MidiEvent& src = events[i];
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

void MidiLoop::SetQuantisation(const MidiQuantisationSettings& settings)
{
	const auto previous = _quantisation;
	_quantisation = settings;

	const auto step = MidiQuantisationStepSamps(_quantisation);
	if (step > 0u && _loopLengthSamps > 0u && _eventCount > 0u)
		PublishQuantisedEvents();
	else
		_quantisedEvents.store(nullptr, std::memory_order_release);

	if (previous != _quantisation)
		++_revision;
}

void MidiLoop::PublishQuantisedEvents()
{
	const auto step = MidiQuantisationStepSamps(_quantisation);
	if (0u == step || 0u == _loopLengthSamps || 0u == _eventCount)
	{
		_quantisedEvents.store(nullptr, std::memory_order_release);
		return;
	}

	auto quantisedEvents = std::make_unique<QuantisedEventBuffer>();
	BuildQuantisedPlaybackEvents(_events.data(), _eventCount, _loopLengthSamps, step, quantisedEvents->Events.data());
	const auto* snapshot = quantisedEvents.get();
	_retainedQuantisedEvents.push_back(std::move(quantisedEvents));
	_quantisedEvents.store(snapshot, std::memory_order_release);
}
