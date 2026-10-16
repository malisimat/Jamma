#include "ConsoleWindow.h"
#include "../../console/CommandMailbox.h"
#include <CommCtrl.h>
#include <Richedit.h>
#include <dwmapi.h>
#include <algorithm>
#pragma comment(lib, "dwmapi.lib")

namespace console
{
	ConsoleWindow::~ConsoleWindow() { Destroy(); }
	std::wstring ConsoleWindow::Widen(std::string_view text)
	{
		if (text.empty()) return {};
		const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			text.data(), static_cast<int>(text.size()), nullptr, 0);
		if (!length) return {};
		std::wstring result(length, L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
			static_cast<int>(text.size()), result.data(), length);
		return result;
	}
	bool ConsoleWindow::CommandText(std::wstring_view wide, std::string& text)
	{
		if (wide.empty() || wide.size() > CommandMailbox::MaxTextBytes) return false;
		for (const auto ch : wide)
			if (ch < L' ' || (ch >= 0x7f && ch <= 0x9f) || ch == 0x2028 || ch == 0x2029) return false;
		const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		if (!bytes || bytes > CommandMailbox::MaxTextBytes) return false;
		text.resize(bytes);
		return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
			static_cast<int>(wide.size()), text.data(), bytes, nullptr, nullptr) == bytes;
	}
	bool ConsoleWindow::CreateHidden(HINSTANCE instance)
	{
		if (_window) return true;
		_backgroundBrush = CreateSolidBrush(BackgroundColor);
		_inputBrush = CreateSolidBrush(InputColor);
		if (!_backgroundBrush || !_inputBrush) { Destroy(); return false; }
		_richEdit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!_richEdit) { Destroy(); return false; }
		WNDCLASSEXW cls{ sizeof(cls) };
		cls.lpfnWndProc = WindowProc; cls.hInstance = instance;
		cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
		cls.lpszClassName = L"JammaNativeCommandWindow";
		if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		{ Destroy(); return false; }
		_window = CreateWindowExW(0, cls.lpszClassName, L"Jamma Commands - /help or F1 for controls", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, 850, 520, nullptr, nullptr, instance, this);
		if (!_window) { Destroy(); return false; }
		// Optional system decoration: older Windows simply rejects unsupported attributes.
		const BOOL dark = TRUE;
		DwmSetWindowAttribute(_window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
		const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
		DwmSetWindowAttribute(_window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
		DwmSetWindowAttribute(_window, DWMWA_CAPTION_COLOR, &BackgroundColor, sizeof(BackgroundColor));
		DwmSetWindowAttribute(_window, DWMWA_TEXT_COLOR, &TextColor, sizeof(TextColor));
		_transcript = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
			0, 0, 0, 0, _window, reinterpret_cast<HMENU>(1), instance, nullptr);
		_input = CreateWindowExW(0, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
			0, 0, 0, 0, _window, reinterpret_cast<HMENU>(2), instance, nullptr);
		_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
			0, 0, 0, 0, _window, reinterpret_cast<HMENU>(3), instance, nullptr);
		if (!_transcript || !_input || !_status) { Destroy(); return false; }
		if (!SetWindowSubclass(_input, ControlProc, 1, reinterpret_cast<DWORD_PTR>(this))
			|| !SetWindowSubclass(_transcript, ControlProc, 1, reinterpret_cast<DWORD_PTR>(this)))
		{ Destroy(); return false; }
		SendMessageW(_input, EM_SETLIMITTEXT, CommandMailbox::MaxTextBytes, 0);
		SendMessageW(_transcript, EM_EXLIMITTEXT, 0, MaxHistoryUnits + 2 * CommandMailbox::MaxTextBytes);
		SendMessageW(_transcript, EM_SETUNDOLIMIT, 0, 0);
		SendMessageW(_transcript, EM_SETBKGNDCOLOR, 0, BackgroundColor);
		SetFontForDpi(GetDpiForWindow(_window));
		Layout(); UpdateStatus(false); return true;
	}
	void ConsoleWindow::Destroy() noexcept
	{
		if (_window) DestroyWindow(_window);
		_window = _transcript = _input = _status = nullptr;
		if (_font) DeleteObject(_font);
		_font = nullptr;
		if (_backgroundBrush) DeleteObject(_backgroundBrush);
		if (_inputBrush) DeleteObject(_inputBrush);
		_backgroundBrush = _inputBrush = nullptr;
		if (_richEdit) FreeLibrary(_richEdit);
		_richEdit = nullptr;
		_entries.clear(); _units = 0; _shownStatus.clear();
	}
	void ConsoleWindow::ShowPassive(HWND mainWindow) noexcept
	{
		// Insert directly behind Jamma, preserving the order of other applications
		// (including any foreground app above Jamma) without taking focus.
		if (_window) SetWindowPos(_window, mainWindow, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW
			| (IsWindow(mainWindow) ? 0 : SWP_NOZORDER));
	}
	void ConsoleWindow::Toggle() noexcept
	{
		if (!_window) return;
		if (IsWindowVisible(_window)) ShowWindow(_window, SW_HIDE);
		else
		{
			ShowWindow(_window, SW_RESTORE); SetForegroundWindow(_window); SetFocus(_input);
		}
	}
	bool ConsoleWindow::Foreground() const noexcept
	{ return _window && GetForegroundWindow() == _window; }
	bool ConsoleWindow::Contains(HWND target) const noexcept
	{ return _window && (target == _window || IsChild(_window, target)); }
	void ConsoleWindow::SetFontForDpi(UINT dpi)
	{
		_dpi = dpi ? dpi : 96;
		const auto font = CreateFontW(-MulDiv(11, _dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
			FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			FIXED_PITCH | FF_MODERN, L"Consolas");
		if (!font) return;
		for (const auto child : { _transcript, _input, _status })
			if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		if (_font) DeleteObject(_font);
		_font = font;
	}
	void ConsoleWindow::Layout()
	{
		if (!_transcript || !_input || !_status) return;
		RECT rect{}; GetClientRect(_window, &rect);
		const int margin = MulDiv(6, _dpi, 96), row = MulDiv(28, _dpi, 96);
		const int width = (std::max)(0L, rect.right - 2 * margin);
		const int history = (std::max)(0L, rect.bottom - 2 * row - 4 * margin);
		MoveWindow(_transcript, margin, margin, width, history, TRUE);
		MoveWindow(_input, margin, history + 2 * margin, width, row, TRUE);
		MoveWindow(_status, margin, history + row + 3 * margin, width, row, TRUE);
	}
	void ConsoleWindow::Status(std::wstring message)
	{
		_last = std::move(message);
		if (_last.size() > 200) _last.resize(200);
		if (!_last.empty() && _last.back() >= 0xd800 && _last.back() <= 0xdbff) _last.pop_back();
		UpdateStatus(_connected);
	}
	void ConsoleWindow::UpdateStatus(bool connected)
	{
		_connected = connected;
		const std::wstring status = (connected ? L"NINJAM connected | " : L"NINJAM disconnected | ") + _last;
		if (_status && status != _shownStatus)
		{ SetWindowTextW(_status, status.c_str()); _shownStatus = status; }
	}
	void ConsoleWindow::Submit()
	{
		const int length = GetWindowTextLengthW(_input);
		if (length <= 0) return;
		if (length > CommandMailbox::MaxTextBytes)
		{ Status(L"Command too long (maximum 4096 UTF-8 bytes)"); return; }
		std::wstring draft(length + 1, L'\0');
		draft.resize(GetWindowTextW(_input, draft.data(), static_cast<int>(draft.size())));
		std::string text;
		if (!CommandText(draft, text))
		{ Status(L"Use a single line of valid text, at most 4096 UTF-8 bytes"); return; }
		if (!_commands.Submit(std::move(text)))
		{ Status(L"Command queue full or shutting down; draft retained"); return; }
		SetWindowTextW(_input, L""); Status(L"Command queued");
	}
	void ConsoleWindow::Paste()
	{
		if (!OpenClipboard(_window)) return;
		const auto handle = GetClipboardData(CF_UNICODETEXT);
		const auto bytes = handle ? GlobalSize(handle) : 0;
		const auto data = bytes >= sizeof(wchar_t) ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
		std::wstring pasted;
		if (data)
		{
			const auto bound = (std::min)(bytes / sizeof(wchar_t), CommandMailbox::MaxTextBytes + 1);
			const auto end = std::find(data, data + bound, L'\0');
			if (end != data + bound) pasted.assign(data, end);
			GlobalUnlock(handle);
		}
		CloseClipboard();
		ApplyPaste(pasted);
	}
	void ConsoleWindow::ApplyPaste(std::wstring_view pasted)
	{
		std::string encoded;
		if (!CommandText(pasted, encoded))
		{ Status(L"Paste rejected: use one line, at most 4096 UTF-8 bytes"); return; }
		const int length = GetWindowTextLengthW(_input);
		if (length > CommandMailbox::MaxTextBytes) return;
		std::wstring draft(length + 1, L'\0');
		draft.resize(GetWindowTextW(_input, draft.data(), static_cast<int>(draft.size())));
		DWORD start = 0, end = 0;
		SendMessageW(_input, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
		draft.replace(start, end - start, pasted);
		if (!CommandText(draft, encoded))
		{ Status(L"Paste would exceed 4096 UTF-8 bytes; draft retained"); return; }
		SendMessageW(_input, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(std::wstring(pasted).c_str()));
	}
	void ConsoleWindow::Append(std::span<const std::string> lines)
	{
		if (!_transcript || lines.empty()) return;
		CHARRANGE selection{};
		SendMessageW(_transcript, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
		const LONG firstLine = static_cast<LONG>(SendMessageW(_transcript, EM_GETFIRSTVISIBLELINE, 0, 0));
		const LONG anchor = static_cast<LONG>(SendMessageW(_transcript, EM_LINEINDEX, firstLine, 0));
		SCROLLINFO scroll{ sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS };
		GetScrollInfo(_transcript, SB_VERT, &scroll);
		const bool follow = selection.cpMin == selection.cpMax
			&& scroll.nPos + static_cast<int>(scroll.nPage) >= scroll.nMax;
		SendMessageW(_transcript, WM_SETREDRAW, FALSE, 0);
		LONG removed = 0;
		for (const auto& line : lines.first((std::min)(lines.size(), MaxAppendEntries)))
		{
			if (line.size() > 2 * CommandMailbox::MaxTextBytes) continue;
			auto text = Widen(line);
			if (!line.empty() && text.empty()) text = L"[CONSOLE] Invalid UTF-8 log entry dropped";
			// Native paragraph indices count one unit. Normalize captured line endings/NUL.
			std::replace(text.begin(), text.end(), L'\r', L' ');
			std::replace(text.begin(), text.end(), L'\n', L' ');
			std::replace(text.begin(), text.end(), L'\0', L' ');
			Status(text); text += L'\r';
			LONG trim = 0;
			while (!_entries.empty() && (_entries.size() >= MaxHistoryEntries
				|| _units + static_cast<LONG>(text.size()) > MaxHistoryUnits))
			{ trim += _entries.front(); _units -= _entries.front(); _entries.pop_front(); }
			if (trim)
			{
				CHARRANGE prefix{ 0, trim };
				SendMessageW(_transcript, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&prefix));
				SendMessageW(_transcript, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
				removed += trim;
			}
			CHARRANGE tail{ _units, _units };
			SendMessageW(_transcript, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tail));
			CHARFORMAT2W format{ sizeof(format) }; format.dwMask = CFM_COLOR;
			format.crTextColor = LineColor(line);
			SendMessageW(_transcript, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
			SendMessageW(_transcript, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
			_entries.push_back(static_cast<LONG>(text.size())); _units += static_cast<LONG>(text.size());
		}
		if (follow)
		{
			CHARRANGE tail{ _units, _units };
			SendMessageW(_transcript, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tail));
			SendMessageW(_transcript, WM_VSCROLL, SB_BOTTOM, 0);
		}
		else
		{
			selection.cpMin = (std::max)(0L, selection.cpMin - removed);
			selection.cpMax = (std::max)(0L, selection.cpMax - removed);
			SendMessageW(_transcript, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
			const LONG adjusted = (std::max)(0L, anchor - removed);
			const auto wantedLine = SendMessageW(_transcript, EM_EXLINEFROMCHAR, 0, adjusted);
			const auto currentLine = SendMessageW(_transcript, EM_GETFIRSTVISIBLELINE, 0, 0);
			SendMessageW(_transcript, EM_LINESCROLL, 0, wantedLine - currentLine);
		}
		SendMessageW(_transcript, WM_SETREDRAW, TRUE, 0);
		InvalidateRect(_transcript, nullptr, FALSE);
	}
	LRESULT CALLBACK ConsoleWindow::ControlProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR, DWORD_PTR data)
	{
		auto& self = *reinterpret_cast<ConsoleWindow*>(data);
		if (message == WM_KEYDOWN && wParam == VK_TAB)
		{ SetFocus(window == self._input ? self._transcript : self._input); return 0; }
		if (message == WM_KEYDOWN && wParam == VK_F1)
		{
			if (!self._commands.Submit("/help")) self.Status(L"Help queue full");
			return 0;
		}
		if (message == WM_CHAR && (wParam == L'\t' || wParam == L'\n')) return 0;
		if (window == self._input)
		{
			if (message == WM_CHAR && wParam == L'\r') { self.Submit(); return 0; }
			if (message == WM_PASTE) { self.Paste(); return 0; }
		}
		return DefSubclassProc(window, message, wParam, lParam);
	}
	LRESULT CALLBACK ConsoleWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		auto* self = reinterpret_cast<ConsoleWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
		if (message == WM_NCCREATE)
		{
			self = static_cast<ConsoleWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
			SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		}
		if (self)
		{
			switch (message)
			{
			case WM_ERASEBKGND:
			{
				RECT rect{}; GetClientRect(window, &rect);
				FillRect(reinterpret_cast<HDC>(wParam), &rect, self->_backgroundBrush);
				return 1;
			}
			case WM_CTLCOLOREDIT:
			case WM_CTLCOLORSTATIC:
			{
				const auto dc = reinterpret_cast<HDC>(wParam);
				const bool input = reinterpret_cast<HWND>(lParam) == self->_input;
				SetTextColor(dc, TextColor);
				SetBkColor(dc, input ? InputColor : BackgroundColor);
				return reinterpret_cast<LRESULT>(input ? self->_inputBrush : self->_backgroundBrush);
			}
			case WM_CLOSE: ShowWindow(window, SW_HIDE); return 0;
			case WM_SETFOCUS: if (self->_input) SetFocus(self->_input); return 0;
			case WM_SIZE: self->Layout(); return 0;
			case WM_DPICHANGED:
			{
				self->SetFontForDpi(HIWORD(wParam));
				const auto& rect = *reinterpret_cast<RECT*>(lParam);
				SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left,
					rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
				self->Layout(); return 0;
			}
			case WM_NCDESTROY: SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
			}
		}
		return DefWindowProcW(window, message, wParam, lParam);
	}
	COLORREF ConsoleWindow::LineColor(std::string_view line) noexcept
	{
		const auto has = [line](std::string_view word) { return line.find(word) != std::string_view::npos; };
		if (has("error") || has("Error") || has("failed") || has("Failed") || has("invalid")
			|| has("Invalid") || has("Could not") || has("cannot") || has("quitting") || has("unreadable"))
			return RGB(255, 92, 96);
		if (has("warning") || has("Warning") || has("WARN") || has("fallback") || has("unavailable")
			|| has("not connected") || has("Not connected") || has("Unknown command") || has("recovery needed"))
			return RGB(255, 166, 74);
		if (line.starts_with("[NINJAM]"))
		{
			if (has("<you>")) return RGB(128, 255, 128);
			if (has("(private)")) return RGB(255, 128, 255);
			if (has("<")) return RGB(128, 255, 255);
			if (line.substr(8).starts_with(" Commands:") || line.substr(8).starts_with(" Servers:")
				|| line.substr(8).starts_with("   ")) return RGB(255, 255, 255);
			return RGB(123, 213, 224);
		}
		if ((line.starts_with("[ASIO] ") && !line.starts_with("[ASIO] Attempt"))
			|| (line.starts_with("[MIDI] ") && !line.starts_with("[MIDI] Request"))) return RGB(255, 255, 255);
		if (line.starts_with("[Vst") || line.starts_with("[Automation]") || line.starts_with("[Input]")
			|| line.starts_with("[Serial]") || line.starts_with("[tap]")) return RGB(145, 151, 160);
		if (line.starts_with("[ASIO]")) return RGB(245, 208, 99);
		if (line.starts_with("[MIDI")) return RGB(210, 217, 98);
		if (line.starts_with("[JAM]") || line.starts_with("[LOAD]")) return RGB(245, 195, 102);
		if (line.starts_with("[RIG]")) return RGB(228, 211, 113);
		if (line.starts_with("[Station]") || line.starts_with("[Loop")) return RGB(247, 190, 125);
		if (line.starts_with("[WINDOW]")) return RGB(229, 199, 133);
		if (line.starts_with("[CONSOLE]")) return RGB(244, 215, 115);
		if (line.starts_with("[BOOT]")) return RGB(255, 222, 110);
		return RGB(236, 202, 112);
	}
}
