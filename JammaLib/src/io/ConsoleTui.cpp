#include "ConsoleTui.h"

#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <iostream>
#include <streambuf>

using namespace io;

namespace
{
	// ANSI/VT escape sequences. ENABLE_VIRTUAL_TERMINAL_PROCESSING is set on
	// stdout in Start(), so these are interpreted by the host terminal.
	constexpr const char* kReset      = "\x1b[0m";
	constexpr const char* kFgGray     = "\x1b[90m";
	constexpr const char* kFgGreen    = "\x1b[32m";
	constexpr const char* kFgYellow   = "\x1b[33m";
	constexpr const char* kFgMagenta  = "\x1b[35m";
	constexpr const char* kFgCyan     = "\x1b[36m";
	constexpr const char* kFgBrGreen  = "\x1b[92m";
	constexpr const char* kFgBrCyan   = "\x1b[96m";
	constexpr const char* kPromptStyle = "\x1b[1;36m";

	// Returns a styled, emoji-prefixed version of `line` for known [NINJAM]
	// log shapes. Lines without the [NINJAM] tag are returned unchanged so
	// non-ninjam logs continue to look exactly as they did before.
	std::string FormatLine(const std::string& line)
	{
		constexpr const char kTag[] = "[NINJAM]";
		constexpr std::size_t kTagLen = sizeof(kTag) - 1;
		if (line.compare(0, kTagLen, kTag) != 0)
			return line;

		std::size_t pos = kTagLen;
		while (pos < line.size() && line[pos] == ' ')
			++pos;
		const std::string rest = line.substr(pos);

		auto starts = [&](const char* p) {
			const std::size_t n = std::strlen(p);
			return rest.size() >= n && rest.compare(0, n, p) == 0;
		};

		const char* emoji = u8"\xE2\x80\xA2 "; // bullet
		const char* color = kFgGray;

		if (starts("<you>"))                      { emoji = u8"\xF0\x9F\x93\xA4 "; color = kFgBrGreen; }   // outbox
		else if (starts("<"))                     { emoji = u8"\xF0\x9F\x92\xAC "; color = kFgBrCyan;  }   // speech balloon
		else if (starts("(private)"))             { emoji = u8"\xF0\x9F\x94\x92 "; color = kFgMagenta; }   // lock
		else if (starts("Topic"))                 { emoji = u8"\xF0\x9F\x93\x8C "; color = kFgCyan;    }   // pushpin
		else if (starts("-->"))                   { emoji = u8"\xF0\x9F\x9F\xA2 "; color = kFgGreen;   }   // green circle
		else if (starts("<--"))                   { emoji = u8"\xF0\x9F\x94\xB4 "; color = kFgYellow;  }   // red circle
		else if (starts("**"))                    { emoji = u8"\xE2\x9C\xA8 ";     color = kFgMagenta; }   // sparkles
		else if (starts("Auto-connect"))          { emoji = u8"\xF0\x9F\x94\x8C "; color = kFgCyan;    }   // plug
		else if (starts("Type a message"))        { emoji = u8"\xE2\x8C\xA8 ";     color = kFgGray;    }   // keyboard
		else if (starts("Not connected"))         { emoji = u8"\xE2\x9A\xA0 ";     color = kFgYellow;  }   // warn
		else if (starts("Chat input"))            { emoji = u8"\xE2\x9A\xA0 ";     color = kFgYellow;  }
		else if (starts("Console TUI disabled"))  { emoji = u8"\xE2\x9A\xA0 ";     color = kFgYellow;  }

		std::string out;
		out.reserve(line.size() + 32);
		out += kFgGray;
		out += "[NINJAM] ";
		out += kReset;
		out += emoji;
		out += color;
		out += rest;
		out += kReset;
		return out;
	}
}

class ConsoleTui::TuiBuf : public std::streambuf
{
public:
	explicit TuiBuf(ConsoleTui* tui) : _tui(tui) {}

protected:
	int_type overflow(int_type ch) override
	{
		if (ch == traits_type::eof())
			return traits_type::not_eof(ch);
		_tui->_OnLogChar(static_cast<char>(ch));
		return ch;
	}

	std::streamsize xsputn(const char* s, std::streamsize n) override
	{
		for (std::streamsize i = 0; i < n; ++i)
			_tui->_OnLogChar(s[i]);
		return n;
	}

	int sync() override { return 0; }

private:
	ConsoleTui* _tui;
};

ConsoleTui::ConsoleTui() = default;

ConsoleTui::~ConsoleTui()
{
	Stop();
}

void ConsoleTui::Start(std::string prompt, SubmitHandler onSubmit)
{
	if (_started.exchange(true))
		return;

	_prompt = std::move(prompt);
	_onSubmit = std::move(onSubmit);
	_stop = false;
	_input.clear();

	_hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
	_hStdin  = GetStdHandle(STD_INPUT_HANDLE);

	bool ttyOut = false;
	if (_hStdout && _hStdout != INVALID_HANDLE_VALUE)
	{
		DWORD mode = 0;
		if (GetConsoleMode(_hStdout, &mode))
		{
			_origOutMode = mode;
			_outModeSaved = true;
			SetConsoleMode(_hStdout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
			_origOutCP = GetConsoleOutputCP();
			SetConsoleOutputCP(CP_UTF8);
			ttyOut = true;
		}
	}

	bool ttyIn = false;
	if (_hStdin && _hStdin != INVALID_HANDLE_VALUE)
	{
		DWORD mode = 0;
		if (GetConsoleMode(_hStdin, &mode))
		{
			_origInMode = mode;
			_inModeSaved = true;
			// Raw input: no line buffering, no auto-echo, no Ctrl-handling
			// stealing keys, no quick-edit eating clicks. Keep window-size
			// events so we can redraw on resize.
			DWORD newMode = (mode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS)
				& ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
					| ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT);
			SetConsoleMode(_hStdin, newMode);
			ttyIn = true;
		}
	}

	if (ttyOut && ttyIn)
	{
		_coutBuf = std::make_unique<TuiBuf>(this);
		_cerrBuf = std::make_unique<TuiBuf>(this);
		_origCout = std::cout.rdbuf(_coutBuf.get());
		_origCerr = std::cerr.rdbuf(_cerrBuf.get());

		{
			std::lock_guard<std::mutex> lk(_renderMutex);
			_RedrawInputLocked();
		}

		_inputThread = std::thread([this]() { _InputLoop(); });
	}
	else
	{
		// No interactive console: leave std::cout alone and surface a notice.
		std::cout << "[NINJAM] Console TUI disabled - no interactive console" << std::endl;
	}
}

void ConsoleTui::Stop()
{
	if (!_started.exchange(false))
		return;

	_stop = true;
	if (_inputThread.joinable())
		_inputThread.join();

	// Restore stdout/stderr first so any subsequent logs go straight to the
	// underlying stream without hitting the (about-to-be-destroyed) buffers.
	if (_origCout)
	{
		std::cout.rdbuf(_origCout);
		_origCout = nullptr;
	}
	if (_origCerr)
	{
		std::cerr.rdbuf(_origCerr);
		_origCerr = nullptr;
	}
	_coutBuf.reset();
	_cerrBuf.reset();

	// Move past the input artifact and reset SGR.
	if (_hStdout && _hStdout != INVALID_HANDLE_VALUE)
	{
		const char tail[] = "\r\x1b[2K\x1b[0m";
		DWORD written = 0;
		WriteFile(_hStdout, tail, static_cast<DWORD>(sizeof(tail) - 1), &written, nullptr);

		if (_outModeSaved)
		{
			SetConsoleMode(_hStdout, _origOutMode);
			_outModeSaved = false;
		}
		if (_origOutCP != 0)
		{
			SetConsoleOutputCP(_origOutCP);
			_origOutCP = 0;
		}
	}

	if (_hStdin && _hStdin != INVALID_HANDLE_VALUE && _inModeSaved)
	{
		SetConsoleMode(_hStdin, _origInMode);
		_inModeSaved = false;
	}

	_hStdout = nullptr;
	_hStdin = nullptr;
	_input.clear();
}

void ConsoleTui::_WriteRawLocked(const char* data, std::size_t n)
{
	if (!_hStdout || _hStdout == INVALID_HANDLE_VALUE || n == 0)
		return;
	DWORD written = 0;
	WriteFile(_hStdout, data, static_cast<DWORD>(n), &written, nullptr);
}

void ConsoleTui::_RedrawInputLocked()
{
	std::string out;
	out.reserve(_prompt.size() + _input.size() + 16);
	out += "\r\x1b[2K";
	out += kPromptStyle;
	out += _prompt;
	out += kReset;
	out += _input;
	_WriteRawLocked(out.data(), out.size());
}

void ConsoleTui::_OnLogChar(char c)
{
	// Per-thread line accumulator: every thread that writes to std::cout /
	// std::cerr has its own pending line, so concurrent producers cannot
	// interleave partial bytes into each other's lines.
	thread_local std::string pending;

	if (c == '\r')
		return;

	if (c != '\n')
	{
		pending.push_back(c);
		return;
	}

	std::string formatted = FormatLine(pending);
	pending.clear();

	std::lock_guard<std::mutex> lk(_renderMutex);

	// Sequence:
	//   \r \x1b[2K   wipe whatever is on the current (input) line
	//   <formatted>  styled log line at column 0
	//   \r\n         move to next line, scrolling region if at bottom
	//   redraw       reprint pinned input on the new bottom row
	std::string out;
	out.reserve(formatted.size() + 8);
	out += "\r\x1b[2K";
	out += formatted;
	out += "\r\n";
	_WriteRawLocked(out.data(), out.size());
	_RedrawInputLocked();
}

void ConsoleTui::_InputLoop()
{
	if (!_hStdin || _hStdin == INVALID_HANDLE_VALUE)
		return;

	while (!_stop.load())
	{
		const DWORD waitResult = WaitForSingleObject(_hStdin, 100 /*ms*/);
		if (waitResult == WAIT_TIMEOUT)
			continue;
		if (waitResult != WAIT_OBJECT_0)
			break;
		if (_stop.load())
			break;

		INPUT_RECORD records[64];
		DWORD numRead = 0;
		if (!ReadConsoleInputW(_hStdin, records, _countof(records), &numRead))
			break;

		std::string submitText;
		bool haveSubmit = false;

		{
			std::lock_guard<std::mutex> lk(_renderMutex);
			bool dirty = false;

			for (DWORD i = 0; i < numRead; ++i)
			{
				if (_stop.load())
					break;

				const auto& rec = records[i];

				if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT)
				{
					dirty = true;
					continue;
				}
				if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
					continue;

				const WCHAR wch = rec.Event.KeyEvent.uChar.UnicodeChar;
				if (wch == 0)
					continue;

				if (wch == L'\r' || wch == L'\n')
				{
					if (!_input.empty() && !haveSubmit)
					{
						submitText = _input;
						haveSubmit = true;
					}
					_input.clear();
					dirty = true;
				}
				else if (wch == L'\b' || wch == 127)
				{
					if (!_input.empty())
					{
						// Pop one UTF-8 code point: continuation bytes match
						// 10xxxxxx (top two bits 10), then drop the lead byte.
						while (!_input.empty()
							&& ((static_cast<unsigned char>(_input.back()) & 0xC0) == 0x80))
						{
							_input.pop_back();
						}
						if (!_input.empty())
							_input.pop_back();
						dirty = true;
					}
				}
				else if (wch == 27 /*ESC*/)
				{
					if (!_input.empty())
					{
						_input.clear();
						dirty = true;
					}
				}
				else if (wch >= 0x20)
				{
					char utf8[8];
					const int n = WideCharToMultiByte(CP_UTF8, 0, &wch, 1,
						utf8, sizeof(utf8), nullptr, nullptr);
					if (n > 0)
					{
						_input.append(utf8, static_cast<std::size_t>(n));
						dirty = true;
					}
				}
			}

			if (dirty)
				_RedrawInputLocked();
		}

		// Invoke the submit callback OUTSIDE the render lock so it can safely
		// log back through std::cout (which would otherwise re-enter
		// _OnLogChar and try to take _renderMutex again).
		if (haveSubmit && _onSubmit)
			_onSubmit(submitText);
	}
}
