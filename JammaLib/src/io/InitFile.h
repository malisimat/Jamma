///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <vector>
#include <stack>
#include <map>
#include <optional>
#include <variant>
#include <iostream>
#include <sstream>
#include "Json.h"
#include "../audio/AudioMixer.h"
#include "../utils/CommonTypes.h"

namespace io
{
	struct LoggingConfig
	{
		std::string Midi;   // "verbose" to enable verbose MIDI packet logging
		std::string Audio;  // "verbose" to enable verbose audio logging
		std::string Event;  // "verbose" to enable verbose event logging
	};

	struct InitFile
	{
		static std::optional<InitFile> FromStream(std::stringstream ss);
		static bool ToStream(InitFile ini, std::stringstream& ss);
		static const std::string DefaultJson(std::string roamingPath);
		static const void SetWinParams(InitFile& ini, const Json::JsonArray& array);

		enum LoadType
		{
			LOAD_LAST,
			LOAD_SPECIFIC,
			LOAD_DEFAULT
		};

		std::wstring Jam;
		std::wstring Rig;
		LoadType JamLoadType;
		LoadType RigLoadType;

		utils::Position2d WinPos;
		utils::Size2d WinSize;
		LoggingConfig Logging;
	};
}
