#pragma once
#include <Windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
namespace console
{
	// Called only under ConsoleCapture's off-callback mutex (or during setup).
	class StartupLog
	{
	public:
		explicit StartupLog(const std::wstring& defaultsPath, std::size_t maxBytes = 1024u * 1024u) : _maxBytes(maxBytes)
		{
			const auto directory = std::filesystem::path(defaultsPath).parent_path();
			_path = (directory / L"startup.log").wstring();
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			if (error) { Fail("create-directory", error.value()); return; }
			const auto oldest = _path + L".3";
			if (!DeleteFileW(oldest.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) Fail("rotation-delete", GetLastError());
			for (int index = 2; index >= 0; --index)
			{
				const auto from = _path + (index ? L"." + std::to_wstring(index) : L"");
				const auto to = _path + L"." + std::to_wstring(index + 1);
				if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) && GetLastError() != ERROR_FILE_NOT_FOUND)
				{ Fail("rotation-move", GetLastError()); return; }
			}
			_file = CreateFileW(_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (_file == INVALID_HANDLE_VALUE) Fail("open", GetLastError());
		}
		~StartupLog() { if (_file != INVALID_HANDLE_VALUE) CloseHandle(_file); }
		StartupLog(const StartupLog&) = delete;
		StartupLog& operator=(const StartupLog&) = delete;
		void Write(const std::string& line) { std::cout << line << '\n'; }
		void Append(std::string_view line)
		{
			if (_file == INVALID_HANDLE_VALUE || _limited) return;
			if (_bytes + line.size() + 2 > _maxBytes)
			{ _limited = true; Fail("size-limit", ERROR_FILE_TOO_LARGE); return; }
			DWORD written = 0;
			if (!WriteFile(_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size())
			{ Fail("write", GetLastError()); CloseHandle(_file); _file = INVALID_HANDLE_VALUE; return; }
			_bytes += written;
			if (!WriteFile(_file, "\r\n", 2, &written, nullptr) || written != 2 || !FlushFileBuffers(_file))
			{ Fail("flush", GetLastError()); CloseHandle(_file); _file = INVALID_HANDLE_VALUE; return; }
			_bytes += 2;
		}
		std::string TakeFailure() { return std::exchange(_failure, {}); }
		const std::wstring& Path() const noexcept { return _path; }
		bool Available() const noexcept { return _file != INVALID_HANDLE_VALUE; }
	private:
		friend struct StartupLogTestAccess;
		void Fail(const char* operation, unsigned long error)
		{ _failure = std::string("[ERROR][diagnostics] startup.log stage=") + operation + " windows-error=" + std::to_string(error) + "; console/debugger output remains available"; }
		std::wstring _path;
		HANDLE _file = INVALID_HANDLE_VALUE;
		std::size_t _bytes = 0, _maxBytes;
		bool _limited = false;
		std::string _failure;
	};
}
