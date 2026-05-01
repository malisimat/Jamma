///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "ConsoleTui.h"
#include "Main.h"

namespace
{
	void ReportConsoleRedirectFailure(const char* streamName)
	{
		OutputDebugStringA("Jamma ConsoleTui failed to redirect ");
		OutputDebugStringA(streamName);
		OutputDebugStringA("\n");
	}
}

ConsoleTui::ConsoleTui()
{
	if (!GetConsoleWindow())
		_ownsConsole = (AllocConsole() != FALSE);

	_ReopenStandardStreams();
}

ConsoleTui::~ConsoleTui()
{
	fflush(stdout);
	fflush(stderr);

	if (_ownsConsole)
		FreeConsole();
}

void ConsoleTui::_ReopenStandardStreams()
{
	FILE* stream = nullptr;
	if (freopen_s(&stream, "CONOUT$", "w", stdout) != 0)
		ReportConsoleRedirectFailure("stdout");

	stream = nullptr;
	if (freopen_s(&stream, "CONOUT$", "w", stderr) != 0)
		ReportConsoleRedirectFailure("stderr");

	stream = nullptr;
	if (freopen_s(&stream, "CONIN$", "r", stdin) != 0)
		ReportConsoleRedirectFailure("stdin");
}
