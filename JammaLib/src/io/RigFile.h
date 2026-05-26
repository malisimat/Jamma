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

		enum MidiTriggerEvent
		{
			NOTE,
			CC
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
			std::string Device;

			static std::optional<TriggerPair> FromJson(Json::JsonPart json);
		};

		struct Trigger
		{
			struct MidiTriggerBindingSpec
			{
				MidiTriggerEvent Kind;
				unsigned int Channel;
				unsigned int Id;
				unsigned int State;
				bool MatchAnyChannel;

				static std::optional<MidiTriggerBindingSpec> FromJson(Json::JsonPart json);
			};

			struct MidiTriggerBinding
			{
				std::string Device;
				MidiTriggerBindingSpec Activate;
				MidiTriggerBindingSpec Ditch;

				static std::optional<MidiTriggerBinding> FromJson(Json::JsonPart json);
			};

			std::string Name;
			unsigned int StationType;
			std::vector<TriggerPair> TriggerPairs;
			std::vector<unsigned int> InputChannels;
			std::vector<unsigned int> MidiInputChannels;
			std::vector<std::string> MidiInputDevices;
			std::optional<MidiTriggerBinding> MidiTrigger;

			static std::optional<Trigger> FromJson(Json::JsonPart json);
		};

		Version Version;
		std::string Name;
		UserConfig User;
		std::vector<Trigger> Triggers;
	};
}
