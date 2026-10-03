#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace console
{
	// Multi-producer, single-consumer bounded handoff. Publish copies into a
	// preallocated slot with a bounded CAS attempt count and never waits or allocates.
	class LineRing
	{
	public:
		static constexpr std::size_t Capacity = 256;
		static constexpr std::size_t MaxLineBytes = 4 * 1024;
		LineRing() : _slots(std::make_unique<Slot[]>(Capacity))
		{
			for (std::size_t index = 0; index < Capacity; ++index)
				_slots[index].Sequence.store(index, std::memory_order_relaxed);
		}
		bool Publish(std::string_view line) noexcept
		{
			if (line.size() > MaxLineBytes)
			{
				_dropped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			auto position = _write.load(std::memory_order_relaxed);
			for (unsigned attempt = 0; attempt < 8; ++attempt)
			{
				auto& slot = _slots[position % Capacity];
				const auto sequence = slot.Sequence.load(std::memory_order_acquire);
				if (sequence == position)
				{
					if (!_write.compare_exchange_weak(position, position + 1,
						std::memory_order_relaxed)) continue;
					slot.Length = line.size();
					if (slot.Length) std::memcpy(slot.Bytes.data(), line.data(), slot.Length);
					slot.Sequence.store(position + 1, std::memory_order_release);
					return true;
				}
				if (sequence < position)
				{
					_dropped.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				position = _write.load(std::memory_order_relaxed);
			}
			_dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		std::optional<std::string> Take()
		{
			auto& slot = _slots[_read % Capacity];
			if (slot.Sequence.load(std::memory_order_acquire) != _read + 1)
				return std::nullopt;
			std::string line(slot.Bytes.data(), slot.Length);
			slot.Sequence.store(_read + Capacity, std::memory_order_release);
			++_read;
			return line;
		}
		std::size_t ConsumeDropped() noexcept
		{
			return _dropped.exchange(0, std::memory_order_acq_rel);
		}
	private:
		struct Slot
		{
			std::atomic<std::size_t> Sequence{ 0 };
			std::size_t Length = 0;
			std::array<char, MaxLineBytes> Bytes{};
		};
		std::unique_ptr<Slot[]> _slots;
		std::atomic<std::size_t> _write{ 0 };
		std::size_t _read = 0; // app owner only
		std::atomic<std::size_t> _dropped{ 0 };
	};
}
