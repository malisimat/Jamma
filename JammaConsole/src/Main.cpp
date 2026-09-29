#include "../../console/WindowsPipe.h"
#include "../../console/PromptEditor.h"
#include "../../console/PasteInput.h"
#include "../../console/Transcript.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

struct ConsoleClientState
{
	static std::optional<std::string> ClipboardText(bool& tooLong)
	{
		tooLong = false;
		if (!OpenClipboard(nullptr)) return std::nullopt;
		std::optional<std::string> result;
		const auto handle = GetClipboardData(CF_UNICODETEXT);
		if (handle)
		{
			const auto* wide = static_cast<const wchar_t*>(GlobalLock(handle));
			if (wide)
			{
				const auto capacity = GlobalSize(handle) / sizeof(wchar_t);
				std::size_t length = 0;
				while (length < capacity && length <= console::MaxInputBytes && wide[length]) ++length;
				if (length > console::MaxInputBytes) tooLong = true;
				else if (length < capacity)
				{
					const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
						wide, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
					if (bytes > static_cast<int>(console::MaxInputBytes)) tooLong = true;
					else if (bytes >= 0)
					{
						std::string text(static_cast<std::size_t>(bytes), '\0');
						if (!bytes || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
							static_cast<int>(length), text.data(), bytes, nullptr, nullptr) == bytes)
							result = std::move(text);
					}
				}
				GlobalUnlock(handle);
			}
		}
		CloseClipboard();
		return result;
	}
	static std::wstring Argument(int argc, wchar_t** argv, const wchar_t* name)
	{
		for (int index = 1; index + 1 < argc; ++index)
			if (std::wstring_view(argv[index]) == name) return argv[index + 1];
		return {};
	}
	std::mutex InboxMutex;
	std::condition_variable InboxReady;
	std::deque<console::Message> Inbox;
	std::optional<console::Message> PendingStatus;
	std::mutex PostMutex;
	std::atomic<bool> NotificationPending{ false };
	std::string Status = "Connecting to Jamma...";
	console::Transcript Transcript;
	console::PromptEditor Prompt;
	std::string PromptError;
	console::PasteInput Paste;
	std::uint64_t NextRequestId = 1;
	std::set<std::uint64_t> PendingRequests;
	std::mutex OutboxMutex;
	std::condition_variable OutboxReady;
	std::deque<console::Message> Outbox;
	std::size_t Dropped = 0;
};

int wmain(int argc, wchar_t** argv)
{
	const auto pipeName = ConsoleClientState::Argument(argc, argv, L"--pipe");
	const auto wideToken = ConsoleClientState::Argument(argc, argv, L"--token");
	if (pipeName.empty() || wideToken.size() != 32) return 2;
	std::string token;
	token.reserve(wideToken.size());
	for (const auto ch : wideToken)
	{
		if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))) return 2;
		token.push_back(static_cast<char>(ch));
	}
	console::UniqueHandle pipe(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
		0, nullptr, OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr));
	if (!pipe.Valid())
	{
		WaitNamedPipeW(pipeName.c_str(), 2000);
		pipe.Reset(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
			0, nullptr, OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr));
	}
	if (!pipe.Valid()) return 3;
	console::UniqueHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!stop.Valid() || !console::WriteMessage(pipe.Get(), stop.Get(),
		{ console::MessageType::Hello, 0, token })) return 4;

	auto screen = ftxui::ScreenInteractive::Fullscreen();
	screen.TrackMouse();
	screen.ForceHandleCtrlC(false);
	ConsoleClientState state;
	auto drain = [&] {
		std::deque<console::Message> batch;
		std::optional<console::Message> status;
		std::size_t dropped = 0;
		{
			std::scoped_lock lock(state.InboxMutex);
			batch.swap(state.Inbox);
			status.swap(state.PendingStatus);
			dropped = std::exchange(state.Dropped, 0);
		}
		state.InboxReady.notify_all();
		if (status) state.Status = std::move(status->Text);
		for (auto& message : batch)
		{
			if (message.Type == console::MessageType::Event)
			{
				state.Transcript.Append(message.Text);
			}
			else if (message.Type == console::MessageType::CommandResult)
			{
				if (state.PendingRequests.erase(message.RequestId))
					state.Transcript.Append(message.Text);
				else state.Transcript.Append("[CONSOLE] Unexpected command result");
			}
		}
		if (dropped)
		{
			state.Transcript.Append("[CONSOLE] Lost " + std::to_string(dropped) + " events");
		}
	};
	auto view = ftxui::Renderer([&] {
		const int width = std::max(1, screen.dimx());
		const int height = std::max(1, screen.dimy());
		const int visibleHeight = std::max(0, height - 5);
		ftxui::Elements rows;
		if (height >= 3) rows.push_back(ftxui::text("JAMMA"));
		if (height >= 4) rows.push_back(ftxui::separator());
		const auto visible = state.Transcript.Visible(width, visibleHeight);
		for (const auto& row : visible) rows.push_back(ftxui::text(row.Text));
		for (int line = static_cast<int>(visible.size()); line < visibleHeight; ++line)
			rows.push_back(ftxui::text(""));
		if (height >= 5) rows.push_back(ftxui::separator());
		const auto prompt = state.Prompt.Text();
		const auto caret = state.Prompt.Caret();
		const auto label = width >= 3 ? "> " : width == 2 ? ">" : "";
		const int available = width - static_cast<int>(std::string_view(label).size());
		const auto next = console::NextGrapheme(prompt, caret);
		const auto caretText = caret < prompt.size() ? prompt.substr(caret, next - caret) : " ";
		const auto atCaret = ftxui::string_width(caretText) <= available ? caretText : "_";
		const auto begin = console::PromptWindowStart(prompt, caret,
			std::max(0, available - ftxui::string_width(atCaret)));
		const auto prefix = prompt.substr(begin, caret - begin);
		const auto remaining = std::max(0, available - ftxui::string_width(prefix) - ftxui::string_width(atCaret));
		if (height >= 2)
			rows.push_back(ftxui::hbox({ ftxui::text(label), ftxui::text(prefix),
				ftxui::text(atCaret) | ftxui::inverted,
				ftxui::text(console::ClipColumns(std::string_view(prompt).substr(next), remaining)) }));
		rows.push_back(ftxui::text(console::StatusForWidth(
			state.PromptError.empty() ? state.Status : state.PromptError, width)));
		return ftxui::vbox(std::move(rows));
	});
	view = ftxui::CatchEvent(view, [&](ftxui::Event event) {
		if (event == ftxui::Event::Custom)
		{
			state.NotificationPending.store(false, std::memory_order_release);
			drain();
			return true;
		}
		if (!state.Paste.Active() && event.input() == "\x1b[200~")
		{
			state.Paste.Start();
			return true;
		}
		if (state.Paste.Active())
		{
			if (event.input() == "\x1b[201~")
			{
				const auto result = state.Paste.Finish(state.Prompt);
				state.PromptError = result == console::InputResult::Accepted ? ""
					: result == console::InputResult::TooLong ? "Input limit reached"
					: "Paste rejected: multiline or invalid input";
				return true;
			}
			if (event.is_character()) state.Paste.Character(event.character());
			else state.Paste.Control();
			return true;
		}
		if (event == ftxui::Event::CtrlC) return true;
		if (event == ftxui::Event::ArrowUp || event == ftxui::Event::PageUp)
		{
			state.Transcript.ScrollUp(std::max(1, screen.dimx()), event == ftxui::Event::PageUp ? std::max(1, screen.dimy() - 6) : 1);
			return true;
		}
		if (event == ftxui::Event::ArrowDown || event == ftxui::Event::PageDown)
		{
			state.Transcript.ScrollDown(std::max(1, screen.dimx()), event == ftxui::Event::PageDown ? std::max(1, screen.dimy() - 6) : 1);
			return true;
		}
		if (event == ftxui::Event::CtrlF) { state.Transcript.FollowTail(); return true; }
		if (event == ftxui::Event::End) { state.Prompt.MoveEnd(); return true; }
		if (event == ftxui::Event::ArrowLeft) { state.Prompt.MoveLeft(); return true; }
		if (event == ftxui::Event::ArrowRight) { state.Prompt.MoveRight(); return true; }
		if (event == ftxui::Event::ArrowLeftCtrl) { state.Prompt.MoveWordLeft(); return true; }
		if (event == ftxui::Event::ArrowRightCtrl) { state.Prompt.MoveWordRight(); return true; }
		if (event == ftxui::Event::Home) { state.Prompt.MoveHome(); return true; }
		if (event == ftxui::Event::Backspace) { state.Prompt.Backspace(); return true; }
		if (event == ftxui::Event::Delete) { state.Prompt.Delete(); return true; }
		if (event == ftxui::Event::CtrlW) { state.Prompt.DeleteWordLeft(); return true; }
		if (event == ftxui::Event::CtrlV)
		{
			bool tooLong = false;
			const auto pasted = ConsoleClientState::ClipboardText(tooLong);
			const auto result = pasted ? state.Prompt.Insert(*pasted) : console::InputResult::InvalidUtf8;
			state.PromptError = result == console::InputResult::Accepted ? ""
				: tooLong || result == console::InputResult::TooLong ? "Input limit reached"
				: "Clipboard unavailable or contains multiline/invalid text";
			return true;
		}
		if (event == ftxui::Event::Return)
		{
			if (!state.Prompt.Text().empty())
			{
				std::scoped_lock lock(state.OutboxMutex);
				if (state.Outbox.size() < 64 && state.PendingRequests.size() < 64)
				{
					const auto requestId = state.NextRequestId++;
					state.PendingRequests.insert(requestId);
					state.Outbox.push_back({ console::MessageType::CommandRequest,
						requestId, state.Prompt.Take() });
					state.OutboxReady.notify_one();
					state.PromptError.clear();
				}
				else state.PromptError = "Command queue full";
			}
			return true;
		}
		if (event.is_character())
		{
			const auto result = state.Prompt.Insert(event.character());
			state.PromptError = result == console::InputResult::Accepted ? ""
				: result == console::InputResult::TooLong ? "Input limit reached"
				: "Invalid or multiline input";
			return true;
		}
		if (event == ftxui::Event::Escape)
		{
			std::scoped_lock lock(state.PostMutex);
			SetEvent(stop.Get());
			screen.Exit();
			return true;
		}
		if (event.is_mouse())
		{
			const auto& mouse = event.mouse();
			if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Moved)
				return true;
		}
		return false;
	});

	std::thread writer([&] {
		for (;;)
		{
			console::Message request;
			{
				std::unique_lock lock(state.OutboxMutex);
				state.OutboxReady.wait(lock, [&] {
					return !state.Outbox.empty() || WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0;
				});
				if (WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) break;
				request = std::move(state.Outbox.front());
				state.Outbox.pop_front();
			}
			if (!console::WriteMessage(pipe.Get(), stop.Get(), request))
			{
				SetEvent(stop.Get());
				std::scoped_lock lock(state.PostMutex);
				screen.Exit();
				break;
			}
		}
	});
	std::thread reader([&] {
		for (;;)
		{
			const auto received = console::ReadMessage(pipe.Get(), stop.Get());
			if (!received.Value || received.Value->Type == console::MessageType::Shutdown)
				break;
			if (received.Value->Type != console::MessageType::Event
				&& received.Value->Type != console::MessageType::StatusSnapshot
				&& received.Value->Type != console::MessageType::CommandResult) break;
			{
				std::unique_lock lock(state.InboxMutex);
				if (received.Value->Type == console::MessageType::StatusSnapshot)
				{
					state.PendingStatus = std::move(*received.Value);
				}
				else if (received.Value->Type == console::MessageType::CommandResult)
				{
					while (state.Inbox.size() == 256
						&& WaitForSingleObject(stop.Get(), 0) != WAIT_OBJECT_0)
					{
						auto event = std::find_if(state.Inbox.begin(), state.Inbox.end(),
							[](const console::Message& item) {
								return item.Type == console::MessageType::Event;
							});
						if (event != state.Inbox.end())
						{
							state.Inbox.erase(event);
							++state.Dropped;
							break;
						}
						state.InboxReady.wait_for(lock, std::chrono::milliseconds(50));
					}
				}
				if (WaitForSingleObject(stop.Get(), 0) != WAIT_OBJECT_0) break;
				if (received.Value->Type != console::MessageType::StatusSnapshot
					&& state.Inbox.size() == 256)
				{
					if (received.Value->Type == console::MessageType::Event) ++state.Dropped;
				}
				else if (received.Value->Type != console::MessageType::StatusSnapshot)
					state.Inbox.push_back(std::move(*received.Value));
			}
			std::scoped_lock postLock(state.PostMutex);
			if (WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) break;
			if (!state.NotificationPending.exchange(true, std::memory_order_acq_rel))
				screen.PostEvent(ftxui::Event::Custom);
		}
		std::scoped_lock postLock(state.PostMutex);
		if (WaitForSingleObject(stop.Get(), 0) != WAIT_OBJECT_0)
		{
			SetEvent(stop.Get());
			screen.Exit();
		}
	});
	const auto outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	const auto inputHandle = GetStdHandle(STD_INPUT_HANDLE);
	DWORD originalInputMode = 0;
	const bool hasInputMode = GetConsoleMode(inputHandle, &originalInputMode) != 0;
	if (hasInputMode)
		SetConsoleMode(inputHandle, originalInputMode & ~ENABLE_PROCESSED_INPUT);
	DWORD originalOutputMode = 0;
	const bool hasConsoleMode = GetConsoleMode(outputHandle, &originalOutputMode) != 0;
	if (hasConsoleMode)
		SetConsoleMode(outputHandle, originalOutputMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	std::fputs("\x1b[?2004h", stdout);
	std::fflush(stdout);
	screen.Loop(view);
	std::fputs("\x1b[?2004l", stdout);
	std::fflush(stdout);
	if (hasConsoleMode) SetConsoleMode(outputHandle, originalOutputMode);
	if (hasInputMode) SetConsoleMode(inputHandle, originalInputMode);
	{
		std::scoped_lock lock(state.PostMutex);
		SetEvent(stop.Get());
	}
	state.OutboxReady.notify_all();
	if (WaitForSingleObject(writer.native_handle(), 2500) != WAIT_OBJECT_0)
	{
		TerminateProcess(GetCurrentProcess(), 0);
		return 0;
	}
	writer.join();
	if (WaitForSingleObject(reader.native_handle(), 2500) != WAIT_OBJECT_0)
	{
		// A stuck pipe driver must not keep a closed companion alive or let
		// its reader access FTXUI after the owner loop has gone away.
		TerminateProcess(GetCurrentProcess(), 0);
		return 0;
	}
	reader.join();
	return 0;
}
