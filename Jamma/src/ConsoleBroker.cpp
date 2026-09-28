#include "ConsoleBroker.h"
#include "ConsoleLaunch.h"
#include "../../console/WindowsPipe.h"
#include "../../console/SessionGate.h"
#include "../../console/CommandMailbox.h"
#include "../../console/OutboundMailbox.h"
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <utility>
#include <vector>

namespace console
{
	struct ConsoleBroker::State
	{
		explicit State(std::wstring path, std::shared_ptr<CommandMailbox> commands,
			std::string initialStatus)
			: CompanionPath(std::move(path)), Commands(std::move(commands)),
			Events(std::make_shared<OutboundMailbox>()),
			StopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr))
		{
			Events->SetStatus(std::move(initialStatus));
		}
		std::wstring CompanionPath;
		std::shared_ptr<CommandMailbox> Commands;
		std::shared_ptr<OutboundMailbox> Events;
		UniqueHandle StopEvent;
		std::atomic<bool> Connected{ false };
		std::atomic<bool> FallbackNotice{ false };
		std::atomic<bool> Finished{ false };
	};

	std::vector<std::uint8_t> ConsoleBroker::CurrentUserSid()
	{
		UniqueHandle token;
		HANDLE raw = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return {};
		token.Reset(raw);
		DWORD size = 0;
		GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
		if (!size) return {};
		std::vector<std::uint8_t> info(size);
		if (!GetTokenInformation(token.Get(), TokenUser, info.data(), size, &size)) return {};
		const auto sid = reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid;
		std::vector<std::uint8_t> result(GetLengthSid(sid));
		if (!CopySid(static_cast<DWORD>(result.size()), result.data(), sid)) return {};
		return result;
	}

	bool ConsoleBroker::SameUserClient(HANDLE pipe, const std::vector<std::uint8_t>& userSid,
		HANDLE stop)
	{
		if (!ImpersonateNamedPipeClient(pipe)) return false;
		HANDLE rawToken = nullptr;
		const bool opened = !!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &rawToken);
		UniqueHandle token(rawToken);
		bool same = false;
		if (opened)
		{
			DWORD size = 0;
			GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
			alignas(TOKEN_USER) std::array<std::uint8_t, 256> info{};
			if (size && size <= info.size()
				&& GetTokenInformation(token.Get(), TokenUser, info.data(),
					static_cast<DWORD>(info.size()), &size))
				same = !!EqualSid(const_cast<std::uint8_t*>(userSid.data()),
					reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid);
		}
		const bool reverted = !!RevertToSelf();
		if (!reverted) SetEvent(stop); // Fail closed; no fallback while impersonated.
		return same && reverted;
	}

	std::string ConsoleBroker::RandomHex()
	{
		std::array<std::uint8_t, 16> random{};
		if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random.data(),
			static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
		constexpr char digits[] = "0123456789abcdef";
		std::string result;
		result.reserve(random.size() * 2);
		for (const auto byte : random)
		{
			result += digits[byte >> 4];
			result += digits[byte & 15];
		}
		return result;
	}

	std::wstring ConsoleBroker::WidenAscii(const std::string& value)
	{
		return { value.begin(), value.end() };
	}

	UniqueHandle ConsoleBroker::CreateServer(const std::wstring& name,
		const std::vector<std::uint8_t>& userSid)
	{
		LPWSTR sidText = nullptr;
		if (!ConvertSidToStringSidW(const_cast<void*>(static_cast<const void*>(userSid.data())), &sidText))
			return {};
		const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sidText) + L")";
		LocalFree(sidText);
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
			SDDL_REVISION_1, &descriptor, nullptr)) return {};
		SECURITY_ATTRIBUTES attributes{ sizeof(attributes), descriptor, FALSE };
		UniqueHandle pipe(CreateNamedPipeW(name.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, static_cast<DWORD>(MaxFrameBytes), static_cast<DWORD>(MaxFrameBytes), 0,
			&attributes));
		LocalFree(descriptor);
		return pipe;
	}

	bool ConsoleBroker::ConnectClient(HANDLE pipe, HANDLE stop, DWORD timeoutMs)
	{
		UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!event.Valid()) return false;
		OVERLAPPED operation{};
		operation.hEvent = event.Get();
		if (ConnectNamedPipe(pipe, &operation)) return true;
		const auto error = GetLastError();
		if (error == ERROR_PIPE_CONNECTED) return true;
		if (error != ERROR_IO_PENDING) return false;
		const HANDLE waits[]{ stop, event.Get() };
		const auto wait = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
		if (wait != WAIT_OBJECT_0 + 1)
		{
			CancelIoEx(pipe, &operation);
			DWORD done = 0;
			GetOverlappedResult(pipe, &operation, &done, TRUE);
			return false;
		}
		DWORD done = 0;
		return !!GetOverlappedResult(pipe, &operation, &done, FALSE);
	}

	std::wstring ConsoleBroker::FindWindowsTerminal()
	{
		const auto environment = [](const wchar_t* name) {
			const auto required = GetEnvironmentVariableW(name, nullptr, 0);
			if (!required) return std::wstring{};
			std::wstring value(required, L'\0');
			const auto length = GetEnvironmentVariableW(name, value.data(), required);
			if (!length || length >= required) return std::wstring{};
			value.resize(length);
			return value;
		};
		const auto available = [](const std::filesystem::path& path) {
			const auto attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
		};
		const auto localAppData = environment(L"LOCALAPPDATA");
		if (!localAppData.empty())
		{
			const auto alias = std::filesystem::path(localAppData)
				/ L"Microsoft" / L"WindowsApps" / L"wt.exe";
			if (available(alias)) return alias.wstring();
		}
		// Search only absolute PATH entries; SearchPathW with a null path also
		// searches the app's working directory, which is not trusted here.
		const auto path = environment(L"PATH");
		for (std::size_t begin = 0; begin < path.size();)
		{
			const auto end = path.find(L';', begin);
			auto part = path.substr(begin, end == std::wstring::npos ? end : end - begin);
			if (part.size() >= 2 && part.front() == L'"' && part.back() == L'"')
				part = part.substr(1, part.size() - 2);
			if (std::filesystem::path(part).is_absolute())
			{
				const auto candidate = std::filesystem::path(part) / L"wt.exe";
				if (available(candidate)) return candidate.wstring();
			}
			if (end == std::wstring::npos) break;
			begin = end + 1;
		}
		return {};
	}

	UniqueHandle ConsoleBroker::Launch(const std::wstring& application, const std::wstring& arguments,
		bool newConsole)
	{
		std::wstring command = QuoteWindowsArgument(application) + L" " + arguments;
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr,
			FALSE, newConsole ? CREATE_NEW_CONSOLE : 0,
			nullptr, nullptr, &startup, &process)) return {};
		CloseHandle(process.hThread);
		return UniqueHandle(process.hProcess);
	}

	bool ConsoleBroker::RunAttempt(const std::shared_ptr<State>& state,
		bool terminal, const std::wstring& terminalPath,
		const std::vector<std::uint8_t>& userSid)
	{
		const auto nonce = RandomHex();
		const auto token = RandomHex();
		if (nonce.empty() || token.empty()) return false;
		const auto pipeName = L"\\\\.\\pipe\\JammaConsole-"
			+ std::to_wstring(GetCurrentProcessId()) + L"-" + WidenAscii(nonce);
		auto pipe = CreateServer(pipeName, userSid);
		if (!pipe.Valid()) return false;
		UniqueHandle launchedProcess;
		if (terminal)
		{
			const auto windowName = L"Jamma-" + std::to_wstring(GetCurrentProcessId())
				+ L"-" + WidenAscii(nonce);
			launchedProcess = Launch(terminalPath, TerminalArguments(windowName,
				state->CompanionPath, pipeName, WidenAscii(token)), false);
		}
		else launchedProcess = Launch(state->CompanionPath,
			ConsoleArguments(pipeName, WidenAscii(token)), true);
		if (!launchedProcess.Valid()) return false;
		if (!ConnectClient(pipe.Get(), state->StopEvent.Get(), terminal ? 4000 : 3000))
		{
			if (!terminal) TerminateProcess(launchedProcess.Get(), 1);
			return false;
		}
		const auto hello = ReadMessage(pipe.Get(), state->StopEvent.Get(), 4000);
		SessionGate gate;
		gate.Begin(token);
		if (!hello.Value || hello.Value->Type != MessageType::Hello
			|| !gate.AcceptHello(hello.Value->Text,
				SameUserClient(pipe.Get(), userSid, state->StopEvent.Get())))
		{
			if (!terminal) TerminateProcess(launchedProcess.Get(), 1);
			return false;
		}
		state->Connected.store(true, std::memory_order_release);
		std::atomic<bool> writerStopping{ false };
		std::thread writer([&] {
			try
			{
				while (!writerStopping.load(std::memory_order_acquire)
					&& WaitForSingleObject(state->StopEvent.Get(), 0) != WAIT_OBJECT_0)
				{
					auto outbound = state->Events->Take(writerStopping);
					if (outbound && !WriteMessage(pipe.Get(), state->StopEvent.Get(), *outbound, 500))
					{
						SetEvent(state->StopEvent.Get());
						break;
					}
				}
			}
			catch (...) { SetEvent(state->StopEvent.Get()); }
		});
		try
		{
			for (;;)
			{
				const auto message = ReadMessage(pipe.Get(), state->StopEvent.Get());
				if (!message.Value) break;
				if (message.Value->Type != MessageType::CommandRequest) break;
				if (!gate.AcceptRequest(message.Value->RequestId,
					WaitForSingleObject(state->StopEvent.Get(), 0) == WAIT_OBJECT_0)) break;
				auto completion = state->Commands->Submit(std::move(message.Value->Text));
				if (!completion)
				{
					if (!state->Events->PublishResult({ MessageType::CommandResult,
						message.Value->RequestId, "Command queue full or shutting down" },
						[&] { return WaitForSingleObject(state->StopEvent.Get(), 0) == WAIT_OBJECT_0; })) break;
					continue;
				}
				while (completion->wait_for(std::chrono::milliseconds(50))
					!= std::future_status::ready)
					if (WaitForSingleObject(state->StopEvent.Get(), 0) == WAIT_OBJECT_0) break;
				if (WaitForSingleObject(state->StopEvent.Get(), 0) == WAIT_OBJECT_0) break;
				if (!state->Events->PublishResult({ MessageType::CommandResult,
					message.Value->RequestId, completion->get() },
					[&] { return WaitForSingleObject(state->StopEvent.Get(), 0) == WAIT_OBJECT_0; })) break;
			}
		}
		catch (...) { SetEvent(state->StopEvent.Get()); }
		writerStopping.store(true, std::memory_order_release);
		writer.join();
		FinishSession(gate,
			WaitForSingleObject(state->StopEvent.Get(), 0) == WAIT_OBJECT_0,
			!terminal,
			[&] {
				UniqueHandle neverStop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
				if (neverStop.Valid()) WriteMessage(pipe.Get(), neverStop.Get(),
					{ MessageType::Shutdown, 0, "" }, 500);
			},
			[&] { return WaitForSingleObject(launchedProcess.Get(), 500) != WAIT_TIMEOUT; },
			[&] { TerminateProcess(launchedProcess.Get(), 1); });
		state->Connected.store(false, std::memory_order_release);
		return true;
	}

	void ConsoleBroker::Run(const std::shared_ptr<State>& state)
	{
		try
		{
			const auto userSid = CurrentUserSid();
			if (!userSid.empty())
			{
				const auto terminal = FindWindowsTerminal();
				RunLaunchPlan(!terminal.empty(),
					[&](LaunchHost host) {
						return RunAttempt(state, host == LaunchHost::WindowsTerminal,
							terminal, userSid);
					},
					[&] { return WaitForSingleObject(state->StopEvent.Get(), 0) == WAIT_OBJECT_0; },
					[&] { state->FallbackNotice.store(true, std::memory_order_release); });
			}
		}
		catch (...) { OutputDebugStringW(L"[CONSOLE] Broker worker failed.\n"); }
		state->Connected.store(false, std::memory_order_release);
		state->Events->Close();
		state->Finished.store(true, std::memory_order_release);
	}

	ConsoleBroker::~ConsoleBroker()
	{
		if (!Stop() && _worker.joinable())
		{
			// At process exit only: retain State in the worker. Reopen never
			// detaches or creates a second generation while this one is live.
			OutputDebugStringW(L"[CONSOLE] Worker did not stop before process exit.\n");
			_worker.detach();
		}
	}

	bool ConsoleBroker::Start(const std::wstring& companionPath,
		std::shared_ptr<CommandMailbox> commands, std::string initialStatus)
	{
		if (_state && !_state->Finished.load(std::memory_order_acquire)) return false;
		if (!Stop()) return false;
		if (!commands) return false;
		if (GetFileAttributesW(companionPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
		_state = std::make_shared<State>(companionPath, std::move(commands),
			std::move(initialStatus));
		if (!_state->StopEvent.Valid()) { _state.reset(); return false; }
		_worker = std::thread([state = _state] { Run(state); });
		return true;
	}

	bool ConsoleBroker::Stop() noexcept
	{
		if (!_state) return true;
		SetEvent(_state->StopEvent.Get());
		if (_worker.joinable())
		{
			if (WaitForSingleObject(_worker.native_handle(), 2500) != WAIT_OBJECT_0)
				return false; // Keep generation owned; refuse a duplicate reopen.
			_worker.join();
		}
		_state.reset();
		return true;
	}

	bool ConsoleBroker::Reopen(const std::wstring& companionPath,
		std::shared_ptr<CommandMailbox> commands, std::string initialStatus)
	{
		if (Connected() && WaitForSingleObject(_state->StopEvent.Get(), 0) != WAIT_OBJECT_0)
			return true;
		if (!Stop()) return false;
		return Start(companionPath, std::move(commands), std::move(initialStatus));
	}

	bool ConsoleBroker::Connected() const noexcept
	{
		return _state && _state->Connected.load(std::memory_order_acquire);
	}

	bool ConsoleBroker::ConsumeFallbackNotice() noexcept
	{
		return _state && _state->FallbackNotice.exchange(false, std::memory_order_acq_rel);
	}

	std::shared_ptr<OutboundMailbox> ConsoleBroker::Events() const noexcept
	{
		return _state ? _state->Events : nullptr;
	}
}
