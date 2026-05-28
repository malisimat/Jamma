#include "MidiLoop.h"

#include <iostream>

#include "MidiModel.h"
#include "MidiNoteSpan.h"
#include "MidiQuantisation.h"
#include "../include/Constants.h"

using namespace engine;

namespace
{
	static constexpr std::uint32_t MidiModelUpdateIntervalSamps = constants::DefaultSampleRate / 30u;

	const char* MidiQuantisationFractionLabel(MidiQuantisationFraction fraction) noexcept
	{
		switch (fraction)
		{
		case MidiQuantisationFraction::Whole: return "1";
		case MidiQuantisationFraction::Half: return "1/2";
		case MidiQuantisationFraction::Quarter: return "1/4";
		case MidiQuantisationFraction::Eighth: return "1/8";
		case MidiQuantisationFraction::Sixteenth: return "1/16";
		case MidiQuantisationFraction::ThirtySecond: return "1/32";
		default: return "?";
		}
	}
}

MidiLoop::MidiLoop() noexcept
	: _eventCount(0),
	  _loopLengthSamps(0),
	  _dropped(0),
	  _revision(0),
	  _modelRevision(0),
	  _modelLengthSamps(0),
	  _state(MidiLoopState::Empty),
	  _model(nullptr)
{
}

void MidiLoop::StartRecord() noexcept
{
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	_state = MidiLoopState::Recording;
	_held.reset();
	_useQuantised.store(false, std::memory_order_release);
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

	// Recording just finalised the loop window. If quantisation was already armed
	// for this take, rebuild the parallel buffer against the new length now so the
	// first playback block can read it without further work.
	if (_quantisation.Enabled)
		RebuildQuantisedEvents();
}

void MidiLoop::Reset() noexcept
{
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	_state = MidiLoopState::Empty;
	_held.reset();
	_useQuantised.store(false, std::memory_order_release);
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

	// Visualisation must reflect what playback emits, so use the same source the
	// audio thread is reading from.
	const auto useQuant = _useQuantised.load(std::memory_order_acquire);
	const MidiEvent* eventSource = useQuant ? _quantisedEvents.data() : _events.data();

	auto spans = ExtractMidiNoteSpans(eventSource, _eventCount, effectiveLength);
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
	// Snapshot which event source playback is using for this scan. Settings
	// changes flip _useQuantised to false before rebuilding the quantised buffer
	// and only back to true after the rebuild completes, so a single load here
	// gives the audio thread a consistent view for the duration of the block.
	const auto useQuant = _useQuantised.load(std::memory_order_acquire);
	const MidiEvent* events = useQuant ? _quantisedEvents.data() : _events.data();

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

void MidiLoop::SetQuantisation(const MidiQuantisationSettings& settings) noexcept
{
	const auto previous = _quantisation;
	_quantisation = settings;

	// Disable first so the audio thread reverts to the untransformed event buffer
	// while we rebuild. If the new settings produce a non-zero snap step we then
	// rebuild and re-publish. Brief gap is acceptable for a live UI tweak — and
	// is bounded by the rebuild loop (O(eventCount), no allocation).
	_useQuantised.store(false, std::memory_order_release);

	const auto step = MidiQuantisationStepSamps(_quantisation);
	if (step > 0u && _loopLengthSamps > 0u && _eventCount > 0u)
	{
		RebuildQuantisedEvents();
		_useQuantised.store(true, std::memory_order_release);
	}

	std::cout << "MidiLoop quantisation publish: enabled=" << _quantisation.Enabled
		<< " fraction=" << MidiQuantisationFractionLabel(_quantisation.Fraction)
		<< " grain=" << _quantisation.GrainSamps
		<< " step=" << step
		<< " loopLength=" << _loopLengthSamps
		<< " events=" << _eventCount
		<< " active=" << _useQuantised.load(std::memory_order_acquire) << std::endl;

	if (previous != _quantisation)
		++_revision;
}

void MidiLoop::RebuildQuantisedEvents() noexcept
{
	const auto step = MidiQuantisationStepSamps(_quantisation);
	QuantiseEvents(_events.data(), _eventCount, _loopLengthSamps, step, _quantisedEvents.data());
}
