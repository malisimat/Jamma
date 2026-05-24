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
#include "UserConfig.h"

namespace io
{
	struct RigFile
	{
		enum Version
		{
			VERSION_V,
			VERSION_LEGACY
		};

		static std::optional<RigFile> FromStream(std::stringstream ss);
		static bool ToStream(RigFile jam, std::stringstream& ss);
		static const std::string DefaultJson;


		struct TriggerPair
		{
			enum BindingSource
			{
				SOURCE_KEYBOARD,
				SOURCE_SERIAL
			};

			unsigned int ActivateDown;
			unsigned int ActivateUp;
			unsigned int DitchDown;
			unsigned int DitchUp;
			BindingSource Source = SOURCE_KEYBOARD;

			static std::optional<TriggerPair> FromJson(Json::JsonPart json);
		};

		struct Trigger
		{
			std::string Name;
			unsigned int StationType;
			std::vector<TriggerPair> TriggerPairs;
			std::vector<unsigned int> InputChannels;
			std::vector<unsigned int> MidiInputChannels;

			static std::optional<Trigger> FromJson(Json::JsonPart json);
		};

		Version Version;
		std::string Name;
		UserConfig User;
		std::vector<Trigger> Triggers;
	};
}
