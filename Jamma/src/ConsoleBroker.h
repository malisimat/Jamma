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
	// App-owned, non-real-time console broker. The worker owns all pipe I/O;
	// Start/Stop/ToggleVisibility are called only by the app owner thread.
	class ConsoleBroker
	{
	public:
		explicit ConsoleBroker(bool forceConsoleHost = false)
			: _forceConsoleHost(forceConsoleHost) {}
		~ConsoleBroker();
		ConsoleBroker(const ConsoleBroker&) = delete;
		ConsoleBroker& operator=(const ConsoleBroker&) = delete;
		bool Start(const std::wstring& companionPath,
			std::shared_ptr<CommandMailbox> commands, std::string initialStatus,
			bool activateOnLaunch = true);
		bool Stop() noexcept;
		bool Reopen(const std::wstring& companionPath,
			std::shared_ptr<CommandMailbox> commands, std::string initialStatus);
		bool ToggleVisibility(const std::wstring& companionPath,
			std::shared_ptr<CommandMailbox> commands, std::string initialStatus);
		void UpdateVisibility(HWND appWindow) noexcept;
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
			const std::wstring& arguments, bool newConsole, bool activateOnLaunch);
		static bool RunAttempt(const std::shared_ptr<State>& state, bool terminal,
			const std::wstring& terminalPath, const std::vector<std::uint8_t>& userSid);
		static void Run(const std::shared_ptr<State>& state);
		static HWND FindWindowByTitle(const std::wstring& name) noexcept;
		HWND FindWindow() const noexcept;
		std::shared_ptr<State> _state;
		std::thread _worker;
		HWND _window = nullptr; // App-owner thread only; validated by generation title before use.
		bool _hidden = false;
		const bool _forceConsoleHost;
	};
}
