#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "MidiEvent.h"
#include "MidiQuantisation.h"

namespace engine
{
	class MidiModel;

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
		// Finalize the recording with an explicit loop length in samples and transition
		// to Playing. Events whose offset >= loopLengthSamps are kept in storage but will
		// not be emitted (they are outside the playable window).
		void EndRecord(std::uint32_t loopLengthSamps);
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

		// Non-destructive start-time quantisation. Builds an immutable event-buffer
		// snapshot and atomically publishes it for the audio thread. The original
		// _events stream is never modified; disabling restores untransformed playback
		// and rendering with bit-exact recorded timing.
		void SetQuantisation(const MidiQuantisationSettings& settings);
		const MidiQuantisationSettings& Quantisation() const noexcept { return _quantisation; }
		bool IsQuantisationActive() const noexcept { return nullptr != _quantisedEvents.load(std::memory_order_acquire); }

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

		std::array<MidiEvent, DefaultCapacity> _events{};
		std::atomic<std::shared_ptr<const QuantisedEventBuffer>> _quantisedEvents;
		std::size_t _eventCount;
		std::uint32_t _loopLengthSamps;
		std::uint64_t _dropped;
		std::uint64_t _revision;
		std::uint64_t _modelRevision;
		std::uint32_t _modelLengthSamps;
		MidiLoopState _state;
		std::bitset<TotalNoteSlots> _held;
		std::shared_ptr<MidiModel> _model;
		MidiQuantisationSettings _quantisation;
	};
}
