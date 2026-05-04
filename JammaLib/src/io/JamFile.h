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
#include "../engine/Timer.h"
#include "../audio/AudioMixer.h"

namespace io
{
	struct JamFile
	{
		enum Version
		{
			VERSION_V,
			VERSION_LEGACY
		};

		static std::optional<JamFile> FromStream(std::stringstream ss);
		static bool ToStream(JamFile jam, std::stringstream& ss);
		static const std::string DefaultJson;

		struct NinjamConfig
		{
			std::string Host;
			std::string User;
			std::string Pass;
			std::string WorkDir;

			static std::optional<NinjamConfig> FromJson(Json::JsonPart json);
		};

		struct LoopMix
		{
			enum MixType
			{
				MIX_WIRE,
				MIX_PAN
			};

			MixType Mix;
			std::variant<std::vector<unsigned long>,std::vector<double>> Params;

			static std::optional<LoopMix> FromJson(Json::JsonPart json);
		};

		// Describes a single VST3 entry in a VstChain.
		struct VstEntry
		{
			// UTF-8 path to the .vst3 bundle or DLL.
			std::string Path;
			bool Bypass = false;

			static std::optional<VstEntry> FromJson(Json::JsonPart json);
		};

		struct Loop
		{
			std::string Name;
			unsigned long Length;
			unsigned long Index;
			unsigned long MasterLoopCount;
			double Level;
			double Speed;
			unsigned int MuteGroups;
			unsigned int SelectGroups;
			bool Muted;
			LoopMix Mix;
			std::vector<VstEntry> VstChain;

			static std::optional<Loop> FromJson(Json::JsonPart json);
		};

		struct LoopTake
		{
			std::string Name;
			std::vector<Loop> Loops;

			static std::optional<LoopTake> FromJson(Json::JsonPart json);
		};

		struct Station
		{
			std::string Name;
			unsigned int StationType;
			std::vector<LoopTake> LoopTakes;
			std::vector<VstEntry> VstChain;

			static std::optional<Station> FromJson(Json::JsonPart json);
		};

		Version Version;
		std::string Name;
		std::optional<NinjamConfig> Ninjam;
		std::vector<Station> Stations;
		unsigned long TimerTicks;
		unsigned int QuantiseSamps;
		engine::Timer::QuantisationType Quantisation;
	};
}
