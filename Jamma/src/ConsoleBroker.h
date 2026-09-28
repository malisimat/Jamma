#pragma once

#include <atomic>
#include <Windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace console
{
	class CommandMailbox;
	class OutboundMailbox;
	class UniqueHandle;
	// App-owned, non-real-time preview broker. The worker owns all pipe I/O;
	// Start/Stop/Reopen are called only by the app owner thread.
	class ConsoleBroker
	{
	public:
		ConsoleBroker() = default;
		~ConsoleBroker();
		ConsoleBroker(const ConsoleBroker&) = delete;
		ConsoleBroker& operator=(const ConsoleBroker&) = delete;
		bool Start(const std::wstring& companionPath,
			std::shared_ptr<CommandMailbox> commands, std::string initialStatus);
		bool Stop() noexcept;
		bool Reopen(const std::wstring& companionPath,
			std::shared_ptr<CommandMailbox> commands, std::string initialStatus);
		bool Connected() const noexcept;
		bool ConsumeFallbackNotice() noexcept;
		std::shared_ptr<OutboundMailbox> Events() const noexcept;

	private:
		struct State;
		static std::vector<std::uint8_t> CurrentUserSid();
		static bool SameUserClient(HANDLE pipe, const std::vector<std::uint8_t>& userSid,
			HANDLE stop);
		static std::string RandomHex();
		static std::wstring WidenAscii(const std::string& value);
		static UniqueHandle CreateServer(const std::wstring& name,
			const std::vector<std::uint8_t>& userSid);
		static bool ConnectClient(HANDLE pipe, HANDLE stop, DWORD timeoutMs);
		static std::wstring FindWindowsTerminal();
		static UniqueHandle Launch(const std::wstring& application,
			const std::wstring& arguments, bool newConsole);
		static bool RunAttempt(const std::shared_ptr<State>& state, bool terminal,
			const std::wstring& terminalPath, const std::vector<std::uint8_t>& userSid);
		static void Run(const std::shared_ptr<State>& state);
		std::shared_ptr<State> _state;
		std::thread _worker;
	};
}
