///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include "../midi/MidiEvent.h"

namespace vst
{
	// Pure VST3 MIDI-CC-to-parameter mapping logic. Deliberately independent
	// of the VST3 SDK (no Steinberg:: types) so it can be unit-tested without
	// JAMMA_VST3_ENABLED or a real plugin. Vst3Plugin.cpp is the only caller
	// that talks to the actual IMidiMapping SDK interface; it copies results
	// into the ParameterTable defined here.
	namespace Vst3MidiMapping
	{
		constexpr std::size_t ChannelCount = 16u;

		// CC 0..127, plus channel pressure (128) and pitch bend (129) —
		// mirrors Steinberg::Vst::kCountCtrlNumber (130) without depending on
		// the VST3 SDK headers.
		constexpr std::size_t ControllerCount = 130u;

		// Mirrors Steinberg::Vst::kAfterTouch / kPitchBend numeric values.
		constexpr std::uint8_t AfterTouchControllerNumber = 128u;
		constexpr std::uint8_t PitchBendControllerNumber = 129u;

		// Mirrors Steinberg::Vst::kNoParamId (0xffffffff).
		constexpr std::uint32_t NoParamId = 0xFFFFFFFFu;

		// [channel][controllerNumber] -> mapped ParamID, or NoParamId.
		using ParameterTable = std::array<std::array<std::uint32_t, ControllerCount>, ChannelCount>;

		// Returns a table with every entry set to NoParamId.
		ParameterTable MakeEmptyTable() noexcept;

		// A MIDI channel-voice message classified as a VST3 controller
		// number with its normalized [0, 1] value.
		struct ControllerValue
		{
			std::uint8_t ControllerNumber = 0u;
			float NormalizedValue = 0.0f;
		};

		// Classifies event as Control Change, Channel Pressure, or Pitch
		// Bend and fills out its VST3 controller number and normalized
		// [0, 1] value. Returns false for any other message type (notes,
		// program change, poly pressure, etc.) — those are not eligible for
		// IMidiMapping and stay on the legacy MIDI event path.
		bool TryClassify(const midi::MidiEvent& event, ControllerValue& out) noexcept;

		// Looks up the mapped ParamID for (channel, controllerNumber) in
		// table. Returns false when channel/controllerNumber are out of
		// range or no mapping exists (NoParamId).
		bool TryLookup(const ParameterTable& table,
			std::uint8_t channel,
			std::uint8_t controllerNumber,
			std::uint32_t& outParamId) noexcept;

		// Lock-free two-slot mailbox for publishing a freshly rebuilt
		// ParameterTable from a non-RT thread to the audio thread.
		//
		// Threading contract: single producer (non-RT rebuild call, e.g. the
		// owner's IdleEditor pump or a pre-save refresh — callers MUST
		// serialize concurrent producers themselves, e.g. with a mutex, since
		// this mailbox assumes only one producer is active at a time) and a
		// single consumer (the audio thread, once per block).
		//
		// Unlike an atomic<shared_ptr<ParameterTable>>, nothing is ever freed
		// on the audio thread: both slots are allocated for the mailbox's
		// entire lifetime, and the producer is only ever allowed to write
		// into the slot the consumer has already stopped reading.
		class ParameterTableMailbox
		{
		public:
			ParameterTableMailbox() noexcept
				: _slots{ MakeEmptyTable(), MakeEmptyTable() }
				, _published(0)
				, _consumerActive(0)
				, _pendingWriteSlot(-1)
			{
			}

			ParameterTableMailbox(const ParameterTableMailbox&) = delete;
			ParameterTableMailbox& operator=(const ParameterTableMailbox&) = delete;

			// Producer: returns a pointer to a table slot guaranteed not to
			// be the one the consumer is currently reading, ready to be
			// filled in place — or nullptr if the consumer has not yet
			// adopted the previous publish (caller should retry on its next
			// poll rather than blocking).
			ParameterTable* AcquireWriteSlot() noexcept
			{
				const auto consumerActive = _consumerActive.load(std::memory_order_acquire);
				const auto published = _published.load(std::memory_order_relaxed);
				if (published != consumerActive)
					return nullptr;

				_pendingWriteSlot = 1 - published;
				return &_slots[static_cast<std::size_t>(_pendingWriteSlot)];
			}

			// Producer: publish the table most recently returned by
			// AcquireWriteSlot(). No-op if AcquireWriteSlot() was not called
			// (or failed) since the last Publish().
			void Publish() noexcept
			{
				if (_pendingWriteSlot < 0)
					return;
				_published.store(_pendingWriteSlot, std::memory_order_release);
				_pendingWriteSlot = -1;
			}

			// Consumer (audio thread only): call once per block. Adopts the
			// latest published table if it differs from the one already
			// adopted (tracked by the caller-owned lastAdoptedSlot, which
			// must be initialized to 0 and touched only by the consumer).
			// Real-time safe: no allocation, bounded work.
			void AdoptPending(int& lastAdoptedSlot) noexcept
			{
				const auto published = _published.load(std::memory_order_acquire);
				if (published != lastAdoptedSlot)
				{
					lastAdoptedSlot = published;
					_consumerActive.store(published, std::memory_order_release);
				}
			}

			// Consumer (audio thread only): read the table adopted by the
			// most recent AdoptPending() call. Real-time safe: a bounded
			// array index, no atomics.
			const ParameterTable& Slot(int adoptedSlot) const noexcept
			{
				return _slots[static_cast<std::size_t>(adoptedSlot)];
			}

			// Non-RT only: clear both slots back to "no mappings" and reset
			// indices. Call only when the audio thread is guaranteed not to
			// be reading (matches Unload()'s existing precondition).
			void Reset() noexcept
			{
				_slots[0] = MakeEmptyTable();
				_slots[1] = MakeEmptyTable();
				_published.store(0, std::memory_order_relaxed);
				_consumerActive.store(0, std::memory_order_relaxed);
				_pendingWriteSlot = -1;
			}

		private:
			std::array<ParameterTable, 2> _slots;
			std::atomic<int> _published;
			std::atomic<int> _consumerActive;
			int _pendingWriteSlot;
		};
	}
}
