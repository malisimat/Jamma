#pragma once
#include <Windows.h>
#include <deque>
#include <span>
#include <string>
#include <string_view>
namespace console
{
	class CommandMailbox;
	struct ConsoleWindowTestAccess;
	// UI-thread RAII owner. Producers hand off through LineRing; no Scene/audio access.
	class ConsoleWindow
	{
	public:
		explicit ConsoleWindow(CommandMailbox& commands) : _commands(commands) {}
		~ConsoleWindow();
		ConsoleWindow(const ConsoleWindow&) = delete;
		ConsoleWindow& operator=(const ConsoleWindow&) = delete;
		bool CreateHidden(HINSTANCE instance);
		void Destroy() noexcept;
		void ShowPassive() noexcept;
		void Toggle() noexcept;
		bool Created() const noexcept { return _window != nullptr; }
		bool Foreground() const noexcept;
		bool Contains(HWND target) const noexcept;
		void Append(std::span<const std::string> lines);
		void UpdateStatus(bool connected);
		static constexpr LONG MaxHistoryUnits = 256 * 1024;
		static constexpr std::size_t MaxHistoryEntries = 2048;
		static constexpr std::size_t MaxAppendEntries = 32;
	private:
		friend struct ConsoleWindowTestAccess;
		static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
		static LRESULT CALLBACK ControlProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
		static std::wstring Widen(std::string_view text);
		static bool CommandText(std::wstring_view wide, std::string& text);
		static COLORREF LineColor(std::string_view line) noexcept;
		void Layout();
		void SetFontForDpi(UINT dpi);
		void Submit();
		void Paste();
		void ApplyPaste(std::wstring_view pasted);
		void Status(std::wstring message);
		CommandMailbox& _commands;
		HMODULE _richEdit = nullptr;
		HWND _window = nullptr, _transcript = nullptr, _input = nullptr, _status = nullptr;
		HFONT _font = nullptr;
		UINT _dpi = 96;
		std::deque<LONG> _entries;
		LONG _units = 0;
		bool _connected = false;
		std::wstring _last = L"Ready - F1 for help", _shownStatus;
	};
}


