#include "MidiLoop.h"

#include <algorithm>
#include <cmath>
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
	  _automationGlobalSampleOrigin(0u),
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
	_completedLengthForEditor.store(0u, std::memory_order_release);
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	_state = MidiLoopState::Recording;
	_playbackSnapshot.store(nullptr, std::memory_order_seq_cst);
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
	_completedLengthForEditor.store(0u, std::memory_order_release);
	_eventCount = 0u;
	_dropped = 0u;

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

	PublishCompletedEvents(_events.data(), _eventCount, _loopLengthSamps,
		_revision, _quantisation, _quantisationTransportStartSamps);
	_completedLengthForEditor.store(loopLengthSamps, std::memory_order_release);
}

void MidiLoop::FinalizeOverdubBase(std::uint32_t loopLengthSamps)
{
	_completedLengthForEditor.store(0u, std::memory_order_release);
	MidiNote::SortMidiEvents(_events.data(), _eventCount);
	_loopLengthSamps = loopLengthSamps;
	_state = MidiLoopState::Playing;
	++_revision;

	PublishCompletedEvents(_events.data(), _eventCount, _loopLengthSamps,
		_revision, _quantisation, _quantisationTransportStartSamps);
	_completedLengthForEditor.store(loopLengthSamps, std::memory_order_release);
}

void MidiLoop::EndRecord(std::uint32_t loopLengthSamps, std::uint32_t startGlobalSample)
{
	_completedLengthForEditor.store(0u, std::memory_order_release);
	MidiNote::SortMidiEvents(_events.data(), _eventCount);

	_loopLengthSamps = loopLengthSamps;
	_automationGlobalSampleOrigin = startGlobalSample;
	_state = MidiLoopState::Playing;
	++_revision;

	// Recording just finalised the loop window. If quantisation was already armed
	// for this take, rebuild the parallel buffer against the new length now so the
	// first playback block can read it without further work.
	PublishCompletedEvents(_events.data(), _eventCount, _loopLengthSamps,
		_revision, _quantisation, _quantisationTransportStartSamps);
	_completedLengthForEditor.store(loopLengthSamps, std::memory_order_release);
}

void MidiLoop::Reset() noexcept
{
	_completedLengthForEditor.store(0u, std::memory_order_release);
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	_state = MidiLoopState::Empty;
	_playbackSnapshot.store(nullptr, std::memory_order_seq_cst);
	++_revision;
}

bool MidiLoop::SnapshotForExport(ExportState& state,
	std::int32_t anchorCorrection) const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	state.LoopLengthSamps = snapshot ? snapshot->LoopLengthSamps : _loopLengthSamps;
	state.AutomationGlobalSampleOrigin = _automationGlobalSampleOrigin
		+ static_cast<std::uint32_t>(anchorCorrection);
	const auto count = snapshot ? snapshot->EventCount : _eventCount;
	if (count > state.Events.size())
	{
		if (snapshot) ReleasePlaybackSnapshot();
		return false;
	}

	// EndRecord deliberately retains events beyond a quantised loop boundary so
	// capture can finish without modifying its source storage. Playback ignores
	// those events, and export must do the same: sidecars only represent the
	// playable loop window.
	state.EventCount = 0u;
	for (std::size_t i = 0u; i < count; ++i)
	{
		const auto& event = snapshot ? snapshot->Raw[i] : _events[i];
		if (event.sampleOffset >= state.LoopLengthSamps)
			continue;
		state.Events[state.EventCount++] = event;
	}
	if (snapshot) ReleasePlaybackSnapshot();

	for (std::size_t laneIdx = 0u; laneIdx < MaxAutomationLanes; ++laneIdx)
	{
		const auto& lane = _lanes[laneIdx];
		auto& exported = state.AutomationLanes[laneIdx];
		bool copied = false;
		for (unsigned int attempt = 0u; attempt < 8u; ++attempt)
		{
			const auto before = lane.Revision.load(std::memory_order_acquire);
			if ((before & 1u) != 0u)
				continue;
			exported.MatchKey = lane.Mapping.MatchKey.load(std::memory_order_acquire);
			exported.TargetParameterIndex = lane.Mapping.TargetParameterIndex;
			exported.TargetPlugin = lane.Mapping.TargetPlugin;
			exported.PointCount = lane.PointCount;
			if (exported.PointCount > exported.Points.size())
				return false;
			for (std::size_t point = 0u; point < exported.PointCount; ++point)
				exported.Points[point] = lane.Points[point];
			if (before == lane.Revision.load(std::memory_order_acquire))
			{
				copied = true;
				break;
			}
		}
		if (!copied)
			return false;
	}

	return true;
}

bool MidiLoop::RestoreFromExport(const ExportState& state) noexcept
{
	if (state.LoopLengthSamps == 0u || state.EventCount > _events.size())
		return false;
	for (std::size_t i = 0u; i < state.EventCount; ++i)
	{
		if (state.Events[i].sampleOffset >= state.LoopLengthSamps)
			return false;
	}

	for (const auto& laneState : state.AutomationLanes)
	{
		if (laneState.PointCount > AutomationLane::MaxPoints)
			return false;
		const auto matchKey = laneState.MatchKey;
		const auto isEditor = matchKey == AutomationMapping::MakeEditorMatchKey();
		const auto isCc = (matchKey & (1u << 16)) != 0u
			&& (matchKey & ~0x1ffffu) == 0u
			&& ((matchKey >> 8) & 0xffu) < 16u
			&& (matchKey & 0xffu) < 128u;
		if (matchKey != AutomationMapping::kInactive && !isEditor && !isCc)
			return false;
		for (std::size_t point = 0u; point < laneState.PointCount; ++point)
		{
			const auto [frac, value] = laneState.Points[point];
			if (!std::isfinite(frac) || !std::isfinite(value) || frac < 0.0f || frac > 1.0f)
				return false;
			if (point > 0u && laneState.Points[point - 1u].first > frac)
				return false;
		}
	}
	if (!PublishCompletedEvents(state.Events.data(), state.EventCount,
		state.LoopLengthSamps, _revision + 1u, _quantisation,
		_quantisationTransportStartSamps))
		return false;

	_eventCount = state.EventCount;
	for (std::size_t i = 0u; i < _eventCount; ++i)
		_events[i] = state.Events[i];
	_loopLengthSamps = state.LoopLengthSamps;
	_automationGlobalSampleOrigin = state.AutomationGlobalSampleOrigin;
	_dropped = 0u;
	_state = MidiLoopState::Playing;

	for (std::size_t laneIdx = 0u; laneIdx < MaxAutomationLanes; ++laneIdx)
	{
		auto& lane = _lanes[laneIdx];
		const auto generation = lane.Revision.load(std::memory_order_relaxed);
		lane.Revision.store(generation + 1u, std::memory_order_release);
		const auto& imported = state.AutomationLanes[laneIdx];
		lane.Mapping.TargetPlugin = nullptr;
		lane.Mapping.TargetParameterIndex = imported.TargetParameterIndex;
		lane.Mapping.MatchKey.store(imported.MatchKey, std::memory_order_relaxed);
		lane.PointCount = imported.PointCount;
		for (std::size_t point = 0u; point < imported.PointCount; ++point)
			lane.Points[point] = imported.Points[point];
		lane.Revision.store(generation + 2u, std::memory_order_release);
	}

	++_revision;
	_completedLengthForEditor.store(state.LoopLengthSamps, std::memory_order_release);
	return true;
}

bool MidiLoop::BindAutomationLaneTarget(std::size_t laneIdx,
	vst::IVstPlugin* plugin) noexcept
{
	if (laneIdx >= MaxAutomationLanes || !plugin || !_lanes[laneIdx].Mapping.IsActive())
		return false;

	_lanes[laneIdx].Mapping.TargetPlugin = plugin;
	return true;
}

bool MidiLoop::TryGetEvent(std::size_t index, MidiEvent& ev) const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (snapshot)
	{
		const bool found = index < snapshot->EventCount;
		if (found) ev = snapshot->Raw[index];
		ReleasePlaybackSnapshot();
		return found;
	}
	if (index >= _eventCount) return false;
	ev = _events[index];
	return true;
}

bool MidiLoop::TryGetPlaybackEvent(std::size_t index, MidiEvent& ev) const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (snapshot)
	{
		const bool found = index < snapshot->EventCount;
		if (found) ev = snapshot->QuantisationActive ? snapshot->Quantised[index] : snapshot->Raw[index];
		ReleasePlaybackSnapshot();
		return found;
	}
	return TryGetEvent(index, ev);
}

bool MidiLoop::SnapshotForEdit(EditState& state) const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (!snapshot) return false;
	state.EventCount = snapshot->EventCount;
	state.LoopLengthSamps = snapshot->LoopLengthSamps;
	state.Revision = snapshot->Revision;
	state.Quantisation = snapshot->Quantisation;
	state.QuantisationTransportStartSamps = snapshot->QuantisationTransportStartSamps;
	std::copy_n(snapshot->Raw.begin(), snapshot->EventCount, state.Events.begin());
	ReleasePlaybackSnapshot();
	return true;
}

bool MidiLoop::PublishEdit(const EditState& state) noexcept
{
	if (_state != MidiLoopState::Playing || state.Revision != _revision
		|| state.LoopLengthSamps != _loopLengthSamps || state.LoopLengthSamps == 0u
		|| state.Quantisation != _quantisation
		|| state.QuantisationTransportStartSamps != _quantisationTransportStartSamps
		|| state.EventCount > DefaultCapacity) return false;
	const auto* previous = AcquirePlaybackSnapshot();
	if (!previous || previous->Revision != state.Revision)
	{
		if (previous) ReleasePlaybackSnapshot();
		return false;
	}
	std::array<MidiEvent, DefaultCapacity> priorEvents{};
	const auto priorCount = previous->EventCount;
	std::copy_n(previous->Raw.begin(), priorCount, priorEvents.begin());
	// Non-note events belong to capture/automation and are never changed by an
	// editor gesture. Compare their complete ordered subsequence before publish.
	std::size_t oldNonNote = 0u;
	std::size_t newNonNote = 0u;
	for (;;)
	{
		while (oldNonNote < previous->EventCount &&
			(previous->Raw[oldNonNote].IsNoteOn() || previous->Raw[oldNonNote].IsNoteOff())) ++oldNonNote;
		while (newNonNote < state.EventCount &&
			(state.Events[newNonNote].IsNoteOn() || state.Events[newNonNote].IsNoteOff())) ++newNonNote;
		if (oldNonNote == previous->EventCount || newNonNote == state.EventCount) break;
		const auto& oldEvent = previous->Raw[oldNonNote++];
		const auto& newEvent = state.Events[newNonNote++];
		if (oldEvent.sampleOffset != newEvent.sampleOffset || oldEvent.status != newEvent.status
			|| oldEvent.data1 != newEvent.data1 || oldEvent.data2 != newEvent.data2)
		{
			ReleasePlaybackSnapshot();
			return false;
		}
	}
	const bool nonNoteCountMatches = oldNonNote == previous->EventCount && newNonNote == state.EventCount;
	ReleasePlaybackSnapshot();
	if (!nonNoteCountMatches) return false;
	// Finalised capture may retain a suffix beyond the snapped playable length.
	// It is invisible to playback/export but must survive an unrelated edit.
	std::size_t priorTail = 0u;
	while (priorTail < priorCount && priorEvents[priorTail].sampleOffset < state.LoopLengthSamps)
		++priorTail;
	std::size_t newTail = 0u;
	while (newTail < state.EventCount && state.Events[newTail].sampleOffset < state.LoopLengthSamps)
		++newTail;
	if (priorCount - priorTail != state.EventCount - newTail) return false;
	for (std::size_t i = 0u; i < priorCount - priorTail; ++i)
	{
		const auto& a = priorEvents[priorTail + i];
		const auto& b = state.Events[newTail + i];
		if (a.sampleOffset != b.sampleOffset || a.status != b.status
			|| a.data1 != b.data1 || a.data2 != b.data2) return false;
	}
	std::array<std::uint32_t, TotalNoteSlots> activeStart{};
	std::bitset<TotalNoteSlots> active;
	std::bitset<TotalNoteSlots> invalidSlots;
	std::uint32_t previousOffset = 0u;
	int previousPriority = -1;
	for (std::size_t i = 0u; i < state.EventCount; ++i)
	{
		const auto& event = state.Events[i];
		if (event.sampleOffset >= state.LoopLengthSamps) break;
		const int priority = event.IsNoteOff() ? 0 : (event.IsNoteOn() ? 2 : 1);
		if (i > 0u && (event.sampleOffset < previousOffset
			|| (event.sampleOffset == previousOffset && priority < previousPriority))) return false;
		previousOffset = event.sampleOffset;
		previousPriority = priority;
		if (!event.IsNoteOn() && !event.IsNoteOff()) continue;
		if (event.data1 >= 128u) return false;
		const auto slot = NoteSlot(event.Channel(), event.data1);
		if (event.IsNoteOn())
		{
			if (active.test(slot)) invalidSlots.set(slot);
			active.set(slot);
			activeStart[slot] = event.sampleOffset;
		}
		else
		{
			if (!active.test(slot) || event.sampleOffset <= activeStart[slot]) invalidSlots.set(slot);
			active.reset(slot);
		}
	}
	// An unmatched NoteOn legitimately lasts to the loop seam, where playback
	// emits a synthetic off. Existing malformed overlap on an untouched slot
	// must not prevent a separate note from being edited.
	for (std::size_t slot = 0u; slot < TotalNoteSlots; ++slot)
	{
		if (!invalidSlots.test(slot)) continue;
		std::size_t oldIndex = 0u;
		std::size_t newIndex = 0u;
		for (;;)
		{
			while (oldIndex < priorCount &&
				(!priorEvents[oldIndex].IsNoteOn() && !priorEvents[oldIndex].IsNoteOff()
					|| NoteSlot(priorEvents[oldIndex].Channel(), priorEvents[oldIndex].data1) != slot)) ++oldIndex;
			while (newIndex < state.EventCount &&
				(!state.Events[newIndex].IsNoteOn() && !state.Events[newIndex].IsNoteOff()
					|| NoteSlot(state.Events[newIndex].Channel(), state.Events[newIndex].data1) != slot)) ++newIndex;
			if (oldIndex == priorCount || newIndex == state.EventCount) break;
			const auto& oldEvent = priorEvents[oldIndex++];
			const auto& newEvent = state.Events[newIndex++];
			if (oldEvent.sampleOffset != newEvent.sampleOffset || oldEvent.status != newEvent.status
				|| oldEvent.data1 != newEvent.data1 || oldEvent.data2 != newEvent.data2) return false;
		}
		if (oldIndex != priorCount || newIndex != state.EventCount) return false;
	}
	if (!PublishCompletedEvents(state.Events.data(), state.EventCount,
		state.LoopLengthSamps, _revision + 1u, _quantisation,
		_quantisationTransportStartSamps)) return false;
	std::copy_n(state.Events.begin(), state.EventCount, _events.begin());
	_eventCount = state.EventCount;
	++_revision;
	return true;
}

const MidiLoop::PlaybackSnapshot* MidiLoop::AcquirePlaybackSnapshot() const noexcept
{
	_playbackReaders.fetch_add(1u, std::memory_order_seq_cst);
	const auto* snapshot = _playbackSnapshot.load(std::memory_order_seq_cst);
	if (!snapshot) ReleasePlaybackSnapshot();
	return snapshot;
}

void MidiLoop::ReleasePlaybackSnapshot() const noexcept
{
	_playbackReaders.fetch_sub(1u, std::memory_order_seq_cst);
}

bool MidiLoop::IsQuantisationActive() const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (!snapshot) return false;
	const bool active = snapshot->QuantisationActive;
	ReleasePlaybackSnapshot();
	return active;
}

std::bitset<MidiLoop::TotalNoteSlots> MidiLoop::HeldNotes() const noexcept
{
	std::bitset<TotalNoteSlots> result;
	for (std::size_t slot = 0u; slot < TotalNoteSlots; ++slot)
		if (_heldPublished[slot].load(std::memory_order_seq_cst) != 0u)
			result.set(slot);
	return result;
}

std::size_t MidiLoop::EventCount() const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (!snapshot) return _eventCount;
	const auto count = snapshot->EventCount;
	ReleasePlaybackSnapshot();
	return count;
}

std::uint32_t MidiLoop::LoopLengthSamps() const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (!snapshot) return _loopLengthSamps;
	const auto length = snapshot->LoopLengthSamps;
	ReleasePlaybackSnapshot();
	return length;
}

std::uint64_t MidiLoop::Revision() const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (!snapshot) return _revision;
	const auto revision = snapshot->Revision;
	ReleasePlaybackSnapshot();
	return revision;
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
		const auto* snapshot = AcquirePlaybackSnapshot();
		const MidiEvent* eventSource = snapshot ?
			(snapshot->QuantisationActive ? snapshot->Quantised.data() : snapshot->Raw.data()) : _events.data();
		std::uint32_t maxOffset = 0u;
		for (std::size_t i = 0; i < (snapshot ? snapshot->EventCount : _eventCount); ++i)
			if (eventSource[i].sampleOffset > maxOffset)
				maxOffset = eventSource[i].sampleOffset;
		if (snapshot) ReleasePlaybackSnapshot();

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
	const auto* snapshot = AcquirePlaybackSnapshot();
	const MidiEvent* eventSource = snapshot ?
		(snapshot->QuantisationActive ? snapshot->Quantised.data() : snapshot->Raw.data()) : _events.data();
	auto spans = MidiNote::ExtractSpans(eventSource, snapshot ? snapshot->EventCount : _eventCount, effectiveLength);
	if (snapshot) ReleasePlaybackSnapshot();
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
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (!snapshot || 0u == snapshot->LoopLengthSamps || 0u == numSamples)
	{
		if (numSamples > 0u)
			FlushHeldNotes(globalSample, sink);
		if (snapshot) ReleasePlaybackSnapshot();
		return;
	}
	const auto loopLength = snapshot->LoopLengthSamps;
	if (_callbackRevision != snapshot->Revision)
	{
		FlushHeldNotes(globalSample, sink);
		_callbackRevision = snapshot->Revision;
	}

	std::uint32_t remaining = numSamples;
	std::uint32_t cursor = globalSample;

	while (remaining > 0u)
	{
		const std::uint32_t loopOffset =
			static_cast<std::uint32_t>(cursor % loopLength);
		const std::uint32_t roomInLoop = loopLength - loopOffset;
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
		EmitEventsInRange(loopOffset, loopOffset + segment, globalBase, *snapshot, sink);

		const bool wraps = (segment == roomInLoop) && (remaining > roomInLoop);
		if (wraps)
		{
			// Wrap boundary: flush held notes at the exact wrap sample.
			const std::uint32_t wrapSample = globalBase + loopLength;
			FlushHeldNotes(wrapSample, sink);
		}

		cursor += segment;
		remaining -= segment;
	}
	if (_heldFlushRequested.load(std::memory_order_seq_cst))
		FlushHeldNotes(cursor - 1u, sink);
	ReleasePlaybackSnapshot();
}

void MidiLoop::EmitEventsInRange(std::uint32_t lo,
                                 std::uint32_t hi,
                                 std::uint32_t globalBase,
								 const PlaybackSnapshot& snapshot,
                                 IMidiSink& sink) noexcept
{
	const MidiEvent* events = snapshot.QuantisationActive ? snapshot.Quantised.data() : snapshot.Raw.data();

	// Linear scan: small N expected, and storage is contiguous.
	for (std::size_t i = 0; i < snapshot.EventCount; ++i)
	{
		const MidiEvent& src = events[i];
		const std::uint32_t off = src.sampleOffset;
		if (off >= snapshot.LoopLengthSamps)
			continue;
		if (off < lo || off >= hi)
			continue;

		MidiEvent out = src;
		out.sampleOffset = globalBase + off;
		sink.OnEvent(out);

		// Track held notes for wrap flushing.
		if (src.IsNoteOn())
		{
			_held.set(NoteSlot(src.Channel(), src.data1));
			_heldPublished[NoteSlot(src.Channel(), src.data1)].store(1u, std::memory_order_seq_cst);
		}
		else if (src.IsNoteOff())
		{
			_held.reset(NoteSlot(src.Channel(), src.data1));
			_heldPublished[NoteSlot(src.Channel(), src.data1)].store(0u, std::memory_order_seq_cst);
		}
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
			_heldPublished[slot].store(0u, std::memory_order_seq_cst);
		}
	}
}

bool MidiLoop::SetQuantisation(const MidiQuantisationSettings& settings,
	std::uint64_t transportStartSamps)
{
	if (settings == _quantisation && transportStartSamps == _quantisationTransportStartSamps)
		return true;
	if (_state == MidiLoopState::Playing &&
		!PublishCompletedEvents(_events.data(), _eventCount, _loopLengthSamps,
			_revision + 1u, settings, transportStartSamps))
		return false;
	_quantisation = settings;
	_quantisationTransportStartSamps = transportStartSamps;
	++_revision;
	return true;
}

bool MidiLoop::PublishCompletedEvents(const MidiEvent* raw, std::size_t count,
	std::uint32_t length, std::uint64_t revision,
	const MidiQuantisationSettings& quantisation,
	std::uint64_t transportStart) noexcept
{
	if (count > DefaultCapacity || length == 0u)
		return false;
	const auto* current = _playbackSnapshot.load(std::memory_order_seq_cst);
	if (_playbackReaders.load(std::memory_order_seq_cst) == 0u)
	{
		for (std::size_t i = 0u; i < _playbackBuffers.size(); ++i)
			if (&_playbackBuffers[i] != current) _playbackBufferUsed[i] = false;
	}
	std::size_t slot = _playbackBuffers.size();
	for (std::size_t i = 0u; i < _playbackBuffers.size(); ++i)
		if (!_playbackBufferUsed[i]) { slot = i; break; }
	if (slot == _playbackBuffers.size()) return false;
	auto& snapshot = _playbackBuffers[slot];
	std::copy_n(raw, count, snapshot.Raw.begin());
	snapshot.EventCount = count;
	snapshot.LoopLengthSamps = length;
	snapshot.Revision = revision;
	snapshot.Quantisation = quantisation;
	snapshot.QuantisationTransportStartSamps = transportStart;
	snapshot.QuantisationActive = quantisation.Enabled
		&& (quantisation.GrainSamps > 0u || quantisation.HasRemoteGrid()) && count > 0u;
	if (snapshot.QuantisationActive)
		MidiQuantisation::BuildQuantisedPlaybackEvents(snapshot.Raw.data(), count,
			length, quantisation, transportStart, snapshot.Quantised.data());
	_playbackBufferUsed[slot] = true;
	_playbackSnapshot.store(&snapshot, std::memory_order_seq_cst);
	return true;
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
