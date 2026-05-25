#pragma once

#include <cstdint>
#include <string>

namespace io
{
	struct SerialTriggerEvent
	{
		const std::string* Device = nullptr;
		unsigned int ButtonIndex = 0u;
		bool IsPressed = false;
	};

	class SerialTriggerProtocol
	{
	public:
		static constexpr std::uint8_t PacketHeader = 0xA5u;

		bool PushByte(std::uint8_t byte, SerialTriggerEvent& out) noexcept
		{
			switch (_state)
			{
			case State::WaitingForHeader:
				if (byte == PacketHeader)
					_state = State::WaitingForIndex;
				return false;
			case State::WaitingForIndex:
				if (byte == PacketHeader)
				{
					_state = State::WaitingForIndex;
					return false;
				}

				_buttonIndex = byte;
				_state = State::WaitingForPressed;
				return false;
			case State::WaitingForPressed:
				if (byte == PacketHeader)
				{
					_state = State::WaitingForIndex;
					return false;
				}

				out.ButtonIndex = _buttonIndex;
				out.IsPressed = byte != 0u;
				Reset();
				return true;
			}

			return false;
		}

		void Reset() noexcept
		{
			_state = State::WaitingForHeader;
			_buttonIndex = 0u;
		}

	private:
		enum class State
		{
			WaitingForHeader,
			WaitingForIndex,
			WaitingForPressed
		};

		State _state = State::WaitingForHeader;
		unsigned int _buttonIndex = 0u;
	};
}