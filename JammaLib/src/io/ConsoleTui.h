#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>

namespace io
{
	// A small Win32 + ANSI/VT console TUI:
	//   - Logs scroll above as normal
	//   - A single input line is pinned to the bottom row of the console
	//   - std::cout / std::cerr are redirected through a custom streambuf so
	//     any log produced anywhere in the process is rendered above the
	//     input line without disturbing what the user is typing.
	//
	// Lifetime: Start() once, Stop() restores std::cout/std::cerr, the input
	// console mode and the output codepage. Stop() is idempotent and safe to
	// call from the destructor.
	//
	// Designed for Windows console hosts that support virtual terminal
	// sequences (Windows 10+ conhost, Windows Terminal). If the console does
	// not have an interactive stdin/stdout (redirected, GUI app with no
	// console), Start() prints a one-line notice and falls back to plain
	// logging without an input line.
	class ConsoleTui
	{
	public:
		using SubmitHandler = std::function<void(const std::string&)>;

		ConsoleTui();
		~ConsoleTui();

		ConsoleTui(const ConsoleTui&) = delete;
		ConsoleTui& operator=(const ConsoleTui&) = delete;

		// Starts the TUI. `prompt` is shown at the start of the input line
		// (e.g. "> "). `onSubmit` is invoked on the input thread with the
		// entered text whenever the user presses Enter. The submit callback
		// is invoked outside any internal lock, so it is safe to log from it.
		void Start(std::string prompt, SubmitHandler onSubmit);

		// Stops the TUI, joins the input thread and restores console state.
		// Idempotent.
		void Stop();

	private:
		class TuiBuf;

		void _InputLoop();
		void _RedrawInputLocked();                          // requires _renderMutex
		void _WriteRawLocked(const char* data, std::size_t n);
		void _OnLogChar(char c);                            // streambuf hook

	private:
		std::mutex _renderMutex;
		std::string _prompt;
		std::string _input;
		SubmitHandler _onSubmit;

		std::unique_ptr<TuiBuf> _coutBuf;
		std::unique_ptr<TuiBuf> _cerrBuf;
		std::streambuf* _origCout = nullptr;
		std::streambuf* _origCerr = nullptr;

		// Win32 handles + saved console state. Stored as void*/unsigned long
		// so this header doesn't need to drag in <windows.h>.
		void* _hStdout = nullptr;
		void* _hStdin = nullptr;
		unsigned long _origOutMode = 0;
		unsigned long _origInMode = 0;
		unsigned int _origOutCP = 0;
		bool _outModeSaved = false;
		bool _inModeSaved = false;

		std::thread _inputThread;
		std::atomic_bool _stop{ false };
		std::atomic_bool _started{ false };
	};
}
