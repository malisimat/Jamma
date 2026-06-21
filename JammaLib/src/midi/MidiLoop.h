#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "../graphics/MidiModel.h"
#include "../include/Constants.h"
#include "MidiEvent.h"
#include "MidiQuantisation.h"

namespace vst
{
	class IVstPlugin;
}

namespace midi
{
	using namespace graphics;

	// Sink for MIDI playback. Implementations should not allocate or block.
	class IMidiSink
	{
	public:
		virtual ~IMidiSink() = default;
		virtual void OnEvent(const MidiEvent& ev) noexcept = 0;
	};

	class IMidiOutputSink
	{
	public:
		virtual ~IMidiOutputSink() = default;
		virtual void OnEvent(unsigned int outputIndex, const MidiEvent& ev) noexcept = 0;
	};


	enum class MidiLoopState : std::uint8_t
	{
		Empty,
		Recording,

		Playing
	};

	// Metadata describing how a CC controller is wired to a hosted plugin
	// parameter for one automation lane.
	struct AutomationMapping
	{
		// Active, Channel, and CC must be read atomically together on the MIDI
		// thread (CC matching) while written together on the UI thread (wire key).
		// Pack all three into one uint32_t so a reader always sees a consistent
		// triple. Encoding: bit 16 = Active, bits [15:8] = Channel, bits [7:0] = CC.
		std::atomic<std::uint32_t> MatchKey{ 0u };

		// Written and read on the non-audio thread only (_RebuildAutomationDispatch
		// and the wire/delete key handlers). No atomic needed.
		vst::IVstPlugin* TargetPlugin{ nullptr };
		unsigned int     TargetParameterIndex{ 0u };

		static constexpr std::uint32_t kInactive = 0u;

		static constexpr std::uint32_t MakeMatchKey(std::uint8_t ch, std::uint8_t cc) noexcept
		{
			return (1u << 16) | (static_cast<std::uint32_t>(ch) << 8) | static_cast<std::uint32_t>(cc);
		}

		// Match key for a lane wired directly from a plugin editor drag (no CC
		// source). Active so it renders and plays back, but uses out-of-range
		// channel/CC sentinels (0xFF) that no real incoming CC can ever match, so
		// live CC recording never writes into an editor-driven lane.
		static constexpr std::uint32_t MakeEditorMatchKey() noexcept
		{
			return MakeMatchKey(0xFFu, 0xFFu);
		}

		bool IsActive() const noexcept
		{
			return ((MatchKey.load(std::memory_order_relaxed) >> 16) & 1u) != 0u;
		}
		std::uint8_t GetChannel() const noexcept
		{
			return static_cast<std::uint8_t>(MatchKey.load(std::memory_order_relaxed) >> 8);
		}
		std::uint8_t GetCC() const noexcept
		{
			return static_cast<std::uint8_t>(MatchKey.load(std::memory_order_relaxed));
		}
	};

	// One self-contained automation lane: its CC->parameter mapping plus its own
	// sparse control-point buffer recorded along the loop timeline.
	struct AutomationLane
	{
		// Keep the storage aligned with the renderer's uniform cap so recording and
		// display stay bounded by the same predictable limit.
		static constexpr std::size_t MaxPoints = 512u;

		AutomationMapping Mapping;
		std::array<std::pair<float, float>, MaxPoints> Points{}; // (frac, value)
		std::size_t PointCount = 0u;

		// Seqlock generation counter. The MIDI thread (the only writer of Points /
		// PointCount) bumps this to an odd value before mutating the buffer and back
		// to an even value afterwards. Both the audio thread (GetAutomationValueAtCursor)
		// and the render thread (SnapshotAutomationLanePoints) participate in a bounded
		// retry loop so neither ever observes a half-shifted buffer.
		std::atomic<std::uint32_t> Revision{ 0u };
	};

	// In-memory MIDI loop. Records sample-offset-stamped events relative to the
	// start of the loop, then plays them back through an IMidiSink with stable
	// sample-relative timing across arbitrary block boundaries.
	//
	// Real-time invariants:
	//   - All storage is preallocated. RecordEvent / ReadBlock perform no heap allocation.
	//   - When event capacity is reached during recording, further events are dropped
	//     (Drop-newest; the recorded loop is what it is).
	//   - Loop wrap during playback emits synthetic NoteOff for any held notes so no
	//     stuck notes leak across the wrap.
	class MidiLoop
	{
	public:
		static constexpr std::size_t DefaultCapacity = 4096;
		static constexpr std::size_t TotalNoteSlots = 16u * 128u; // channel * note

	private:
		struct QuantisedEventBuffer
		{
			std::array<MidiEvent, DefaultCapacity> Events{};
		};

	public:
		MidiLoop() noexcept;

		// State transitions
		void StartRecord() noexcept;
		// Append an event during recording. `ev.sampleOffset` is the offset (in samples)
		// since StartRecord(). Returns false if capacity was exceeded (event dropped).
		bool RecordEvent(const MidiEvent& ev) noexcept;
		// Append an event during non-RT build/finalization flows (for example,
		// MIDI overdub render). Uses the same fixed-capacity drop-newest behavior
		// as RecordEvent.
		bool AppendEventForBuild(const MidiEvent& ev) noexcept;
		// Replace all recorded events with a caller-built set and transition to
		// Playing. If count exceeds capacity, excess tail events are dropped.
		void ReplaceRecordedEvents(const MidiEvent* events,
			std::size_t count,
			std::uint32_t loopLengthSamps);
		// Finalize a caller-built event set that was assembled through
		// AppendEventForBuild.
		void FinalizeOverdubBase(std::uint32_t loopLengthSamps);
		// Finalize the recording with an explicit loop length in samples and transition
		// to Playing. Events whose offset >= loopLengthSamps are kept in storage but will
		// not be emitted (they are outside the playable window).
		// startGlobalSample is the global audio sample at which loop position 0 maps:
		// used by automation dispatch to compute loop-relative fracs correctly.
		void EndRecord(std::uint32_t loopLengthSamps, std::uint32_t startGlobalSample = 0u);
		void Reset() noexcept;

		// Play any events that fall within [globalSample, globalSample + numSamples).
		// Time mapping: loopOffset = globalSample % LoopLengthSamps().
		// Emitted events' `sampleOffset` is rewritten to the absolute global sample at
		// which the sink should treat them as occurring. If the block crosses a loop
		// boundary, held notes are flushed with synthetic NoteOffs at the wrap sample.
		void ReadBlock(std::uint32_t globalSample,
			std::uint32_t numSamples,
			IMidiSink& sink) noexcept;

		// Accessors
		MidiLoopState State() const noexcept { return _state; }
		std::size_t EventCount() const noexcept { return _eventCount; }
		std::uint32_t LoopLengthSamps() const noexcept { return _loopLengthSamps; }
		// Global sample that maps to loop-relative position 0.  Use to convert a
		// global sample counter into a loop-relative frac:
		//   frac = (globalSample - LoopPhaseAnchor()) % loopLen / loopLen
		std::uint32_t LoopPhaseAnchor() const noexcept { return _loopPhaseAnchor; }
		std::uint64_t DroppedEventCount() const noexcept { return _dropped; }
		std::uint64_t Revision() const noexcept { return _revision; }
		// Notes that have been emitted as NoteOn but whose NoteOff has not yet been played.
		// Used by the ditch path to flush stuck notes before the loop is discarded.
		const std::bitset<TotalNoteSlots>& HeldNotes() const noexcept { return _held; }
		bool TryGetEvent(std::size_t index, MidiEvent& ev) const noexcept;
		void AttachModel(std::shared_ptr<MidiModel> model) noexcept;
		std::shared_ptr<MidiModel> Model() const noexcept { return _model; }
		bool UpdateModelFromEvents(std::uint32_t displayLengthSamps = 0u, bool force = false);
		bool QueueModelUpdateFromEvents(std::uint32_t displayLengthSamps = 0u, bool force = false);
		static constexpr std::size_t Capacity() noexcept { return DefaultCapacity; }

		// --- Parameter automation lanes ---
		static constexpr std::size_t MaxAutomationLanes = 8u;

		AutomationLane& GetLane(std::size_t idx) noexcept { return _lanes[idx]; }
		const AutomationLane& GetLane(std::size_t idx) const noexcept { return _lanes[idx]; }

		// Write a control point for lane laneIdx at fractional loop position frac
		// (0..1). If a point already exists at (approximately) frac the value is
		// updated in place; otherwise the point is inserted in frac order. Real-time
		// safe: fixed-capacity storage, no allocation. Points beyond capacity are
		// dropped (drop-newest). Called on the MIDI thread during recording.
		void SetAutomationValueAtFrac(std::size_t laneIdx, double frac, float value) noexcept;

		// Replace automation in the sample-domain half-open window
		// [startSample, startSample + durationSamples) with a held value, then
		// write that same value again at the window end so playback holds steady
		// until the next control point. Non-RT helper for editor-driven automation.
		void OverwriteAutomationWindow(std::size_t laneIdx,
			std::uint32_t startSample,
			std::uint32_t durationSamples,
			float value) noexcept;

		// Cursor-advancing read on lane laneIdx: advances cursorIdx forward to the
		// correct bracket for frac, returns the piecewise-linearly interpolated
		// value. Resets the cursor on loop wrap (detected when frac steps backward).
		// Amortised O(1) per block. Returns 0 when the lane has no points.
		float GetAutomationValueAtCursor(std::size_t laneIdx, double frac, std::uint16_t& cursorIdx) const noexcept;

		// Clear a lane's mapping and control points (non-audio thread; delete key).
		void ClearAutomationLane(std::size_t laneIdx) noexcept;

		// Clear only a lane's recorded points while preserving mapping metadata.
		// Used by editor-driven overwrite mode to replace an existing curve from the
		// first drag event in a new automation-record gesture.
		void ClearAutomationLanePoints(std::size_t laneIdx) noexcept;

		// Resolve which lane should host editor-driven automation for the given
		// (plugin, parameter) pair. Resolution rule: first an active lane already
		// mapped to that pair (reuse), otherwise the first inactive lane (claim),
		// otherwise std::nullopt (full). Pure query: never mutates a lane. Called on
		// the non-audio (MIDI pump) thread.
		std::optional<std::size_t> ResolveAutomationLaneFor(const vst::IVstPlugin* plugin,
			unsigned int paramIdx) const noexcept;

		// Wire a lane for editor-driven automation (no CC source). Sets the target
		// plugin/parameter and an editor match key. Returns true when the mapping
		// topology actually changed (so the caller can rebuild the audio dispatch),
		// false when the lane was already mapped to the same (plugin, parameter).
		// Non-audio thread only.
		bool WireEditorAutomationLane(std::size_t laneIdx,
			vst::IVstPlugin* plugin,
			unsigned int paramIdx) noexcept;

		// Render-thread consistent read of a lane's control points via the lane
		// seqlock. Copies up to maxPoints (frac, value) pairs into out and returns
		// the count actually copied. Returns 0 for an invalid lane. Safe to call
		// concurrently with MIDI-thread writes (retries on a torn read).
		std::uint16_t SnapshotAutomationLanePoints(std::size_t laneIdx,
			std::pair<float, float>* out, std::size_t maxPoints) const noexcept;

		// Whether a lane currently has a mapping wired. Used by the renderer to gate
		// and highlight active automation bands.
		bool IsAutomationLaneActive(std::size_t laneIdx) const noexcept;

		// Non-destructive start-time quantisation. Non-RT publication builds immutable
		// event buffers and publishes a raw pointer for audio-thread readers. Retained
		// buffers are not overwritten or freed until this MidiLoop is destroyed, so
		// ReadBlock never touches shared ownership or dangling storage.
		void SetQuantisation(const MidiQuantisationSettings& settings);
		const MidiQuantisationSettings& Quantisation() const noexcept { return _quantisation; }
		bool IsQuantisationActive() const noexcept { return nullptr != _quantisedEvents.load(std::memory_order_acquire); }

		// Update the sample rate used to project the ms-based automation merge
		// window into normalised frac space. Should be called whenever the audio
		// device sample rate changes, mirroring the pattern used for audio Loop.
		void SetSampleRate(float sampleRate) noexcept { _sampleRate = sampleRate; }

		static constexpr std::size_t NoteSlot(std::uint8_t channel, std::uint8_t note) noexcept
		{
			return (static_cast<std::size_t>(channel & 0x0F) << 7) | (note & 0x7F);
		}

	private:
		bool BuildModelFromEvents(std::uint32_t displayLengthSamps, bool force, bool queueUpdate);
		void EmitEventsInRange(std::uint32_t lo,
			std::uint32_t hi,
			std::uint32_t globalBase,
			IMidiSink& sink) noexcept;
		void FlushHeldNotes(std::uint32_t atGlobalSample, IMidiSink& sink) noexcept;
		void PublishQuantisedEvents();

		// --- Automation point helpers ---
		static constexpr std::uint32_t MidiModelUpdateIntervalSamps = constants::DefaultSampleRate / 30u;
		static constexpr float AutomationMergeWindowMs = 10.0f;
		static constexpr float AutomationFutureProtectWindowMs = 800.0f;

		// Convert a duration in milliseconds to a fractional position within the
		// loop, clamped to [0, 1]. Returns 0 when loop length or sample rate is not resolved.
		static float MsToLoopFrac(float ms, float sampleRate, std::uint32_t loopLengthSamps) noexcept;

		// True when candidateFrac falls inside the half-open circular window
		// [startFrac, startFrac + windowFrac) (with loop wraparound).
		static bool FracIsAheadWithin(float candidateFrac, float startFrac, float windowFrac) noexcept;

		static float ClampAutomationFrac(double frac) noexcept;
		static void InsertOrUpdateAutomationPoint(
			std::array<std::pair<float, float>, AutomationLane::MaxPoints>& points,
			std::size_t& count,
			float sampleRate,
			std::uint32_t loopLengthSamps,
			float frac,
			float value) noexcept;
		static float SampleToAutomationFrac(std::uint32_t sample, std::uint32_t loopLengthSamps) noexcept;
		static bool FracWithinOverwriteWindow(float frac, float startFrac, float endFrac, bool wraps) noexcept;

		std::array<MidiEvent, DefaultCapacity> _events{};
		std::atomic<const QuantisedEventBuffer*> _quantisedEvents;
		std::vector<std::unique_ptr<QuantisedEventBuffer>> _retainedQuantisedEvents;
		std::size_t _eventCount;
		float _sampleRate;
		std::uint32_t _loopLengthSamps;
		std::uint32_t _loopPhaseAnchor;
		std::uint64_t _dropped;
		std::uint64_t _revision;
		std::uint64_t _modelRevision;
		std::uint32_t _modelLengthSamps;
		MidiLoopState _state;
		std::bitset<TotalNoteSlots> _held;
		std::shared_ptr<MidiModel> _model;
		MidiQuantisationSettings _quantisation;
		std::array<AutomationLane, MaxAutomationLanes> _lanes{};
	};
}
