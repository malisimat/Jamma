#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../audio/SerialTriggerProtocol.h"

namespace engine
{
	template <std::size_t Capacity>
	class SerialTriggerQueue
	{
		static_assert(Capacity >= 2, "Capacity must be at least 2");
		static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

	public:
		using value_type = audio::SerialTriggerEvent;
		static constexpr std::size_t capacity = Capacity;

		SerialTriggerQueue() noexcept
			: _head(0), _tail(0), _dropped(0)
		{
		}

		bool Push(const value_type& ev) noexcept
		{
			const auto tail = _tail.load(std::memory_order_relaxed);
			const auto head = _head.load(std::memory_order_acquire);
			const auto next = (tail + 1) & Mask;

			if (next == head)
			{
				_dropped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}

			_buffer[tail] = ev;
			_tail.store(next, std::memory_order_release);
			return true;
		}

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