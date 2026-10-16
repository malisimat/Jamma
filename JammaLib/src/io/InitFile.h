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
#include "../base/LoggingConfig.h"
#include "../utils/CommonTypes.h"

namespace io
{
	using LoggingConfig = base::LoggingConfig;

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
		// Written only after the corresponding generated file is published.
		// Absent in legacy defaults, whose existing files remain user selected.
		std::string RigOrigin;
		std::string JamOrigin;

		utils::Position2d WinPos{ 0, 0 };
		utils::Size2d WinSize{ 1400u, 1000u };
		LoggingConfig Logging;
		bool ConsoleAutoStart = true;
	};
}
