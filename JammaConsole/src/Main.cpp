#include "../../console/WindowsPipe.h"
#include "../../console/PromptEditor.h"
#include "../../console/PasteInput.h"
#include "../../console/SelectionController.h"
#include "../../console/ClipboardText.h"
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
#include <vector>

struct ConsoleClientState
{
	static ftxui::Color LineColor(std::string_view line)
	{
		if (line.starts_with("[NINJAM]"))
		{
			const auto message = line.substr(8);
			if (message.find("<you>") != std::string_view::npos) return ftxui::Color::GreenLight;
			if (message.find("(private)") != std::string_view::npos) return ftxui::Color::MagentaLight;
			if (message.find("Not connected") != std::string_view::npos
				|| message.find("Unknown command") != std::string_view::npos)
				return ftxui::Color::YellowLight;
			if (message.find('<') != std::string_view::npos) return ftxui::Color::CyanLight;
			return ftxui::Color::Cyan;
		}
		if (line.starts_with("[CONSOLE]") || line.starts_with("[BOOT]"))
			return ftxui::Color::Yellow;
		return ftxui::Color::Default;
	}
	static bool WriteClipboard(HWND owner, std::string_view text)
	{
		const auto wideText = console::ClipboardUtf8ToUtf16(text);
		if (!owner || !wideText) return false;
		const auto bytes = (wideText->size() + 1) * sizeof(wchar_t);
		const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (!memory) return false;
		auto* wide = static_cast<wchar_t*>(GlobalLock(memory));
		if (!wide) { GlobalFree(memory); return false; }
		std::copy(wideText->begin(), wideText->end(), wide);
		wide[wideText->size()] = L'\0';
		GlobalUnlock(memory);
		if (!OpenClipboard(owner)) { GlobalFree(memory); return false; }
		const bool saved = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
		CloseClipboard();
		if (!saved) GlobalFree(memory);
		return saved;
	}
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
	HWND ClipboardOwner = nullptr;
	std::condition_variable InboxReady;
	std::deque<console::Message> Inbox;
	std::optional<console::Message> PendingStatus;
	std::mutex PostMutex;
	std::atomic<bool> NotificationPending{ false };
	std::string Status = "Connecting to Jamma...";
	console::Transcript Transcript;
	console::SelectionController Selection;
	std::vector<console::TranscriptRow> VisibleRows;
	std::mutex EdgeMutex;
	std::condition_variable EdgeReady;
	std::atomic<int> EdgeDirection{ 0 };
	std::atomic<int> EdgeX{ 0 };
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
	state.ClipboardOwner = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
		0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
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
		state.Selection.Transcript.ReconcileBounds(state.Transcript.FirstId(), state.Transcript.LastId());
	};
	auto view = ftxui::Renderer([&] {
		const int width = std::max(1, screen.dimx());
		const int height = std::max(1, screen.dimy());
		const int visibleHeight = std::max(0, height - 5);
		ftxui::Elements rows;
		if (height >= 3) rows.push_back(ftxui::text(console::ClipColumns(
			"JAMMA  F1: input help", width)) | ftxui::color(ftxui::Color::CyanLight));
		if (height >= 4) rows.push_back(ftxui::separator());
		state.VisibleRows = state.Transcript.Visible(width, visibleHeight);
		for (const auto& row : state.VisibleRows)
		{
			const auto selected = state.Selection.Transcript.RowRange(row.Start, row.Text.size());
			const auto* logicalText = state.Transcript.TextFor(row.Start.EntryId);
			const auto lineColor = ConsoleClientState::LineColor(logicalText ? *logicalText : row.Text);
			if (!selected) { rows.push_back(ftxui::text(row.Text) | ftxui::color(lineColor)); continue; }
			const auto [first, last] = *selected;
			rows.push_back(ftxui::hbox({ ftxui::text(row.Text.substr(0, first)),
				ftxui::text(row.Text.substr(first, last - first)) | ftxui::inverted,
				ftxui::text(row.Text.substr(last)) }) | ftxui::color(lineColor));
		}
		for (int line = static_cast<int>(state.VisibleRows.size()); line < visibleHeight; ++line)
			rows.push_back(ftxui::text(""));
		if (height >= 5) rows.push_back(ftxui::separator());
		const auto prompt = state.Prompt.Text();
		const auto caret = state.Prompt.Caret();
		const auto label = width >= 3 ? "> " : width == 2 ? ">" : "";
		const int available = width - static_cast<int>(std::string_view(label).size());
		if (height >= 2)
		{
			ftxui::Elements promptPieces{ ftxui::text(label) };
			const auto selected = state.Prompt.SelectedRange();
			const auto addText = [&](std::string_view part, std::size_t at, bool showCaret) {
				for (const auto& cluster : console::Graphemes(part))
				{
					auto element = ftxui::text(std::string(part.substr(cluster.Start, cluster.End - cluster.Start)));
					if ((selected && at + cluster.Start >= selected->first
						&& at + cluster.Start < selected->second)
						|| (showCaret && at + cluster.Start == caret))
						element = element | ftxui::inverted;
					promptPieces.push_back(std::move(element));
				}
			};
			if (state.Selection.PromptOrigin)
			{
				const auto visible = console::ClipColumns(
					std::string_view(prompt).substr(*state.Selection.PromptOrigin), available);
				addText(visible, *state.Selection.PromptOrigin, true);
				if (visible.empty() && *state.Selection.PromptOrigin < prompt.size())
					promptPieces.push_back(ftxui::text("_") | ftxui::inverted);
				if (caret == prompt.size() && ftxui::string_width(visible) < available)
					promptPieces.push_back(ftxui::text(" ") | ftxui::inverted);
			}
			else
			{
				const auto next = console::NextGrapheme(prompt, caret);
				const auto caretText = caret < prompt.size() ? prompt.substr(caret, next - caret) : " ";
				const auto atCaret = ftxui::string_width(caretText) <= available ? caretText : "_";
				const auto begin = console::PromptWindowStart(prompt, caret,
					std::max(0, available - ftxui::string_width(atCaret)));
				const auto prefix = prompt.substr(begin, caret - begin);
				const auto remaining = std::max(0, available - ftxui::string_width(prefix) - ftxui::string_width(atCaret));
				addText(prefix, begin, false);
				promptPieces.push_back(ftxui::text(atCaret) | ftxui::inverted);
				const auto suffix = console::ClipColumns(std::string_view(prompt).substr(next), remaining);
				addText(suffix, next, false);
			}
			rows.push_back(ftxui::hbox(std::move(promptPieces)));
		}
		rows.push_back(ftxui::text(console::StatusForWidth(
			state.PromptError.empty() ? state.Status : state.PromptError, width))
			| ftxui::color(state.PromptError.empty() ? ftxui::Color::Cyan : ftxui::Color::YellowLight));
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
		if (event == ftxui::Event::CtrlC)
		{
			const auto copy = state.Selection.Transcript.Active
				? state.Selection.Transcript.Copy(state.Transcript.LogicalEntries())
				: state.Prompt.SelectedText();
			if (!copy.empty()) state.PromptError = ConsoleClientState::WriteClipboard(state.ClipboardOwner, copy)
				? "Copied selection" : "Clipboard unavailable";
			return true;
		}
		if (event == ftxui::Event::F1)
		{
			state.Transcript.Append(console::NoMouseHelp);
			state.Selection.Transcript.ReconcileBounds(state.Transcript.FirstId(), state.Transcript.LastId());
			state.Transcript.FollowTail();
			return true;
		}
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
			const auto width = std::max(1, screen.dimx());
			const auto height = std::max(1, screen.dimy());
			const auto viewport = std::max(0, height - 5);
			state.VisibleRows = state.Transcript.Visible(width, viewport);
			if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown)
			{
				if (mouse.button == ftxui::Mouse::WheelUp) state.Transcript.ScrollUp(width, 3);
				else state.Transcript.ScrollDown(width, 3);
				return true;
			}
			const auto transcriptPosition = [&](int x, int y) -> std::optional<console::TextPosition> {
				if (state.VisibleRows.empty()) return std::nullopt;
				const auto row = std::clamp(y - 2, 0, static_cast<int>(state.VisibleRows.size()) - 1);
				return console::RowPosition(state.VisibleRows[row].Start,
					state.VisibleRows[row].Text, x);
			};
			if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed)
			{
				state.Selection.Release();
				state.EdgeDirection.store(0, std::memory_order_release);
				if (height >= 2 && mouse.y == height - 2)
				{
					const auto caret = state.Prompt.Caret();
					const auto next = console::NextGrapheme(state.Prompt.Text(), caret);
					const auto atCaret = caret < state.Prompt.Text().size()
						? state.Prompt.Text().substr(caret, next - caret) : " ";
					const auto available = width - (width >= 3 ? 2 : width == 2 ? 1 : 0);
					const auto shownCaret = ftxui::string_width(atCaret) <= available ? atCaret : "_";
					const auto origin = console::PromptWindowStart(state.Prompt.Text(), caret,
						std::max(0, available - ftxui::string_width(shownCaret)));
					state.Selection.PressPrompt(console::PromptColumnToByte(state.Prompt.Text(),
						caret, width, mouse.x), origin, state.Prompt);
					return true;
				}
				if (viewport > 0 && mouse.y >= 2 && mouse.y < 2 + viewport)
				{
					if (const auto position = transcriptPosition(mouse.x, mouse.y))
					{
						state.Transcript.Pause();
						state.Selection.PressTranscript(*position, mouse.x, mouse.y, state.Prompt);
					}
					return true;
				}
			}
			if (mouse.motion == ftxui::Mouse::Moved)
			{
				if (state.Selection.Dragging == console::SelectionRegion::Prompt)
				{
					state.Selection.DragPrompt(console::PromptColumnToByteAtOrigin(state.Prompt.Text(),
						*state.Selection.PromptOrigin, width, mouse.x), state.Prompt);
					return true;
				}
				if (state.Selection.Dragging == console::SelectionRegion::Transcript)
				{
					const auto delta = console::EdgeScrollDelta(mouse.y - 2, viewport);
					state.EdgeX.store(mouse.x, std::memory_order_relaxed);
					state.EdgeDirection.store(delta, std::memory_order_release);
					if (delta) state.EdgeReady.notify_one();
					if (delta < 0) state.Transcript.ScrollUp(width, 1);
					else if (delta > 0) state.Transcript.ScrollDown(width, 1);
					state.VisibleRows = state.Transcript.Visible(width, viewport);
					if (!state.VisibleRows.empty())
					{
						const auto row = std::clamp(mouse.y - 2, 0,
							static_cast<int>(state.VisibleRows.size()) - 1);
						state.Selection.DragTranscriptRow(state.VisibleRows[row].Start,
							state.VisibleRows[row].Text, mouse.x, mouse.y);
					}
					return true;
				}
			}
			if (mouse.motion == ftxui::Mouse::Released
				&& state.Selection.Dragging != console::SelectionRegion::None)
			{
				state.Selection.Release();
				state.EdgeDirection.store(0, std::memory_order_release);
				return true;
			}
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
				if (WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) break;
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
	std::thread edgeTimer([&] {
		std::unique_lock lock(state.EdgeMutex);
		for (;;)
		{
			state.EdgeReady.wait(lock, [&] {
				return state.EdgeDirection.load(std::memory_order_acquire) != 0
					|| WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0;
			});
			if (WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) break;
			if (state.EdgeReady.wait_for(lock, std::chrono::milliseconds(80), [&] {
				return state.EdgeDirection.load(std::memory_order_acquire) == 0
					|| WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0;
			})) continue;
			lock.unlock();
			{
				std::scoped_lock postLock(state.PostMutex);
				if (WaitForSingleObject(stop.Get(), 0) != WAIT_OBJECT_0)
					screen.Post([&] {
						const auto direction = state.EdgeDirection.load(std::memory_order_acquire);
						if (!direction || state.Selection.Dragging != console::SelectionRegion::Transcript) return;
						const auto width = std::max(1, screen.dimx());
						const auto viewport = std::max(0, screen.dimy() - 5);
						if (!viewport) return;
						if (direction < 0) state.Transcript.ScrollUp(width, 1);
						else state.Transcript.ScrollDown(width, 1);
						state.VisibleRows = state.Transcript.Visible(width, viewport);
						if (!state.VisibleRows.empty())
						{
						const auto& row = direction < 0 ? state.VisibleRows.front() : state.VisibleRows.back();
							state.Selection.DragTranscriptRow(row.Start, row.Text,
								state.EdgeX.load(std::memory_order_relaxed),
								direction < 0 ? -1 : screen.dimy());
						}
					});
			}
			lock.lock();
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
	state.EdgeReady.notify_all();
	edgeTimer.join();
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
	if (state.ClipboardOwner) DestroyWindow(state.ClipboardOwner);
	return 0;
}
