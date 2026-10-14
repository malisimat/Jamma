#pragma once

#include <array>
#include <cstddef>

namespace vst
{
	// Callback-owned fixed storage: panic always has 16 dedicated slots,
	// even when ordinary MIDI filled its quota. No existing event is erased.
	template<class Event, std::size_t NormalCapacity>
	class ReservedMidiEvents
	{
	public:
		static constexpr std::size_t PanicCapacity = 16;
		void Clear() noexcept { _normalCount = 0; _panicCount = 0; }
		bool Append(const Event& event, bool panic = false) noexcept
		{
			if (panic)
			{
				if (_panicCount >= PanicCapacity) return false;
				_events[NormalCapacity + _panicCount++] = event;
			}
			else
			{
				if (_normalCount >= NormalCapacity) return false;
				_events[_normalCount++] = event;
			}
			return true;
		}
		std::size_t NormalCount() const noexcept { return _normalCount; }
		std::size_t Count() const noexcept { return _normalCount + _panicCount; }
		// Expose panic first without moving/copying any buffered normal events.
		const Event& At(std::size_t index) const noexcept
		{
			return index < _panicCount ? _events[NormalCapacity + index] : _events[index - _panicCount];
		}

		Event& At(std::size_t index) noexcept
		{
			return index < _panicCount ? _events[NormalCapacity + index] : _events[index - _panicCount];
		}
	private:
		std::array<Event, NormalCapacity + PanicCapacity> _events{};
		std::size_t _normalCount = 0;
		std::size_t _panicCount = 0;
	};
}
