#pragma once

namespace io
{
	class WinsockSession
	{
	public:
		WinsockSession();
		~WinsockSession();

		bool IsStarted() const noexcept;

	private:
		bool _started = false;
	};
}