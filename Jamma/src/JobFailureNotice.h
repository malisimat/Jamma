#pragma once
#include <Windows.h>
#include <algorithm>
#include <array>
#include <string_view>
#include <deque>
#include <filesystem>
#include <string>
#include "../../console/LineRing.h"

// UI-owned, once-only exit notification. Called after Scene joins, while its
// immutable failure record and the main window still exist. Never uses streams.
class JobFailureNotice
{
public:
	template<class Present, class Persist, class Debug>
	void Report(std::string_view detail, Present present, Persist persist, Debug debug) noexcept
	{
		if (_reported || detail.empty()) return;
		_reported = true;
		try { if (present(detail)) return; } catch (...) {}
		try { if (persist(detail)) return; } catch (...) {}
		try { debug(detail); } catch (...) {}
	}
	// Best-effort transcript retention before teardown. The independent record
	// above remains available even if a full ring or failed UI sink loses lines.
	template<class Append>
	static void Drain(console::LineRing& lines, std::deque<std::string>& pending, Append append) noexcept
	{
		try
		{
			while (!pending.empty()) { append(pending.front()); pending.pop_front(); }
			for (std::size_t count = 0; count < console::LineRing::Capacity; ++count)
			{
				auto line = lines.Take();
				if (!line) break;
				append(*line);
			}
		}
		catch (...) {} // Modal/persistent delivery does not depend on this sink.
	}
	static std::wstring FallbackPath(const std::wstring& defaultsPath)
	{ return (std::filesystem::path(defaultsPath).parent_path() / L"worker-failure.log").wstring(); }
	static bool Show(HWND owner, std::string_view detail) noexcept
	{
		std::array<wchar_t, 4096> wide{};
		const auto length = MultiByteToWideChar(CP_UTF8, 0, detail.data(),
			static_cast<int>((std::min)(detail.size(), wide.size() - 1)), wide.data(), static_cast<int>(wide.size() - 1));
		if (!length) return false;
		return MessageBoxW(owner, wide.data(), L"Jamma stopped after a worker failure", MB_OK | MB_ICONERROR | MB_TASKMODAL) != 0;
	}
	static bool Persist(const wchar_t* path, std::string_view detail) noexcept
	{
		// One bounded record, replacing the previous failure. Direct Win32 I/O
		// remains usable even if capture or its startup sink has failed/stopped.
		const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		DWORD written = 0;
		const auto length = static_cast<DWORD>((std::min)(detail.size(), std::size_t{4095}));
		const bool saved = WriteFile(file, detail.data(), length, &written, nullptr) && written == length;
		const bool flushed = FlushFileBuffers(file) != FALSE;
		const bool closed = CloseHandle(file) != FALSE;
		return saved && flushed && closed;
	}
	static void Debug(std::string_view detail) noexcept
	{
		std::array<char, 4096> text{};
		std::copy_n(detail.data(), (std::min)(detail.size(), text.size() - 1), text.data());
		OutputDebugStringA(text.data());
	}
private:
	bool _reported = false;
};