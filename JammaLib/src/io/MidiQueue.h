#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../engine/MidiEvent.h"

namespace io
{
	// Lock-free single-producer/single-consumer ring of MidiEvents for the MIDI ingress path.
	//
	// Real-time invariants:
	//   - No heap allocation: storage is a fixed-size std::array.
	//   - No locks: only acquire/release atomics.
	//   - Push (producer / MIDI device callback) and Pop (consumer / job thread) are wait-free.
	//
	// Overflow policy: drop-newest.
	// When the ring is full, Push() leaves queued events untouched, increments dropped count,
	// and returns false so the caller can count drops.
	//
	// Capacity is a compile-time power of two so head/tail can be masked instead of modded.
	template <std::size_t Capacity>
	class MidiQueue
	{
		static_assert(Capacity >= 2, "Capacity must be at least 2");
		static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

	public:
		using value_type = engine::MidiEvent;
		static constexpr std::size_t capacity = Capacity;

		MidiQueue() noexcept
			: _head(0), _tail(0), _dropped(0)
		{
		}

		MidiQueue(const MidiQueue&) = delete;
		MidiQueue& operator=(const MidiQueue&) = delete;

		// Producer side. Returns true if the event was stored.
		// Returns false if the ring is full and the newest event is dropped.
		bool Push(const value_type& ev) noexcept
		{
			const auto tail = _tail.load(std::memory_order_relaxed);
			const auto head = _head.load(std::memory_order_acquire);
			const auto next = (tail + 1) & Mask;

			if (next == head)
			{
				// Full: drop newest to preserve SPSC single-writer ownership of _head.
				_dropped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}

			_buffer[tail] = ev;
			_tail.store(next, std::memory_order_release);
			return true;
		}

		// Consumer side. Returns false if empty.
		bool Pop(value_type& out) noexcept
		{
			const auto head = _head.load(std::memory_order_relaxed);
			const auto tail = _tail.load(std::memory_order_acquire);
			if (head == tail)
				return false;

			out = _buffer[head];
			_head.store((head + 1) & Mask, std::memory_order_release);
			return true;
		}

		bool Empty() const noexcept
		{
			return _head.load(std::memory_order_acquire) == _tail.load(std::memory_order_acquire);
		}

		std::size_t Size() const noexcept
		{
			const auto head = _head.load(std::memory_order_acquire);
			const auto tail = _tail.load(std::memory_order_acquire);
			return (tail - head) & Mask;
		}

		std::uint64_t DroppedCount() const noexcept
		{
			return _dropped.load(std::memory_order_relaxed);
		}

		void Clear() noexcept
		{
			_head.store(0, std::memory_order_release);
			_tail.store(0, std::memory_order_release);
			_dropped.store(0, std::memory_order_relaxed);
		}

	private:
		static constexpr std::size_t Mask = Capacity - 1;

		std::array<value_type, Capacity> _buffer{};
		std::atomic<std::size_t> _head;
		std::atomic<std::size_t> _tail;
		std::atomic<std::uint64_t> _dropped;
	};
}