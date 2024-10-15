///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "InitFile.h"

using namespace io;
using audio::BehaviourParams;

const std::string InitFile::DefaultJson(std::string roamingPath)
{
	return "{\"rig\":\"" + roamingPath + "\\default.rig\",\"jam\":\"" + roamingPath + "\\default.jam\",\"jamload\":1,\"rigload\":0,\"win\":[-0,0,1400,1000]}";
}

std::optional<InitFile> InitFile::FromStream(std::stringstream ss)
{
	auto root = Json::FromStream(std::move(ss));

	if (!root.has_value())
		return std::nullopt;

	if (root.value().index() != 6)
		return std::nullopt;

	auto iniParams = std::get<Json::JsonPart>(root.value());

	InitFile ini;
	ini.JamLoadType = LOAD_LAST;
	ini.RigLoadType = LOAD_LAST;

	auto iter = iniParams.KeyValues.find("jam");
	if (iter != iniParams.KeyValues.end())
	{
		if (iniParams.KeyValues["jam"].index() == 4)
		{
			ini.Jam = utils::DecodeUtf8(std::get<std::string>(iniParams.KeyValues["jam"]));
		}
	}

	iter = iniParams.KeyValues.find("rig");
	if (iter != iniParams.KeyValues.end())
	{
		if (iniParams.KeyValues["rig"].index() == 4)
		{
			ini.Rig = utils::DecodeUtf8(std::get<std::string>(iniParams.KeyValues["rig"]));
		}
	}

	iter = iniParams.KeyValues.find("jamload");
	if (iter != iniParams.KeyValues.end())
	{
		if (iniParams.KeyValues["jamload"].index() == 2)
		{
			ini.JamLoadType = (LoadType)std::get<unsigned long>(iniParams.KeyValues["jamload"]);
		}
	}

	iter = iniParams.KeyValues.find("rigload");
	if (iter != iniParams.KeyValues.end())
	{
		if (iniParams.KeyValues["rigload"].index() == 2)
		{
			ini.RigLoadType = (LoadType)std::get<unsigned long>(iniParams.KeyValues["rigload"]);
		}
	}

	iter = iniParams.KeyValues.find("win");
	if (iter != iniParams.KeyValues.end())
	{
		if (iniParams.KeyValues["win"].index() == 5)
		{
			auto jsonArray = std::get<Json::JsonArray>(iniParams.KeyValues["win"]);

			if (jsonArray.Array.index() == 1)
			{
				auto winArray = std::get<std::vector<long>>(jsonArray.Array);

				if (winArray.size() > 1)
				{
					ini.WinPos = { winArray[0], winArray[1] };

					if (winArray.size() > 3)
					{
						unsigned int w = winArray[2] > winArray[0] ? (unsigned int)(winArray[2] - winArray[0]) : 1;
						unsigned int h = winArray[3] > winArray[1] ? (unsigned int)(winArray[3] - winArray[1]) : 1;
						ini.WinSize = { w, h };
					}
				}
			}
		}
	}

	return ini;
}

bool InitFile::ToStream(InitFile jam, std::stringstream ss)
{
	return false;
}
