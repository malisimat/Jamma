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
	struct InitFile
	{
		static std::optional<InitFile> FromStream(std::stringstream ss);
		static bool ToStream(InitFile jam, std::stringstream ss);
		static const std::string DefaultJson(std::string roamingPath);

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
	};
}
