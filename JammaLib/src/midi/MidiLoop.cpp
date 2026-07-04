#include "MidiLoop.h"

#include <algorithm>
#include <limits>

#include "../graphics/MidiModel.h"
#include "MidiNote.h"
#include "MidiQuantisation.h"
#include "../include/Constants.h"

using namespace midi;

float MidiLoop::MsToLoopFrac(float ms, float sampleRate, std::uint32_t loopLengthSamps) noexcept
{
	if (0u == loopLengthSamps || sampleRate <= 0.0f)
		return 0.0f;

	const auto frac = (ms * sampleRate / 1000.0f) / static_cast<float>(loopLengthSamps);
	if (frac <= 0.0f) return 0.0f;
	if (frac >= 1.0f) return 1.0f;
	return frac;
}

bool MidiLoop::FracIsAheadWithin(float candidateFrac,
	float startFrac,
	float windowFrac) noexcept
{
	if (windowFrac <= 0.0f) return false;
	if (windowFrac >= 1.0f) return true;

	float ahead = candidateFrac - startFrac;
	if (ahead < 0.0f) ahead += 1.0f;
	return ahead < windowFrac;
}

float MidiLoop::ClampAutomationFrac(double frac) noexcept
{
	auto fracF = static_cast<float>(frac);
	if (fracF < 0.0f)
		return 0.0f;
	if (fracF > 1.0f)
		return 1.0f;
	return fracF;
}

void MidiLoop::InsertOrUpdateAutomationPoint(std::array<std::pair<float, float>, AutomationLane::MaxPoints>& points,
	std::size_t& count,
	float sampleRate,
	std::uint32_t loopLengthSamps,
	float frac,
	float value) noexcept
{
	const auto automationFracEpsilon = MsToLoopFrac(AutomationMergeWindowMs, sampleRate, loopLengthSamps);

	// Find the first point at or ahead of frac (sorted array; insertAt == count means all are behind).
	std::size_t insertAt = 0u;
	while (insertAt < count && points[insertAt].first < frac)
		++insertAt;

	// Merge into the nearest existing point if it falls within the snap window.
	// Prefer the ahead point over the behind point: it's what playback encounters next,
	// and on a running write-head it's more likely to be in the overwrite zone.
	if (insertAt < count && (points[insertAt].first - frac) <= automationFracEpsilon)
	{
		// Loop invariant guarantees points[insertAt].first >= frac, so the difference is non-negative.
		points[insertAt].second = value;
	}
	else if (insertAt > 0u && (frac - points[insertAt - 1u].first) <= automationFracEpsilon)
	{
		points[insertAt - 1u].second = value;
	}
	else if (count < AutomationLane::MaxPoints)
	{
		// Shift right and insert in sorted order.
		for (std::size_t i = count; i > insertAt; --i)
			points[i] = points[i - 1u];
		points[insertAt] = std::make_pair(frac, value);
		++count;
	}
	else if (count > 0u)
	{
		// Array is full and no nearby point to merge into — must evict one entry.
		// Prefer evicting ahead of the write-head (likely overwrite territory),
		// but protect the immediate future hold region used by editor automation.
		const auto protectWindowFrac = MsToLoopFrac(AutomationFutureProtectWindowMs, sampleRate, loopLengthSamps);
		std::size_t evictAt = count; // sentinel: no candidate yet

		// Pass 1: scan forward from the insertion position for the first ahead-point
		// that lies outside the protect window (i.e., far enough ahead to be safe to lose).
		for (std::size_t i = insertAt; i < count; ++i)
		{
			if (!FracIsAheadWithin(points[i].first, frac, protectWindowFrac))
			{
				evictAt = i;
				break;
			}
		}

		// Pass 2 (fallback): all ahead points are protected, so scan backward from the
		// insertion position looking for a point that is NOT in the wrap-around future
		// protect window (i.e., sufficiently far behind in the loop).
		// Guard skipped when protectWindowFrac == 0: in that case Pass 1 always succeeds
		// immediately since FracIsAheadWithin always returns false.
		if (protectWindowFrac > 0.0f && evictAt == count)
		{
			for (std::size_t i = insertAt; i > 0u; --i)
			{
				const auto idx = i - 1u;
				if (!FracIsAheadWithin(points[idx].first, frac, protectWindowFrac))
				{
					evictAt = idx;
					break;
				}
			}
		}

		// Last resort: everything is within the protect window (e.g. all points clustered
		// around the write-head). Evict the nearest-ahead point, or index 0 if frac is
		// past all existing points.
		if (evictAt == count)
			evictAt = (insertAt < count) ? insertAt : 0u;

		// Compact the array by removing the evicted entry.
		for (std::size_t i = evictAt + 1u; i < count; ++i)
			points[i - 1u] = points[i];
		--count;

		// Re-scan for the insertion position: evicting a point before the original
		// insertAt shifts all subsequent indices, so we cannot reuse the old value.
		insertAt = 0u;
		while (insertAt < count && points[insertAt].first < frac)
			++insertAt;

		for (std::size_t i = count; i > insertAt; --i)
			points[i] = points[i - 1u];
		points[insertAt] = std::make_pair(frac, value);
		++count;
	}
}

float MidiLoop::SampleToAutomationFrac(std::uint32_t sample,
	std::uint32_t loopLengthSamps) noexcept
{
	if (0u == loopLengthSamps)
		return 0.0f;
	return static_cast<float>(sample % loopLengthSamps)
		/ static_cast<float>(loopLengthSamps);
}

bool MidiLoop::FracWithinOverwriteWindow(float frac,
	float startFrac,
	float endFrac,
	bool wraps) noexcept
{
	if (!wraps)
		return frac >= startFrac && frac < endFrac;

	return frac >= startFrac || frac < endFrac;
}

MidiLoop::MidiLoop() noexcept
	: _eventCount(0),
	  _sampleRate(static_cast<float>(constants::DefaultSampleRate)),
	  _loopLengthSamps(0),
	  _loopPhaseAnchor(0),
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

	MidiNote::SortMidiEvents(_events.data(), _eventCount);
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
	MidiNote::SortMidiEvents(_events.data(), _eventCount);
	_loopLengthSamps = loopLengthSamps;
	_state = MidiLoopState::Playing;
	_held.reset();
	++_revision;

	if (_quantisation.Enabled)
		PublishQuantisedEvents();
}

void MidiLoop::EndRecord(std::uint32_t loopLengthSamps, std::uint32_t startGlobalSample)
{
	MidiNote::SortMidiEvents(_events.data(), _eventCount);

	_loopLengthSamps = loopLengthSamps;
	_loopPhaseAnchor = startGlobalSample;
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

void MidiLoop::AttachModel(std::shared_ptr<graphics::MidiModel> model) noexcept
{
	auto publishedModel = std::move(model);
	if (publishedModel)
		publishedModel->SetAutomationSource(this);
	_model.store(std::move(publishedModel), std::memory_order_release);
	_modelRevision = 0u;
	_modelLengthSamps = 0u;
}

std::shared_ptr<graphics::MidiModel> MidiLoop::Model() const noexcept
{
	return _model.load(std::memory_order_acquire);
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
	auto model = Model();
	if (!model)
		return false;

	const auto modelLength = (MidiLoopState::Playing == _state && _loopLengthSamps > 0u) ?
		_loopLengthSamps :
		displayLengthSamps;
	auto effectiveLength = (modelLength > 0u) ? modelLength : _loopLengthSamps;
	if (0u == effectiveLength && _eventCount > 0u)
	{
		// If loop length is not yet resolved (for example during arming/transition
		// windows), derive a temporary display span from recorded events so notes
		// remain visible instead of disappearing.
		const auto* quantisedEvents = _quantisedEvents.load(std::memory_order_acquire);
		const MidiEvent* eventSource = quantisedEvents ? quantisedEvents->Events.data() : _events.data();
		std::uint32_t maxOffset = 0u;
		for (std::size_t i = 0; i < _eventCount; ++i)
			if (eventSource[i].sampleOffset > maxOffset)
				maxOffset = eventSource[i].sampleOffset;

		constexpr std::uint32_t MaxUint32 = 0xFFFFFFFFu;
		effectiveLength = (maxOffset < MaxUint32) ? (maxOffset + 1u) : maxOffset;
	}
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
		else if (MidiLoopState::Recording == _state && lengthDelta < MidiModelUpdateIntervalSamps)
		{
			// During recording, throttle model rebuilds so we don't rescan
			// all events on every incoming MIDI event (avoids O(N²) cost).
			return false;
		}
	}

	// Visualisation must reflect what playback emits, so use the same published
	// immutable quantised buffer that the audio thread reads from.
	const auto* quantisedEvents = _quantisedEvents.load(std::memory_order_acquire);
	const MidiEvent* eventSource = quantisedEvents ? quantisedEvents->Events.data() : _events.data();

	auto spans = MidiNote::ExtractSpans(eventSource, _eventCount, effectiveLength);
	if (queueUpdate && !force)
		model->QueueModelUpdate(spans, effectiveLength);
	else
		model->UpdateModel(spans, effectiveLength);
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

	const auto step = MidiQuantisation::StepSamps(_quantisation);
	if (step > 0u && _loopLengthSamps > 0u && _eventCount > 0u)
		PublishQuantisedEvents();
	else
		_quantisedEvents.store(nullptr, std::memory_order_release);

	if (previous != _quantisation)
		++_revision;
}

void MidiLoop::PublishQuantisedEvents()
{
	const auto step = MidiQuantisation::StepSamps(_quantisation);
	if (0u == step || 0u == _loopLengthSamps || 0u == _eventCount)
	{
		_quantisedEvents.store(nullptr, std::memory_order_release);
		return;
	}

	auto quantisedEvents = std::make_unique<QuantisedEventBuffer>();
	MidiQuantisation::BuildQuantisedPlaybackEvents(_events.data(),
		_eventCount,
		_loopLengthSamps,
		step,
		quantisedEvents->Events.data(),
		_quantisation.PhaseOffsetSamps);
	const auto* snapshot = quantisedEvents.get();
	_retainedQuantisedEvents.push_back(std::move(quantisedEvents));
	_quantisedEvents.store(snapshot, std::memory_order_release);
}

void MidiLoop::SetAutomationValueAtFrac(std::size_t laneIdx, double frac, float value) noexcept
{
	if (laneIdx >= MaxAutomationLanes)
		return;

	auto& lane = _lanes[laneIdx];
	auto fracF = ClampAutomationFrac(frac);

	auto& points = lane.Points;
	auto& count = lane.PointCount;

	// Open the seqlock (odd) so a concurrent render-thread snapshot retries rather
	// than observing a half-shifted buffer, then close it (even) on every exit.
	const auto gen = lane.Revision.load(std::memory_order_relaxed);
	lane.Revision.store(gen + 1u, std::memory_order_release);

	InsertOrUpdateAutomationPoint(points, count, _sampleRate, _loopLengthSamps, fracF, value);

	lane.Revision.store(gen + 2u, std::memory_order_release);
}

void MidiLoop::OverwriteAutomationWindow(std::size_t laneIdx,
	std::uint32_t startSample,
	std::uint32_t durationSamples,
	float value) noexcept
{
	if (laneIdx >= MaxAutomationLanes || 0u == _loopLengthSamps)
		return;

	auto& lane = _lanes[laneIdx];
	auto& points = lane.Points;
	auto& count = lane.PointCount;

	const auto startWrapped = startSample % _loopLengthSamps;
	const auto durationWrapped = durationSamples % _loopLengthSamps;
	const bool overwritesFullLoop = durationSamples >= _loopLengthSamps && durationWrapped == 0u;
	const auto endWrapped = (startWrapped + durationWrapped) % _loopLengthSamps;
	const auto startFrac = SampleToAutomationFrac(startWrapped, _loopLengthSamps);
	const auto endFrac = SampleToAutomationFrac(endWrapped, _loopLengthSamps);
	const bool wraps = !overwritesFullLoop && durationWrapped > 0u && endWrapped <= startWrapped;

	const auto gen = lane.Revision.load(std::memory_order_relaxed);
	lane.Revision.store(gen + 1u, std::memory_order_release);

	if (overwritesFullLoop)
	{
		count = 0u;
	}
	else
	{
		// Compact in place: keep points outside the window, preserving sort order.
		// Two-pointer pass over the sorted buffer — no temporary copy, no allocation.
		std::size_t keptCount = 0u;
		for (std::size_t i = 0u; i < count; ++i)
		{
			if (FracWithinOverwriteWindow(points[i].first, startFrac, endFrac, wraps))
				continue;

			points[keptCount++] = points[i];
		}
		count = keptCount;
	}

	InsertOrUpdateAutomationPoint(points, count, _sampleRate, _loopLengthSamps, startFrac, value);
	InsertOrUpdateAutomationPoint(points, count, _sampleRate, _loopLengthSamps, endFrac, value);

	lane.Revision.store(gen + 2u, std::memory_order_release);
}

float MidiLoop::GetAutomationValueAtCursor(std::size_t laneIdx, double frac, std::uint16_t& cursorIdx) const noexcept
{
	if (laneIdx >= MaxAutomationLanes)
		return 0.0f;

	const auto& lane = _lanes[laneIdx];
	const auto fracF = static_cast<float>(frac);

	// Seqlock read: retry while the MIDI-thread writer holds the lock (odd generation)
	// or the buffer shifted under us. Bounded to avoid blocking the audio path; on
	// give-up the cursor is left unchanged and the last committed value is returned.
	for (int attempt = 0; attempt < 8; ++attempt)
	{
		const auto gen0 = lane.Revision.load(std::memory_order_acquire);
		if (gen0 & 1u)
			continue; // Writer in progress — spin once more.

		const auto count = lane.PointCount;
		if (0u == count)
		{
			const auto gen1 = lane.Revision.load(std::memory_order_acquire);
			if (gen0 == gen1)
				return 0.0f;
			continue;
		}

		const auto& points = lane.Points;

		// Stage cursor updates locally; only commit if the generation validates.
		auto localCursor = cursorIdx;

		// Clamp cursor into range and reset on loop wrap (frac stepped backward).
		if (localCursor >= count)
			localCursor = 0u;
		if (fracF < points[localCursor].first)
			localCursor = 0u;

		// Advance forward while the next point still starts at or before frac.
		while ((localCursor + 1u) < count && points[localCursor + 1u].first <= fracF)
			++localCursor;

		// Before the first point: hold the first value. At/after the last: hold last.
		float result;
		if (fracF <= points[0].first)
		{
			result = points[0].second;
		}
		else if ((localCursor + 1u) >= count)
		{
			result = points[count - 1u].second;
		}
		else
		{
			const auto lo = points[localCursor];
			const auto hi = points[localCursor + 1u];
			const auto span = hi.first - lo.first;
			result = (span <= 0.0f) ? hi.second
			                        : lo.second + (fracF - lo.first) / span * (hi.second - lo.second);
		}

		const auto gen1 = lane.Revision.load(std::memory_order_acquire);
		if (gen0 == gen1)
		{
			cursorIdx = static_cast<std::uint16_t>(localCursor);
			return result;
		}
	}

	return 0.0f;
}

void MidiLoop::ClearAutomationLane(std::size_t laneIdx) noexcept
{
	if (laneIdx >= MaxAutomationLanes)
		return;

	auto& lane = _lanes[laneIdx];
	const auto gen = lane.Revision.load(std::memory_order_relaxed);
	lane.Revision.store(gen + 1u, std::memory_order_release);
	lane.Mapping.MatchKey.store(AutomationMapping::kInactive, std::memory_order_relaxed);
	lane.Mapping.TargetPlugin = nullptr;
	lane.Mapping.TargetParameterIndex = 0u;
	lane.PointCount = 0u;
	lane.Revision.store(gen + 2u, std::memory_order_release);
}

void MidiLoop::ClearAutomationLanePoints(std::size_t laneIdx) noexcept
{
	if (laneIdx >= MaxAutomationLanes)
		return;

	auto& lane = _lanes[laneIdx];
	const auto gen = lane.Revision.load(std::memory_order_relaxed);
	lane.Revision.store(gen + 1u, std::memory_order_release);
	lane.PointCount = 0u;
	lane.Revision.store(gen + 2u, std::memory_order_release);
}

std::optional<std::size_t> MidiLoop::ResolveAutomationLaneFor(const vst::IVstPlugin* plugin,
	unsigned int paramIdx) const noexcept
{
	// 1) Reuse an active lane already mapped to this exact (plugin, parameter).
	for (std::size_t i = 0u; i < MaxAutomationLanes; ++i)
	{
		const auto& mapping = _lanes[i].Mapping;
		if (mapping.IsActive()
			&& mapping.TargetPlugin == plugin
			&& mapping.TargetParameterIndex == paramIdx)
			return i;
	}

	// 2) Otherwise claim the first inactive lane.
	for (std::size_t i = 0u; i < MaxAutomationLanes; ++i)
	{
		if (!_lanes[i].Mapping.IsActive())
			return i;
	}

	// 3) All lanes occupied by other mappings.
	return std::nullopt;
}

bool MidiLoop::WireEditorAutomationLane(std::size_t laneIdx,
	vst::IVstPlugin* plugin,
	unsigned int paramIdx) noexcept
{
	if (laneIdx >= MaxAutomationLanes)
		return false;

	auto& mapping = _lanes[laneIdx].Mapping;
	const bool alreadyMapped = mapping.IsActive()
		&& mapping.TargetPlugin == plugin
		&& mapping.TargetParameterIndex == paramIdx;
	if (alreadyMapped)
		return false;

	// Publish the target before activating the match key so a reader that observes
	// the active key also observes the resolved plugin/parameter.
	mapping.TargetPlugin = plugin;
	mapping.TargetParameterIndex = paramIdx;
	mapping.MatchKey.store(AutomationMapping::MakeEditorMatchKey(), std::memory_order_release);
	return true;
}

std::uint16_t MidiLoop::SnapshotAutomationLanePoints(std::size_t laneIdx,
	std::pair<float, float>* out, std::size_t maxPoints) const noexcept
{
	if (laneIdx >= MaxAutomationLanes || !out || 0u == maxPoints)
		return 0u;

	const auto& lane = _lanes[laneIdx];

	// Seqlock read: retry while the writer holds the lock (odd generation) or the
	// generation changed mid-copy. Bounded retries — this is a display path, so a
	// rare give-up returning the latest partial copy is acceptable.
	for (int attempt = 0; attempt < 8; ++attempt)
	{
		const auto gen0 = lane.Revision.load(std::memory_order_acquire);
		if (gen0 & 1u)
			continue; // Writer in progress.

		auto count = lane.PointCount;
		if (count > maxPoints)
			count = maxPoints;
		for (std::size_t i = 0u; i < count; ++i)
			out[i] = lane.Points[i];

		const auto gen1 = lane.Revision.load(std::memory_order_acquire);
		if (gen0 == gen1)
			return static_cast<std::uint16_t>(count);
	}

	return 0u;
}

bool MidiLoop::IsAutomationLaneActive(std::size_t laneIdx) const noexcept
{
	if (laneIdx >= MaxAutomationLanes)
		return false;

	return _lanes[laneIdx].Mapping.IsActive();
}
