///////////////////////////////////////////////////////////
//
// Author 2026 GitHub Copilot
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <windows.h>
#include "VstDebug.h"
#include "../utils/StringUtils.h"

namespace vst
{
	class VstDiagnostics final
	{
	public:
		static void Configure(const DebugOptions& options, const std::wstring& defaultLogPath = L"")
		{
			auto& state = _State();
			std::scoped_lock lock(state.Mutex);

			state.Enabled = options.LogToFile;
			state.LogPath = options.LogPath.empty() ? defaultLogPath : options.LogPath;

			if (state.Stream.is_open())
				state.Stream.close();

			if (!state.Enabled || state.LogPath.empty())
				return;

			std::filesystem::create_directories(std::filesystem::path(state.LogPath).parent_path());
			state.Stream.open(state.LogPath, std::ios::out | std::ios::trunc);
		}

		static bool IsEnabled() noexcept
		{
			return _State().Enabled;
		}

		static std::wstring LogPath()
		{
			auto& state = _State();
			std::scoped_lock lock(state.Mutex);
			return state.LogPath;
		}

		static void Log(std::string_view source,
			std::string_view event,
			const std::string& detail = std::string())
		{
			auto& state = _State();
			std::scoped_lock lock(state.Mutex);
			if (!state.Enabled || !state.Stream.is_open())
				return;

			state.Stream << _Timestamp() << " [tid=" << GetCurrentThreadId() << "] ["
				<< source << "] " << event;
			if (!detail.empty())
				state.Stream << " | " << detail;
			state.Stream << std::endl;
			state.Stream.flush();
		}

		static void Log(std::string_view source,
			std::string_view event,
			const std::wstring& detail)
		{
			Log(source, event, utils::EncodeUtf8(detail));
		}

	private:
		struct State
		{
			std::mutex Mutex;
			bool Enabled = false;
			std::wstring LogPath;
			std::ofstream Stream;
		};

		static State& _State()
		{
			static State state;
			return state;
		}

		static std::string _Timestamp()
		{
			using namespace std::chrono;
			const auto now = system_clock::now();
			const auto timeT = system_clock::to_time_t(now);
			const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

			std::tm tmValue{};
			localtime_s(&tmValue, &timeT);

			std::ostringstream ss;
			ss << std::put_time(&tmValue, "%Y-%m-%d %H:%M:%S")
				<< '.' << std::setw(3) << std::setfill('0') << millis.count();
			return ss.str();
		}
	};
}