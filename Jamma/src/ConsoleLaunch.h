#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace console
{
	// Quote one Windows argv item for CreateProcessW/CRT parsing.
	inline std::wstring QuoteWindowsArgument(std::wstring_view value)
	{
		std::wstring quoted = L"\"";
		std::size_t slashes = 0;
		for (const auto ch : value)
		{
			if (ch == L'\\') { ++slashes; continue; }
			if (ch == L'"')
			{
				quoted.append(slashes * 2 + 1, L'\\');
				quoted += L'"';
				slashes = 0;
				continue;
			}
			quoted.append(slashes, L'\\');
			slashes = 0;
			quoted += ch;
		}
		quoted.append(slashes * 2, L'\\');
		quoted += L'"';
		return quoted;
	}

	enum class LaunchHost { WindowsTerminal, ConsoleHost };
	enum class LaunchOutcome { WindowsTerminal, ConsoleHost, Failed, Stopped };

	// The callbacks are the process/pipe boundary. An accepted Terminal
	// session ends normally without starting a second companion.
	template<class Attempt, class Stopping, class Fallback>
	LaunchOutcome RunLaunchPlan(bool terminalAvailable, Attempt&& attempt,
		Stopping&& stopping, Fallback&& fallback)
	{
		if (terminalAvailable && attempt(LaunchHost::WindowsTerminal))
			return LaunchOutcome::WindowsTerminal;
		if (stopping()) return LaunchOutcome::Stopped;
		fallback();
		return attempt(LaunchHost::ConsoleHost)
			? LaunchOutcome::ConsoleHost : LaunchOutcome::Failed;
	}

	inline std::wstring SiblingCompanionPath(std::wstring_view jammaPath)
	{
		return (std::filesystem::path(jammaPath).parent_path()
			/ L"JammaConsole.exe").wstring();
	}

	inline std::wstring ConsoleArguments(std::wstring_view pipeName,
		std::wstring_view token)
	{
		return L"--pipe " + QuoteWindowsArgument(pipeName)
			+ L" --token " + QuoteWindowsArgument(token);
	}

	inline std::wstring TerminalArguments(std::wstring_view windowName,
		std::wstring_view companionPath, std::wstring_view pipeName,
		std::wstring_view token)
	{
		return L"-w " + QuoteWindowsArgument(windowName) + L" --size 100,30 "
			+ QuoteWindowsArgument(companionPath) + L" " + ConsoleArguments(pipeName, token);
	}
}
