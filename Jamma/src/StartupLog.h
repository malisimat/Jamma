#pragma once
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
namespace console
{
	// Historical name retained for boot-message callers. The sink now spans the
	// entire run. All methods run under ConsoleCapture's off-callback mutex.
	class StartupLog
	{
	public:
		explicit StartupLog(const std::wstring& defaultsPath, std::size_t maxBytes = 1024u * 1024u)
			: _maxBytes((std::max)(maxBytes, std::size_t{6}))
		{
			const auto directory = std::filesystem::path(defaultsPath).parent_path();
			_path = (directory / L"run.log").wstring();
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			if (error) { Fail("create-directory", error.value()); return; }
			Rotate();
		}
		~StartupLog() { Close(); }
		StartupLog(const StartupLog&) = delete;
		StartupLog& operator=(const StartupLog&) = delete;
		static std::string RunStart(std::string_view version)
		{
			SYSTEMTIME time{};
			TIME_ZONE_INFORMATION zone{};
			const auto kind = GetTimeZoneInformation(&zone);
			LONG offset = 0;
			if (kind == TIME_ZONE_ID_INVALID) GetSystemTime(&time);
			else
			{
				GetLocalTime(&time);
				offset = -zone.Bias;
				if (kind == TIME_ZONE_ID_DAYLIGHT) offset -= zone.DaylightBias;
				else if (kind == TIME_ZONE_ID_STANDARD) offset -= zone.StandardBias;
			}
			const auto magnitude = offset < 0 ? -offset : offset;
			std::ostringstream header;
			header << "[BOOT] run-start=" << time.wYear << '-' << std::setfill('0')
				<< std::setw(2) << time.wMonth << '-' << std::setw(2) << time.wDay << 'T'
				<< std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute << ':'
				<< std::setw(2) << time.wSecond << (offset < 0 ? '-' : '+')
				<< std::setw(2) << magnitude / 60 << ':' << std::setw(2) << magnitude % 60
				<< " Jamma v" << version;
			return header.str();
		}
		void Write(const std::string& line) { std::cout << line << '\n'; }
		void Append(std::string_view line)
		{
			if (!Available()) return;
			// Normal capture entries fit in 4096 bytes. Also bound direct callers
			// and tiny test limits without losing a UTF-8 tail at a rotation edge.
			do
			{
				auto count = (std::min)(line.size(), _maxBytes - 2);
				if (count < line.size())
					while (count && (static_cast<unsigned char>(line[count]) & 0xc0) == 0x80) --count;
				if (!count && !line.empty()) count = (std::min)(line.size(), _maxBytes - 2);
				if (_bytes > _maxBytes - count - 2)
				{
					Close();
					if (!Rotate()) return;
				}
				DWORD written = 0;
				if (!WriteFile(_file, line.data(), static_cast<DWORD>(count), &written, nullptr) || written != count)
				{ Fail("write", GetLastError()); Close(); return; }
				if (!WriteFile(_file, "\r\n", 2, &written, nullptr) || written != 2 || !FlushFileBuffers(_file))
				{ Fail("flush", GetLastError()); Close(); return; }
				_bytes += count + 2;
				line.remove_prefix(count);
			} while (!line.empty());
		}
		std::string TakeFailure() { return std::exchange(_failure, {}); }
		const std::wstring& Path() const noexcept { return _path; }
		bool Available() const noexcept { return _file != INVALID_HANDLE_VALUE; }
	private:
		friend struct StartupLogTestAccess;
		void Close() noexcept
		{
			if (_file != INVALID_HANDLE_VALUE) CloseHandle(_file);
			_file = INVALID_HANDLE_VALUE;
		}
		bool Rotate()
		{
			const auto oldest = _path + L".3";
			if (!DeleteFileW(oldest.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
			{ Fail("rotation-delete", GetLastError()); return false; }
			for (int index = 2; index >= 0; --index)
			{
				const auto from = _path + (index ? L"." + std::to_wstring(index) : L"");
				const auto to = _path + L"." + std::to_wstring(index + 1);
				if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) && GetLastError() != ERROR_FILE_NOT_FOUND)
				{ Fail("rotation-move", GetLastError()); return false; }
			}
			_file = CreateFileW(_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			_bytes = 0;
			if (!Available()) { Fail("open", GetLastError()); return false; }
			return true;
		}
		void Fail(const char* operation, unsigned long error)
		{ _failure = std::string("[ERROR][diagnostics] run.log stage=") + operation + " windows-error=" + std::to_string(error) + "; console/debugger output remains available"; }
		std::wstring _path;
		HANDLE _file = INVALID_HANDLE_VALUE;
		std::size_t _bytes = 0, _maxBytes;
		std::string _failure;
	};
}
