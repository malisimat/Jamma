///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vst
{
	// Fixed-capacity single-producer/single-consumer queue of controller
	// edits, used to forward UI-thread parameter edits
	// (IComponentHandler::performEdit) to the audio thread so they reach the
	// processor's IParameterChanges on the next process block.
	//
	// Threading contract: exactly one producer thread (the VST3 SDK
	// guarantees performEdit is called from a single UI thread) and exactly
	// one consumer thread (the audio thread, once per block). No allocation,
	// no locks, no logging in Push()/Pop(). When full, Push() drops the
	// incoming edit rather than overwriting an unread slot or advancing the
	// consumer's read index — losing one intermediate automation value under
	// sustained overflow is preferable to corrupting queue ordering.
	class Vst3ControllerEditQueue
	{
	public:
		struct Edit
		{
			std::uint32_t ParamId = 0u;
			double Value = 0.0;
		};

		// Must be a power of two so index wraparound can use a mask instead
		// of a modulo.
		static constexpr std::size_t Capacity = 256u;

		Vst3ControllerEditQueue() noexcept : _slots{}, _writeIndex(0), _readIndex(0)
		{
		}

		Vst3ControllerEditQueue(const Vst3ControllerEditQueue&) = delete;
		Vst3ControllerEditQueue& operator=(const Vst3ControllerEditQueue&) = delete;

		// Producer only. Returns false (and drops the edit) if the queue is
		// currently full.
		bool Push(std::uint32_t paramId, double value) noexcept
		{
			const auto writeIndex = _writeIndex.load(std::memory_order_relaxed);
			const auto readIndex = _readIndex.load(std::memory_order_acquire);
			if (writeIndex - readIndex >= Capacity)
				return false;

			_slots[writeIndex & IndexMask] = Edit{ paramId, value };
			_writeIndex.store(writeIndex + 1, std::memory_order_release);
			return true;
		}

		// Consumer only. Returns false when the queue is empty.
		bool Pop(Edit& out) noexcept
		{
			const auto readIndex = _readIndex.load(std::memory_order_relaxed);
			const auto writeIndex = _writeIndex.load(std::memory_order_acquire);
			if (readIndex == writeIndex)
				return false;

			out = _slots[readIndex & IndexMask];
			_readIndex.store(readIndex + 1, std::memory_order_release);
			return true;
		}

		// Non-RT only: drop all pending entries. Call only when neither the
		// UI-thread producer nor the audio-thread consumer can be active
		// concurrently (e.g. while unloading a plugin).
		void Clear() noexcept
		{
			_writeIndex.store(0, std::memory_order_relaxed);
			_readIndex.store(0, std::memory_order_relaxed);
		}

	private:
		static constexpr std::size_t IndexMask = Capacity - 1u;
		static_assert((Capacity & IndexMask) == 0u, "Capacity must be a power of two");

		std::array<Edit, Capacity> _slots;
		std::atomic<std::size_t> _writeIndex;
		std::atomic<std::size_t> _readIndex;
	};
}
