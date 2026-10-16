#pragma once
#include <array>
#include <cstddef>
namespace console
{
	// App-owner thread only. Observe native messages before dispatch, then filter
	// the global transitions: both paths must agree on where a hold originated.
	class GlobalKeyGate
	{
	public:
		bool Observe(unsigned key, bool down, bool consoleMessage, bool mainMessage, bool repeat) noexcept
		{
			const auto index = Index(key);
			if (index == 3) return true;
			if (consoleMessage)
			{
				if (!down) _consolePress[index] = false;
				else if (!repeat) _consolePress[index] = true;
			}
			else if (mainMessage)
			{
				if (down)
				{
					if (repeat) return !_consolePress[index];
					_consolePress[index] = false;
					_forwarded[index] = true;
				}
				else
				{
					const bool release = !_consolePress[index] || _forwarded[index];
					_consolePress[index] = false;
					_forwarded[index] = false;
					return release;
				}
			}
			return true;
		}
		bool Forward(unsigned key, bool down, bool consoleForeground) noexcept
		{
			const auto index = Index(key);
			if (index == 3) return !consoleForeground;
			if (down)
			{
				if (consoleForeground) _consolePress[index] = true;
				if (_consolePress[index]) return false;
				_forwarded[index] = true;
				return true;
			}
			const bool release = _forwarded[index];
			_forwarded[index] = false;
			_consolePress[index] = false;
			return release;
		}
	private:
		static std::size_t Index(unsigned key) noexcept
		{ return key == 0x2d ? 0 : key == 0x21 ? 1 : key == 0x22 ? 2 : 3; }
		std::array<bool, 3> _forwarded{}, _consolePress{};
	};
}
