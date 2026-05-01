///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "ConsoleTui.h"
#include "Main.h"

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
	freopen_s(&_stdoutStream, "CONOUT$", "w", stdout);
	freopen_s(&_stderrStream, "CONOUT$", "w", stderr);
	freopen_s(&_stdinStream, "CONIN$", "r", stdin);
}
