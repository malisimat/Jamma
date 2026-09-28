#include "../../console/WindowsPipe.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

struct ConsoleClientState
{
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
	std::deque<std::string> Lines;
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

	auto screen = ftxui::ScreenInteractive::TerminalOutput();
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
			if (message.Type == console::MessageType::Event
				|| message.Type == console::MessageType::CommandResult)
			{
				if (state.Lines.size() == 100) state.Lines.pop_front();
				state.Lines.push_back(std::move(message.Text));
			}
		}
		if (dropped)
		{
			if (state.Lines.size() == 100) state.Lines.pop_front();
			state.Lines.push_back("[CONSOLE] Lost " + std::to_string(dropped) + " events");
		}
	};
	auto view = ftxui::Renderer([&] {
		ftxui::Elements rows{ ftxui::text("JAMMA"), ftxui::separator() };
		for (const auto& line : state.Lines) rows.push_back(ftxui::text(line));
		rows.push_back(ftxui::filler());
		rows.push_back(ftxui::separator());
		rows.push_back(ftxui::text(state.Status));
		return ftxui::vbox(std::move(rows));
	});
	view = ftxui::CatchEvent(view, [&](ftxui::Event event) {
		if (event == ftxui::Event::Custom)
		{
			state.NotificationPending.store(false, std::memory_order_release);
			drain();
			return true;
		}
		if (event == ftxui::Event::CtrlC) return true;
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
	screen.Loop(view);
	{
		std::scoped_lock lock(state.PostMutex);
		SetEvent(stop.Get());
	}
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
