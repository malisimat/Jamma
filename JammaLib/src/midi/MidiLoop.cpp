#include "MidiLoop.h"
#include <limits>

#include <algorithm>
#include <cmath>
#include <thread>

#include "../graphics/MidiModel.h"
#include "MidiNote.h"
#include "MidiQuantisation.h"
#include "../../include/Constants.h"

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
	AcquireOwnerPublicationGate();
	_state = MidiLoopState::Recording;
	_playbackSnapshot.store(nullptr, std::memory_order_seq_cst);
	_preparedPlaybackSnapshot.store(nullptr, std::memory_order_seq_cst);
	ReleaseOwnerPublicationGate();
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
	const auto keepCount = (events && count > 0u)
		? (std::min)(count, _events.size()) : 0u;
	if (keepCount > 0u)
		std::copy_n(events, keepCount, _completionScratch.begin());
	MidiNote::SortMidiEvents(_completionScratch.data(), keepCount);
	PublishCompletionWithRetry(_completionScratch.data(), keepCount,
		loopLengthSamps, _revision + 1u);
	std::copy_n(_completionScratch.begin(), keepCount, _events.begin());
	_eventCount = keepCount;
	_dropped = (events && count > _events.size())
		? static_cast<std::uint64_t>(count - _events.size()) : 0u;
	_loopLengthSamps = loopLengthSamps;
	_state = MidiLoopState::Playing;
	++_revision;
	_completedLengthForEditor.store(loopLengthSamps, std::memory_order_release);
}

void MidiLoop::FinalizeOverdubBase(std::uint32_t loopLengthSamps)
{
	std::copy_n(_events.begin(), _eventCount, _completionScratch.begin());
	MidiNote::SortMidiEvents(_completionScratch.data(), _eventCount);
	PublishCompletionWithRetry(_completionScratch.data(), _eventCount,
		loopLengthSamps, _revision + 1u);
	std::copy_n(_completionScratch.begin(), _eventCount, _events.begin());
	_loopLengthSamps = loopLengthSamps;
	_state = MidiLoopState::Playing;
	++_revision;
	_completedLengthForEditor.store(loopLengthSamps, std::memory_order_release);
}

void MidiLoop::EndRecord(std::uint32_t loopLengthSamps, std::uint32_t startGlobalSample)
{
	std::copy_n(_events.begin(), _eventCount, _completionScratch.begin());
	MidiNote::SortMidiEvents(_completionScratch.data(), _eventCount);
	// Publish the final grid before making the source or editor length visible.
	PublishCompletionWithRetry(_completionScratch.data(), _eventCount,
		loopLengthSamps, _revision + 1u);
	std::copy_n(_completionScratch.begin(), _eventCount, _events.begin());
	_loopLengthSamps = loopLengthSamps;
	_automationGlobalSampleOrigin = startGlobalSample;
	_state = MidiLoopState::Playing;
	++_revision;
	_completedLengthForEditor.store(loopLengthSamps, std::memory_order_release);
}

void MidiLoop::Reset() noexcept
{
	_completedLengthForEditor.store(0u, std::memory_order_release);
	_eventCount = 0;
	_loopLengthSamps = 0;
	_dropped = 0;
	AcquireOwnerPublicationGate();
	_state = MidiLoopState::Empty;
	_playbackSnapshot.store(nullptr, std::memory_order_seq_cst);
	_preparedPlaybackSnapshot.store(nullptr, std::memory_order_seq_cst);
	ReleaseOwnerPublicationGate();
	++_revision;
}

bool MidiLoop::SnapshotForExport(ExportState& state,
	std::int32_t anchorCorrection) const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	state.LoopQuantisation = _loopQuantisationOverride;
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
	OwnerPublicationGuard publication(*this);
	if (_preparedGridActivated.load(std::memory_order_seq_cst))
		CompletePreparedQuantisationOnOwner();
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
	const auto activated = _preparedGridActivated.load(std::memory_order_seq_cst);
	const auto& inherited = activated ? _preparedOwnerInheritedQuantisation : _inheritedQuantisation;
	const auto& forced = activated ? _preparedOwnerForcedQuantisationEnabled : _forcedQuantisationEnabled;
	auto restoredQuantisation = ResolveQuantisation(inherited, state.LoopQuantisation);
	if (forced) restoredQuantisation.Enabled = *forced;
	if (!PublishCompletedEvents(state.Events.data(), state.EventCount,
		state.LoopLengthSamps, _revision + 1u, restoredQuantisation,
		activated ? _preparedOwnerTransportStartSamps : _quantisationTransportStartSamps,
		true, false, true))
		return false;

	_loopQuantisationOverride = state.LoopQuantisation;
	_quantisation = restoredQuantisation;
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
		if (found) ev = snapshot->PlaybackViewActive ? snapshot->Quantised[index] : snapshot->Raw[index];
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
	state.SourceRevision = snapshot->SourceRevision;
	state.Quantisation = snapshot->Quantisation;
	state.QuantisationTransportStartSamps = snapshot->QuantisationTransportStartSamps;
	std::copy_n(snapshot->Raw.begin(), snapshot->EventCount, state.Events.begin());
	ReleasePlaybackSnapshot();
	return true;
}

bool MidiLoop::PublishEdit(const EditState& state) noexcept
{
	OwnerPublicationGuard publication(*this);
	if (_preparedGridActivated.load(std::memory_order_seq_cst))
		CompletePreparedQuantisationOnOwner();
	if (_state != MidiLoopState::Playing || state.Revision != _revision
		|| state.SourceRevision != _sourceRevision
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
			|| oldEvent.data1 != newEvent.data1 || oldEvent.data2 != newEvent.data2 || oldEvent.flags != newEvent.flags)
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
			|| a.data1 != b.data1 || a.data2 != b.data2 || a.flags != b.flags) return false;
	}
	std::array<std::uint32_t, MidiEvent::PairingSlotCount> activeStart{};
	std::bitset<MidiEvent::PairingSlotCount> active;
	std::bitset<MidiEvent::PairingSlotCount> invalidSlots;
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
		const auto slot = event.PairingSlot();
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
	for (std::size_t slot = 0u; slot < MidiEvent::PairingSlotCount; ++slot)
	{
		if (!invalidSlots.test(slot)) continue;
		std::size_t oldIndex = 0u;
		std::size_t newIndex = 0u;
		for (;;)
		{
			while (oldIndex < priorCount &&
				(!priorEvents[oldIndex].IsNoteOn() && !priorEvents[oldIndex].IsNoteOff()
					|| priorEvents[oldIndex].PairingSlot() != slot)) ++oldIndex;
			while (newIndex < state.EventCount &&
				(!state.Events[newIndex].IsNoteOn() && !state.Events[newIndex].IsNoteOff()
					|| state.Events[newIndex].PairingSlot() != slot)) ++newIndex;
			if (oldIndex == priorCount || newIndex == state.EventCount) break;
			const auto& oldEvent = priorEvents[oldIndex++];
			const auto& newEvent = state.Events[newIndex++];
			if (oldEvent.sampleOffset != newEvent.sampleOffset || oldEvent.status != newEvent.status
				|| oldEvent.data1 != newEvent.data1 || oldEvent.data2 != newEvent.data2 || oldEvent.flags != newEvent.flags) return false;
		}
		if (oldIndex != priorCount || newIndex != state.EventCount) return false;
	}
	if (!PublishCompletedEvents(state.Events.data(), state.EventCount,
		state.LoopLengthSamps, _revision + 1u,
		_preparedGridActivated.load(std::memory_order_seq_cst)
			? _preparedOwnerQuantisation : _quantisation,
		_preparedGridActivated.load(std::memory_order_seq_cst)
			? _preparedOwnerTransportStartSamps : _quantisationTransportStartSamps,
		true, false, true)) return false;
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

std::optional<std::uint64_t> MidiLoop::PublishedRevisionAtAudioBoundary() const noexcept
{
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (!snapshot) return std::nullopt;
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
			(snapshot->PlaybackViewActive ? snapshot->Quantised.data() : snapshot->Raw.data()) : _events.data();
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
		// The shared picker ring belongs to the first stream, which may stay
		// silent while another channel records. Its geometry still follows time.
		if (!revisionChanged && 0u == _eventCount && MidiLoopState::Recording != _state)
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
		(snapshot->PlaybackViewActive ? snapshot->Quantised.data() : snapshot->Raw.data()) : _events.data();
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

void MidiLoop::FlushPlaybackHeldNotes(std::uint32_t atGlobalSample, IMidiSink& sink) noexcept
{
	RetryPendingReleases(atGlobalSample, sink);
	FlushHeldNotes(atGlobalSample, sink);
	_heldFlushRequested.exchange(false, std::memory_order_seq_cst);
}

void MidiLoop::ReadBlock(std::uint32_t globalSample,
                         std::uint32_t numSamples,
                         IMidiSink& sink) noexcept
{
	if (numSamples > 0u) RetryPendingReleases(globalSample, sink);
	const auto* snapshot = AcquirePlaybackSnapshot();
	if (!snapshot || 0u == snapshot->LoopLengthSamps || 0u == numSamples)
	{
		if (numSamples > 0u)
		{
			FlushHeldNotes(globalSample, sink);
			_heldFlushRequested.exchange(false, std::memory_order_seq_cst);
		}
		if (snapshot) ReleasePlaybackSnapshot();
		return;
	}
	const auto loopLength = snapshot->LoopLengthSamps;
	if (_callbackRevision != snapshot->Revision)
	{
		FlushHeldNotes(globalSample, sink);
		_callbackRevision = snapshot->Revision;
	}
	if (_relocationChaseRequested)
	{
		FlushHeldNotes(globalSample, sink);
		if (_held.any() || _pendingReleases.any())
		{
			ReleasePlaybackSnapshot();
			return; // Retry before reconstructing a new voice.
		}
		_heldFlushRequested.exchange(false, std::memory_order_seq_cst);
		_relocationChaseRetry = !ChaseAtCursor(globalSample, *snapshot, sink);
		_relocationChaseRequested = false;
	}
	else if (_relocationChaseRetry)
	{
		_relocationChaseRetry = !ChaseAtCursor(globalSample, *snapshot, sink);
	}

	std::uint32_t remaining = numSamples;
	std::uint32_t cursor = globalSample;

	while (remaining > 0u)
	{
		const std::uint32_t loopOffset =
			static_cast<std::uint32_t>(cursor % loopLength);
		const std::uint32_t roomInLoop = loopLength - loopOffset;
		const std::uint32_t segment = (remaining < roomInLoop) ? remaining : roomInLoop;

		// Also handle a previous block ending exactly at the seam.
		// Only paired held notes are released; continuations survive both paths.
		if (loopOffset == 0u)
			FlushHeldNotes(cursor, sink, HeldFlushPolicy::Seam);

		// globalBase is the absolute sample corresponding to loopOffset.
		const std::uint32_t globalBase = cursor - loopOffset;
		EmitEventsInRange(loopOffset, loopOffset + segment, globalBase, *snapshot, sink);

		const bool wraps = (segment == roomInLoop) && (remaining > roomInLoop);
		if (wraps)
		{
			// Wrap boundary: flush held notes at the exact wrap sample.
			const std::uint32_t wrapSample = globalBase + loopLength;
			FlushHeldNotes(wrapSample, sink, HeldFlushPolicy::Seam);
		}

		cursor += segment;
		remaining -= segment;
	}
	if (_heldFlushRequested.exchange(false, std::memory_order_seq_cst))
		FlushHeldNotes(cursor - 1u, sink);
	ReleasePlaybackSnapshot();
}

void MidiLoop::EmitEventsInRange(std::uint32_t lo,
                                 std::uint32_t hi,
                                 std::uint32_t globalBase,
								 const PlaybackSnapshot& snapshot,
                                 IMidiSink& sink) noexcept
{
	const MidiEvent* events = snapshot.PlaybackViewActive ? snapshot.Quantised.data() : snapshot.Raw.data();

	const auto& unmatchedStarts = snapshot.PlaybackViewActive ?
		snapshot.QuantisedUnmatchedStarts : snapshot.RawUnmatchedStarts;

	// Linear scan: small N expected, and storage is contiguous.
	for (std::size_t i = 0; i < snapshot.EventCount; ++i)
	{
		const MidiEvent& src = events[i];
		const std::uint32_t off = src.sampleOffset;
		if (off >= snapshot.LoopLengthSamps)
			continue;
		if (off < lo || off >= hi)
			continue;

		const auto slot = NoteSlot(src.Channel(), src.data1);
		if (src.IsNoteOn() && (ShouldSuppressContinuedAttack(_continuing.test(slot),
			_mergedContinuing.test(slot), unmatchedStarts[i], _continuationRoot[slot],
			snapshot.PlaybackViewActive ? snapshot.QuantisedContinuationRoots[i] : 0u) || _pendingReleases.test(slot)))
			continue;

		MidiEvent out = src;
		out.sampleOffset = globalBase + off;
		if (!sink.TryOnEvent(out))
		{
			if (src.IsNoteOff() && _held.test(slot))
			{
				_pendingReleaseVelocity[slot] = src.data2;
				_pendingReleases.set(slot);
			}
			continue;
		}

		// Track only notes delivered to an output for wrap flushing.
		if (src.IsNoteOn())
		{
			_held.set(slot);
			_continuing.set(slot, unmatchedStarts[i] == NaturalContinuation || unmatchedStarts[i] == MergedContinuation);
			_mergedContinuing.set(slot, unmatchedStarts[i] == MergedContinuation);
			_continuationRoot[slot] = snapshot.PlaybackViewActive ? snapshot.QuantisedContinuationRoots[i] : 0u;
			_heldPublished[slot].store(1u, std::memory_order_seq_cst);
		}
		else if (src.IsNoteOff())
		{
			_held.reset(slot);
			_continuing.reset(slot);
			_mergedContinuing.reset(slot);
			_pendingReleases.reset(slot);
			_heldPublished[NoteSlot(src.Channel(), src.data1)].store(0u, std::memory_order_seq_cst);
		}
	}
}

void MidiLoop::RetryPendingReleases(std::uint32_t atGlobalSample, IMidiSink& sink) noexcept
{
	if (_pendingReleases.none()) return;
	for (std::uint8_t channel = 0u; channel < 16u; ++channel)
		for (std::uint8_t note = 0u; note < 128u; ++note)
		{
			const auto slot = NoteSlot(channel, note);
			if (!_pendingReleases.test(slot)) continue;
			const auto off = MidiEvent::MakeNoteOff(atGlobalSample, channel, note, _pendingReleaseVelocity[slot]);
			if (!sink.TryOnEvent(off)) continue;
			_pendingReleases.reset(slot);
			_continuing.reset(slot);
			_mergedContinuing.reset(slot);
			_held.reset(slot);
			_heldPublished[slot].store(0u, std::memory_order_seq_cst);
		}
}

void MidiLoop::FlushHeldNotes(std::uint32_t atGlobalSample, IMidiSink& sink,
	HeldFlushPolicy policy) noexcept
{
	if (_held.none())
	{
		if (policy == HeldFlushPolicy::All)
		{
			_continuing.reset();
			_mergedContinuing.reset();
		}
		return;
	}

	for (std::uint8_t channel = 0; channel < 16; ++channel)
	{
		for (std::uint8_t note = 0; note < 128; ++note)
		{
			const std::size_t slot = NoteSlot(channel, note);
			if (!_held.test(slot) || (policy == HeldFlushPolicy::Seam && _continuing.test(slot)))
				continue;

			const auto velocity = _pendingReleases.test(slot) ? _pendingReleaseVelocity[slot] : 0u;
			MidiEvent off = MidiEvent::MakeNoteOff(
				static_cast<std::uint32_t>(atGlobalSample), channel, note, velocity);
			if (!sink.TryOnEvent(off))
			{
				_pendingReleaseVelocity[slot] = velocity;
				_pendingReleases.set(slot);
				continue;
			}
			_held.reset(slot);
			_continuing.reset(slot);
			_mergedContinuing.reset(slot);
			_pendingReleases.reset(slot);
			_heldPublished[slot].store(0u, std::memory_order_seq_cst);
		}
	}
}

MidiQuantisationSettings MidiLoop::ResolveQuantisation(const MidiQuantisationSettings& inherited,
	const QuantisationOverride& overrides) noexcept
{
	auto settings = inherited;
	if (overrides.Enabled) settings.Enabled = *overrides.Enabled;
	if (overrides.Fraction) settings.Fraction = *overrides.Fraction;
	// Offsets compose once at each ownership depth; avoid signed overflow.
	if (overrides.PhaseOffsetSamps)
		settings.PhaseOffsetSamps = static_cast<std::int32_t>(std::clamp<std::int64_t>(
			static_cast<std::int64_t>(inherited.PhaseOffsetSamps) + *overrides.PhaseOffsetSamps,
			std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
	return settings;
}

bool MidiLoop::SetLoopQuantisationOverride(const QuantisationOverride& value)
{
	const auto previous = _loopQuantisationOverride;
	_loopQuantisationOverride = value;
	if (SetQuantisation(_inheritedQuantisation, _quantisationTransportStartSamps, _forcedQuantisationEnabled))
		return true;
	_loopQuantisationOverride = previous;
	return false;
}

bool MidiLoop::PrepareQuantisation(const MidiQuantisationSettings& settings,
	std::uint64_t transportStartSamps, std::optional<bool> forcedEnabled)
{
	if (_preparedGridActivated.load(std::memory_order_seq_cst))
		CompletePreparedQuantisationOnOwner();
	const auto oldQuantisation = _quantisation;
	const auto oldInherited = _inheritedQuantisation;
	const auto oldForced = _forcedQuantisationEnabled;
	const auto oldStart = _quantisationTransportStartSamps;
	_prepareNextPublication = true;
	const auto beforeRevision = _revision;
	auto result = SetQuantisation(settings, transportStartSamps, forcedEnabled);
	if (result && _state == MidiLoopState::Playing && beforeRevision == _revision)
	{
		result = PublishCompletedEvents(_events.data(), _eventCount, _loopLengthSamps,
			_revision + 1u, _quantisation, _quantisationTransportStartSamps, false);
		if (result) ++_revision;
	}
	if (result)
	{
		_preparedOwnerPending = true;
		_preparedOwnerQuantisation = _quantisation;
		_preparedOwnerInheritedQuantisation = _inheritedQuantisation;
		_preparedOwnerForcedQuantisationEnabled = _forcedQuantisationEnabled;
		_preparedOwnerTransportStartSamps = _quantisationTransportStartSamps;
	}
	// Source replacements before activation continue to use the old grid.
	_quantisation = oldQuantisation;
	_inheritedQuantisation = oldInherited;
	_forcedQuantisationEnabled = oldForced;
	_quantisationTransportStartSamps = oldStart;
	_prepareNextPublication = false;
	return result;
}

void MidiLoop::CompletePreparedQuantisationOnOwner() noexcept
{
	if (!_preparedOwnerPending || !_preparedGridActivated.load(std::memory_order_seq_cst)) return;
	_preparedOwnerPending = false;
	_quantisation = _preparedOwnerQuantisation;
	_inheritedQuantisation = _preparedOwnerInheritedQuantisation;
	_forcedQuantisationEnabled = _preparedOwnerForcedQuantisationEnabled;
	_quantisationTransportStartSamps = _preparedOwnerTransportStartSamps;
	_preparedGridActivated.store(false, std::memory_order_seq_cst);
}

void MidiLoop::CancelPreparedQuantisationOnOwner() noexcept
{
	OwnerPublicationGuard publication(*this);
	CompletePreparedQuantisationOnOwner();
	_preparedPlaybackSnapshot.store(nullptr, std::memory_order_seq_cst);
	_preparedOwnerPending = false;
}

void MidiLoop::AcquireOwnerPublicationGate() noexcept
{
	std::uint8_t expected = 0u;
	while (!_promotionPublicationGate.compare_exchange_weak(expected, 2u,
		std::memory_order_seq_cst))
	{
		expected = 0u;
		std::this_thread::yield();
	}
}

void MidiLoop::ReleaseOwnerPublicationGate() noexcept
{
	_promotionPublicationGate.store(0u, std::memory_order_seq_cst);
}

bool MidiLoop::TryClaimPreparedQuantisationAtAudioBoundary() noexcept
{
	std::uint8_t expected = 0u;
	if (!_promotionPublicationGate.compare_exchange_strong(expected, 1u,
		std::memory_order_seq_cst)) return false;
	if (PreparedQuantisationReadyAtAudioBoundary()) return true;
	ReleasePreparedQuantisationClaimAtAudioBoundary();
	return false;
}

void MidiLoop::ReleasePreparedQuantisationClaimAtAudioBoundary() noexcept
{
	std::uint8_t expected = 1u;
	_promotionPublicationGate.compare_exchange_strong(expected, 0u,
		std::memory_order_seq_cst);
}

void MidiLoop::ActivatePreparedQuantisationAtAudioBoundary() noexcept
{
	// This version brackets the two-pointer handoff for owner-side pool reclamation.
	_playbackActivationVersion.fetch_add(1u, std::memory_order_seq_cst);
	const auto* prepared = _preparedPlaybackSnapshot.exchange(nullptr, std::memory_order_seq_cst);
	if (prepared)
	{
		_playbackSnapshot.store(prepared, std::memory_order_seq_cst);
		_preparedGridActivated.store(true, std::memory_order_seq_cst);
	}
	_playbackActivationVersion.fetch_add(1u, std::memory_order_seq_cst);
}

bool MidiLoop::PreparedQuantisationReadyAtAudioBoundary() const noexcept
{
	if (_state.load(std::memory_order_acquire) != MidiLoopState::Playing) return true;
	_playbackReaders.fetch_add(1u, std::memory_order_seq_cst);
	const auto* prepared = _preparedPlaybackSnapshot.load(std::memory_order_seq_cst);
	const auto* active = _playbackSnapshot.load(std::memory_order_seq_cst);
	const auto ready = prepared && active &&
		prepared->SourceRevision == active->SourceRevision;
	ReleasePlaybackSnapshot();
	return ready;
}

bool MidiLoop::SetQuantisation(const MidiQuantisationSettings& inherited,
	std::uint64_t transportStartSamps, std::optional<bool> forcedEnabled)
{
	OwnerPublicationGuard publication(*this);
	if (!_prepareNextPublication && _preparedGridActivated.load(std::memory_order_seq_cst))
		CompletePreparedQuantisationOnOwner();
	auto settings = ResolveQuantisation(inherited, _loopQuantisationOverride);
	if (forcedEnabled) settings.Enabled = *forcedEnabled;
	if (settings == _quantisation && transportStartSamps == _quantisationTransportStartSamps)
	{
		_inheritedQuantisation = inherited;
		_forcedQuantisationEnabled = forcedEnabled;
		return true;
	}
	if (_state == MidiLoopState::Playing &&
		!PublishCompletedEvents(_events.data(), _eventCount, _loopLengthSamps,
			_revision + 1u, settings, transportStartSamps, false, false, true))
		return false;
	_quantisation = settings;
	_inheritedQuantisation = inherited;
	_forcedQuantisationEnabled = forcedEnabled;
	_quantisationTransportStartSamps = transportStartSamps;
	++_revision;
	return true;
}

void MidiLoop::PublishCompletionWithRetry(const MidiEvent* raw, std::size_t count,
	std::uint32_t length, std::uint64_t revision) noexcept
{
	if (length == 0u)
	{
		AcquireOwnerPublicationGate();
		_playbackSnapshot.store(nullptr, std::memory_order_seq_cst);
		_preparedPlaybackSnapshot.store(nullptr, std::memory_order_seq_cst);
		ReleaseOwnerPublicationGate();
		return;
	}

	// Completion runs on the MIDI owner, never the audio callback. A reader of
	// one of the fixed buffers can finish independently, making a slot reusable.
	while (!PublishCompletedEvents(raw, count, length, revision,
		_quantisation, _quantisationTransportStartSamps, true, true))
		std::this_thread::yield();
}

void MidiLoop::ClassifyUnmatchedStarts(PlaybackSnapshot& snapshot) noexcept
{
	// Intrusive fixed-capacity FIFO per source pairing slot. Include capture-tail
	// releases: an endpoint outside the playable window still pairs its attack.
	constexpr std::uint16_t End = static_cast<std::uint16_t>(DefaultCapacity);
	static_assert(DefaultCapacity < std::numeric_limits<std::uint16_t>::max());
	std::array<std::uint16_t, MidiEvent::PairingSlotCount> heads;
	std::array<std::uint16_t, MidiEvent::PairingSlotCount> tails;
	std::array<std::uint16_t, DefaultCapacity> next;
	heads.fill(End);
	tails.fill(End);
	snapshot.RawUnmatchedStarts.fill(0u);
	for (std::size_t i = 0u; i < snapshot.EventCount; ++i)
	{
		const auto& event = snapshot.Raw[i];
		const auto slot = event.PairingSlot();
		if (event.IsNoteOn())
		{
			const auto index = static_cast<std::uint16_t>(i);
			next[i] = End;
			if (heads[slot] == End) heads[slot] = index;
			else next[tails[slot]] = index;
			tails[slot] = index;
			snapshot.RawUnmatchedStarts[i] = 1u;
		}
		else if (event.IsNoteOff() && heads[slot] != End)
		{
			const auto index = heads[slot];
			snapshot.RawUnmatchedStarts[index] = 0u;
			heads[slot] = next[index];
		}
	}
}

bool MidiLoop::MergePlaybackSeamNotes(PlaybackSnapshot& snapshot)
{
	struct Span
	{
		std::size_t On, Off, Parent;
		std::int64_t Start, End, Shift = 0;
		bool Continuous = false;
		std::size_t Members = 0, First = 0, Release = 0;
		std::int64_t FirstSample = (std::numeric_limits<std::int64_t>::max)();
		std::int64_t LastSample = (std::numeric_limits<std::int64_t>::lowest)();
		unsigned int Rank = 0;
	};
	const auto count = snapshot.EventCount;
	const auto length = static_cast<std::int64_t>(snapshot.LoopLengthSamps);
	std::vector<Span> spans;
	spans.reserve(count / 2u);
	std::array<std::vector<std::size_t>, MidiEvent::PairingSlotCount> pending;
	std::array<std::size_t, MidiEvent::PairingSlotCount> read{};
	const auto& grid = snapshot.Quantisation;
	for (std::size_t i = 0; i < count; ++i)
	{
		const auto& event = snapshot.Raw[i];
		const auto slot = event.PairingSlot();
		if (event.IsNoteOn())
		{
			auto start = static_cast<std::int64_t>(event.sampleOffset);
			if (snapshot.QuantisationActive && !event.HasExactTiming())
			{
				const auto transport = static_cast<std::int64_t>(snapshot.QuantisationTransportStartSamps);
				const auto origin = grid.HasRemoteGrid() ? grid.RemoteOriginSamps : 0ll;
				const auto cell = MidiQuantisation::NearestBoundaryIndex(transport + start - origin,
					grid.GridInterval(), grid.GridDivisions());
				start = origin + MidiQuantisation::BoundarySampleAt(cell,
					grid.GridInterval(), grid.GridDivisions()) - transport + grid.PhaseOffsetSamps;
			}
			pending[slot].push_back(spans.size());
			spans.push_back({ i, count, spans.size(), start, start });
		}
		else if (event.IsNoteOff() && read[slot] < pending[slot].size())
		{
			auto& span = spans[pending[slot][read[slot]++]];
			span.Off = i;
			span.End = span.Start + static_cast<std::int64_t>(event.sampleOffset)
				- snapshot.Raw[span.On].sampleOffset;
		}
	}
	auto find = [&spans](std::size_t index, std::int64_t& shift)
	{
		shift = 0;
		while (spans[index].Parent != index)
		{
			shift += spans[index].Shift;
			index = spans[index].Parent;
		}
		return index;
	};
	// Compare only compatible endpoints. In particular, an unmatched-only
	// recording must not scan every possible pair during owner publication.
	// The surrounding completion/export paths already use substantial stack
	// storage. Allocate these owner-only lookup tables away from that stack.
	std::vector<std::vector<std::size_t>> finiteByNote(TotalNoteSlots);
	std::vector<std::vector<std::size_t>> unmatchedByNote(TotalNoteSlots);
	for (std::size_t i = 0; i < spans.size(); ++i)
	{
		const auto& event = snapshot.Raw[spans[i].On];
		const auto slot = NoteSlot(event.Channel(), event.data1);
		if (spans[i].Off == count) unmatchedByNote[slot].push_back(i);
		else finiteByNote[slot].push_back(i);
	}
	// Compare original source repeats before modulo wrapping. Same-repeat
	// overlaps and endpoints that merely touch never create a merge edge.
	for (const auto& noteSpans : finiteByNote)
	for (const auto i : noteSpans)
	for (const auto j : noteSpans)
	{
		const auto& a = spans[i];
		const auto& b = spans[j];
		// A self-overlapping sustain must not absorb a shorter articulation
		// already overlapping it in the same source repeat. Its own repeat
		// establishes continuity without erasing that legitimate retrigger.
		if (i != j && (a.End - a.Start > length || b.End - b.Start > length)
			&& a.Start < b.End && b.Start < a.End) continue;
		if (snapshot.Raw[a.On].sampleOffset >= length || snapshot.Raw[b.On].sampleOffset >= length
			|| snapshot.Raw[a.On].Channel() != snapshot.Raw[b.On].Channel()
			|| snapshot.Raw[a.On].data1 != snapshot.Raw[b.On].data1
			|| a.End <= a.Start || b.End <= b.Start
			|| a.Off == count || b.Off == count
			|| a.Start >= b.End + length || b.Start + length >= a.End) continue;
		std::int64_t aShift, bShift;
		const auto aRoot = find(i, aShift);
		const auto bRoot = find(j, bShift);
		const auto translation = length + aShift - bShift;
		if (aRoot == bRoot)
		{
			if (translation != 0) spans[aRoot].Continuous = true;
		}
		else if (spans[aRoot].Rank < spans[bRoot].Rank)
		{
			spans[aRoot].Parent = bRoot;
			spans[aRoot].Shift = -translation;
			spans[bRoot].Continuous |= spans[aRoot].Continuous;
			spans[aRoot].Continuous = false;
		}
		else
		{
			spans[bRoot].Parent = aRoot;
			spans[bRoot].Shift = translation;
			spans[aRoot].Continuous |= spans[bRoot].Continuous;
			spans[bRoot].Continuous = false;
			if (spans[aRoot].Rank == spans[bRoot].Rank) ++spans[aRoot].Rank;
		}
	}
	for (std::size_t i = 0; i < spans.size(); ++i)
	{
		std::int64_t shift;
		auto& group = spans[find(i, shift)];
		const auto start = spans[i].Start + shift;
		const auto end = spans[i].End + shift;
		if (group.Members++ == 0 || start < group.FirstSample
			|| (start == group.FirstSample && snapshot.Raw[spans[i].On].sampleOffset
				> snapshot.Raw[spans[group.First].On].sampleOffset))
		{
			group.First = i;
			group.FirstSample = start;
		}
		if (end > group.LastSample)
		{
			group.LastSample = end;
			if (spans[i].Off != count) group.Release = spans[i].Off;
		}
	}
	bool merged = false;
	for (std::size_t i = 0; i < spans.size(); ++i)
	{
		std::int64_t shift;
		const auto root = find(i, shift);
		const auto& group = spans[root];
		if (group.Members < 2u && !group.Continuous) continue;
		snapshot.QuantisedContinuationRoots[spans[i].On] = static_cast<std::uint16_t>(root + 1u);
		merged = true;
		const auto groupStart = ((group.FirstSample % length) + length) % length;
		const auto memberStart = ((spans[i].Start % length) + length) % length;
		const bool initialHead = !group.Continuous && memberStart < groupStart
			&& groupStart + group.LastSample - group.FirstSample >= length;
		// Retain the initial head articulation; later repeats suppress it while
		// this group's tail continues. Unrelated source retriggers remain ordinary.
		snapshot.Quantised[spans[i].On].sampleOffset = initialHead
			? static_cast<std::uint32_t>(memberStart) : snapshot.LoopLengthSamps;
		snapshot.QuantisedUnmatchedStarts[spans[i].On] = initialHead ? MergedHead : 0u;
		if (spans[i].Off != count)
			snapshot.Quantised[spans[i].Off].sampleOffset = snapshot.LoopLengthSamps;
	}
	for (const auto& group : spans)
	{
		if (group.Members < 2u && !group.Continuous) continue;
		const auto on = spans[group.First].On;
		const auto start = ((group.FirstSample % length) + length) % length;
		const auto duration = group.LastSample - group.FirstSample;
		snapshot.Quantised[on].sampleOffset = static_cast<std::uint32_t>(start);
		snapshot.QuantisedUnmatchedStarts[on] = group.Continuous || start + duration >= length
			? MergedContinuation : 0u;
		if (!group.Continuous)
			snapshot.Quantised[group.Release].sampleOffset = static_cast<std::uint32_t>((start + duration) % length);
	}
	// Existing unmatched tails already continue across ordinary seams. A
	// wrapped tail can coincide with its next-repeat paired head, however.
	// Suppress that head only when their unwrapped starts are one repeat apart;
	// its original release still ends the continuation at the recorded time.
	for (std::size_t slot = 0; slot < TotalNoteSlots; ++slot)
	for (const auto tailIndex : unmatchedByNote[slot])
	for (const auto headIndex : finiteByNote[slot])
	{
		const auto& tail = spans[tailIndex];
		const auto& head = spans[headIndex];
		if (tail.Off != count || head.Off == count || head.End <= head.Start
			|| tail.Start != head.Start + length
			|| snapshot.Raw[tail.On].sampleOffset >= length
			|| snapshot.Raw[head.On].sampleOffset >= length
			|| snapshot.Raw[tail.On].Channel() != snapshot.Raw[head.On].Channel()
			|| snapshot.Raw[tail.On].data1 != snapshot.Raw[head.On].data1
			|| snapshot.Quantised[tail.On].sampleOffset >= length
			|| snapshot.Quantised[tail.On].sampleOffset != snapshot.Quantised[head.On].sampleOffset) continue;
		snapshot.Quantised[head.On].sampleOffset = snapshot.LoopLengthSamps;
		snapshot.QuantisedUnmatchedStarts[head.On] = 0u;
		merged = true;
	}
	return merged;
}

bool MidiLoop::PublishCompletedEvents(const MidiEvent* raw, std::size_t count,
	std::uint32_t length, std::uint64_t revision,
	const MidiQuantisationSettings& quantisation,
	std::uint64_t transportStart, bool sourceChanged,
	bool inheritActiveGrid, bool ownerGateHeld) noexcept
{
	if (count > DefaultCapacity || length == 0u)
		return false;
	if (!ownerGateHeld) AcquireOwnerPublicationGate();
	const auto handoffBefore = _playbackActivationVersion.load(std::memory_order_seq_cst);
	const auto* current = _playbackSnapshot.load(std::memory_order_seq_cst);
	const auto activated = _preparedGridActivated.load(std::memory_order_seq_cst);
	const auto& effectiveQuantisation = inheritActiveGrid
		? (current ? current->Quantisation
			: (activated ? _preparedOwnerQuantisation : quantisation))
		: quantisation;
	const auto effectiveTransportStart = inheritActiveGrid
		? (current ? current->QuantisationTransportStartSamps
			: (activated ? _preparedOwnerTransportStartSamps : transportStart))
		: transportStart;
	const auto* prepared = _preparedPlaybackSnapshot.load(std::memory_order_seq_cst);
	const auto readers = _playbackReaders.load(std::memory_order_seq_cst);
	const auto handoffAfter = _playbackActivationVersion.load(std::memory_order_seq_cst);
	if (readers == 0u && handoffBefore == handoffAfter && (handoffAfter & 1u) == 0u)
	{
		for (std::size_t i = 0u; i < _playbackBuffers->size(); ++i)
			if (&(*_playbackBuffers)[i] != current && &(*_playbackBuffers)[i] != prepared)
				_playbackBufferUsed[i] = false;
	}
	std::size_t slot = _playbackBuffers->size();
	for (std::size_t i = 0u; i < _playbackBuffers->size(); ++i)
		if (!_playbackBufferUsed[i]) { slot = i; break; }
	if (slot == _playbackBuffers->size())
	{
		if (!ownerGateHeld) ReleaseOwnerPublicationGate();
		return false;
	}
	auto& snapshot = (*_playbackBuffers)[slot];
	std::copy_n(raw, count, snapshot.Raw.begin());
	snapshot.EventCount = count;
	snapshot.LoopLengthSamps = length;
	snapshot.Revision = revision;
	snapshot.SourceRevision = sourceChanged ? revision : _sourceRevision;
	snapshot.Quantisation = effectiveQuantisation;
	snapshot.QuantisationTransportStartSamps = effectiveTransportStart;
	ClassifyUnmatchedStarts(snapshot);
	BuildChaseHead(snapshot, false);
	snapshot.QuantisationActive = effectiveQuantisation.Enabled
		&& effectiveQuantisation.GridInterval() > 0u && effectiveQuantisation.GridDivisions() > 0u && count > 0u;
	if (snapshot.QuantisationActive)
	{
		MidiQuantisation::BuildQuantisedPlaybackEvents(snapshot.Raw.data(), count,
			length, effectiveQuantisation, effectiveTransportStart, snapshot.Quantised.data(), false);
	}
	else
		std::copy_n(snapshot.Raw.begin(), count, snapshot.Quantised.begin());
	std::copy_n(snapshot.RawUnmatchedStarts.begin(), count, snapshot.QuantisedUnmatchedStarts.begin());
	snapshot.QuantisedContinuationRoots.fill(0u);
	snapshot.PlaybackViewActive = MergePlaybackSeamNotes(snapshot) || snapshot.QuantisationActive;
	if (snapshot.PlaybackViewActive)
	{
		MidiNote::SortMidiEvents(snapshot.Quantised.data(), count, snapshot.QuantisedUnmatchedStarts.data(),
			snapshot.QuantisedContinuationRoots.data());
		BuildChaseHead(snapshot, true);
	}
	_playbackBufferUsed[slot] = true;
	if (_prepareNextPublication)
		_preparedPlaybackSnapshot.store(&snapshot, std::memory_order_seq_cst);
	else
	{
		_preparedPlaybackSnapshot.store(nullptr, std::memory_order_seq_cst);
		_playbackSnapshot.store(&snapshot, std::memory_order_seq_cst);
	}
	_sourceRevision = snapshot.SourceRevision;
	if (!ownerGateHeld) ReleaseOwnerPublicationGate();
	return true;
}

bool MidiLoop::ShouldSuppressContinuedAttack(bool continuing, bool merged,
	std::uint8_t startKind, std::uint16_t soundingRoot, std::uint16_t attackRoot) noexcept
{
	// Natural unmatched notes retain their slot policy. Merged components
	// suppress only their own repeated root/head, not another articulation.
	return continuing && (!merged ||
		((startKind == MergedContinuation || startKind == MergedHead) && soundingRoot == attackRoot));
}

void MidiLoop::AdvanceChaseState(ChaseState& state, const MidiEvent& event,
	std::uint8_t startKind, std::uint16_t root) noexcept
{
	if (!event.IsNoteOn() && !event.IsNoteOff()) return;
	const auto slot = NoteSlot(event.Channel(), event.data1);
	if (event.IsNoteOff())
	{
		state.Held.reset(slot);
		state.Continuing.reset(slot);
		state.MergedContinuing.reset(slot);
		return;
	}
	if (ShouldSuppressContinuedAttack(state.Continuing.test(slot), state.MergedContinuing.test(slot),
		startKind, state.ContinuationRoot[slot], root)) return;
	state.Held.set(slot);
	state.Continuing.set(slot, startKind == NaturalContinuation || startKind == MergedContinuation);
	state.MergedContinuing.set(slot, startKind == MergedContinuation);
	state.Velocity[slot] = event.data2;
	state.Flags[slot] = event.flags;
	state.ContinuationRoot[slot] = root;
}

void MidiLoop::BuildChaseHead(PlaybackSnapshot& snapshot, bool quantised) noexcept
{
	auto& head = quantised ? snapshot.QuantisedChaseHead : snapshot.RawChaseHead;
	head = {};
	const auto& events = quantised ? snapshot.Quantised : snapshot.Raw;
	const auto& unmatched = quantised ? snapshot.QuantisedUnmatchedStarts : snapshot.RawUnmatchedStarts;
	// Two passes establish the stable state entering a repeated loop: a tail
	// continuation may suppress a paired attack at the next head.
	for (unsigned int pass = 0u; pass < 2u; ++pass)
	{
		for (std::size_t i = 0u; i < snapshot.EventCount; ++i)
			if (events[i].sampleOffset < snapshot.LoopLengthSamps)
				AdvanceChaseState(head, events[i], unmatched[i],
					quantised ? snapshot.QuantisedContinuationRoots[i] : 0u);
		head.Held &= head.Continuing; // Ordinary paired voices release at the seam.
	}
}

bool MidiLoop::ChaseAtCursor(std::uint32_t sample, const PlaybackSnapshot& snapshot,
	IMidiSink& sink) noexcept
{
	const bool quantised = snapshot.PlaybackViewActive;
	auto state = quantised ? snapshot.QuantisedChaseHead : snapshot.RawChaseHead;
	const auto& events = quantised ? snapshot.Quantised : snapshot.Raw;
	const auto& unmatched = quantised ? snapshot.QuantisedUnmatchedStarts : snapshot.RawUnmatchedStarts;
	const auto offset = sample % snapshot.LoopLengthSamps;
	bool accepted = true;
	for (std::size_t i = 0u; i < snapshot.EventCount; ++i)
		if (events[i].sampleOffset < offset)
			AdvanceChaseState(state, events[i], unmatched[i],
				quantised ? snapshot.QuantisedContinuationRoots[i] : 0u);
	for (std::uint8_t channel = 0u; channel < 16u; ++channel)
		for (std::uint8_t note = 0u; note < 128u; ++note)
		{
			const auto slot = NoteSlot(channel, note);
			if (!state.Held.test(slot) || _held.test(slot)) continue;
			const auto attack = MidiEvent::MakeNoteOn(sample, channel, note,
				state.Velocity[slot]).WithFlags(state.Flags[slot]);
			if (!sink.TryOnEvent(attack)) { accepted = false; continue; }
			_held.set(slot);
			_continuing.set(slot, state.Continuing.test(slot));
			_mergedContinuing.set(slot, state.MergedContinuing.test(slot));
			_continuationRoot[slot] = state.ContinuationRoot[slot];
			_heldPublished[slot].store(1u, std::memory_order_seq_cst);
		}
	return accepted;
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
