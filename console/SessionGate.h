#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace console
{
	// Broker-owner state only. A launch generation has exactly one authenticated
	// client and strictly increasing command IDs. A reconnect gets a new token.
	class SessionGate
	{
	public:
		void Begin(std::string token)
		{
			_token = std::move(token);
			_authenticated = false;
			_claimed = false;
			_accepting = true;
			_lastRequestId = 0;
		}
		bool AcceptHello(std::string_view token, bool sameUser) noexcept
		{
			if (_claimed || !_accepting || !sameUser || token != _token || _token.empty())
				return false;
			_claimed = true;
			_authenticated = true;
			return true;
		}
		bool AcceptRequest(std::uint64_t id, bool stopping = false) noexcept
		{
			if (stopping) StopAccepting();
			if (!_authenticated || !_accepting || id == 0 || id <= _lastRequestId)
				return false;
			_lastRequestId = id;
			return true;
		}
		void Disconnect() noexcept { _authenticated = false; }
		void StopAccepting() noexcept { _accepting = false; }
		bool Authenticated() const noexcept { return _authenticated; }

	private:
		std::string _token;
		bool _authenticated = false;
		bool _claimed = false;
		bool _accepting = false;
		std::uint64_t _lastRequestId = 0;
	};

	// Process/pipe operations are injected so their ordering can be tested
	// without creating a console or named pipe.
	template<class SendShutdown, class WaitForChild, class TerminateChild>
	void FinishSession(SessionGate& gate, bool stopping, bool directChild,
		SendShutdown&& sendShutdown, WaitForChild&& waitForChild,
		TerminateChild&& terminateChild)
	{
		gate.StopAccepting();
		if (stopping) sendShutdown();
		gate.Disconnect();
		if (directChild && !waitForChild()) terminateChild();
	}
}
