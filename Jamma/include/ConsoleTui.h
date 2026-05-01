///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <cstdio>

class ConsoleTui
{
public:
	ConsoleTui();
	~ConsoleTui();

	ConsoleTui(const ConsoleTui&) = delete;
	ConsoleTui& operator=(const ConsoleTui&) = delete;

private:
	void _ReopenStandardStreams();

private:
	bool _ownsConsole = false;
};
