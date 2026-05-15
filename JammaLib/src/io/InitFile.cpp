///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "InitFile.h"

namespace
{
	std::string EscapeJsonString(const std::string& value)
	{
		std::string escaped;
		escaped.reserve(value.size());

		for (const char ch : value)
		{
			switch (ch)
			{
			case '\\':
				escaped += "\\\\";
				break;
			case '"':
				escaped += "\\\"";
				break;
			default:
				escaped.push_back(ch);
				break;
			}
		}

		return escaped;
	}

	void ParseVstDebug(io::InitFile& ini, const io::Json::JsonPart& json)
	{
		auto iter = json.KeyValues.find("autoopen");
		if (iter != json.KeyValues.end() && iter->second.index() == 0)
			ini.VstDebug.AutoOpenEditor = std::get<bool>(iter->second);

		iter = json.KeyValues.find("logtofile");
		if (iter != json.KeyValues.end() && iter->second.index() == 0)
			ini.VstDebug.LogToFile = std::get<bool>(iter->second);

		iter = json.KeyValues.find("logpath");
		if (iter != json.KeyValues.end() && iter->second.index() == 4)
			ini.VstDebug.LogPath = utils::DecodeUtf8(std::get<std::string>(iter->second));

		iter = json.KeyValues.find("stationindex");
		if (iter != json.KeyValues.end() && iter->second.index() == 2)
			ini.VstDebug.StationIndex = static_cast<unsigned int>(std::get<unsigned long>(iter->second));

		iter = json.KeyValues.find("pluginindex");
		if (iter != json.KeyValues.end() && iter->second.index() == 2)
			ini.VstDebug.PluginIndex = static_cast<unsigned int>(std::get<unsigned long>(iter->second));
	}
}

using namespace io;
using audio::BehaviourParams;

const std::string InitFile::DefaultJson(std::string roamingPath)
{
	const auto rigPath = EscapeJsonString(roamingPath + "\\default.rig");
	const auto jamPath = EscapeJsonString(roamingPath + "\\default.jam");
	const auto logPath = EscapeJsonString(roamingPath + "\\vst-diagnostic.log");
	return "{\"rig\":\"" + rigPath + "\",\"jam\":\"" + jamPath
		+ "\",\"jamload\":1,\"rigload\":0,\"win\":[-0,0,1400,1000],"
		+ "\"vstdebug\":{\"autoopen\":false,\"logtofile\":false,\"logpath\":\""
		+ logPath + "\",\"stationindex\":0,\"pluginindex\":0}}";
}

const void InitFile::SetWinParams(InitFile& ini, const Json::JsonArray& array)
{
	auto vec = std::visit([](auto&& value) -> std::vector<long>
		{
			std::vector<long> result;
			using ValueType = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<ValueType, std::vector<long>>)
			{
				auto res = static_cast<std::vector<long>>(value);
				if (res.size() > 1)
				{
					result.push_back(res[0]);
					result.push_back(res[1]);

					if (res.size() > 3)
					{
						result.push_back(res[2]);
						result.push_back(res[3]);
					}
				}
			}
			else if constexpr (std::is_same_v<ValueType, std::vector<unsigned long>>)
			{
				auto res = static_cast<std::vector<unsigned long>>(value);
				if (res.size() > 1)
				{
					result.push_back(res[0]);
					result.push_back(res[1]);

					if (res.size() > 3)
					{
						result.push_back(res[2]);
						result.push_back(res[3]);
					}
				}
			}
			return result;
		}, array.Array);

	if (vec.size() > 1)
		ini.WinPos = { vec[0], vec[1] };

	if (vec.size() > 3)
		ini.WinSize = { (unsigned int)vec[2], (unsigned int)vec[3] };
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
			SetWinParams(ini, jsonArray);
		}
	}

	iter = iniParams.KeyValues.find("vstdebug");
	if (iter != iniParams.KeyValues.end())
	{
		if (iter->second.index() == 6)
			ParseVstDebug(ini, std::get<Json::JsonPart>(iter->second));
	}

	return ini;
}

bool InitFile::ToStream(InitFile ini, std::stringstream& ss)
{
	ss << "Jam: " << utils::EncodeUtf8(ini.Jam) << std::endl;
	ss << "JamLoadType: " << ini.JamLoadType << std::endl;
	ss << "Rig: " << utils::EncodeUtf8(ini.Rig) << std::endl;
	ss << "RigLoadType: " << ini.RigLoadType << std::endl;
	ss << "VstDebug.AutoOpenEditor: " << ini.VstDebug.AutoOpenEditor << std::endl;
	ss << "VstDebug.LogToFile: " << ini.VstDebug.LogToFile << std::endl;
	ss << "VstDebug.LogPath: " << utils::EncodeUtf8(ini.VstDebug.LogPath) << std::endl;
	ss << "VstDebug.StationIndex: " << ini.VstDebug.StationIndex << std::endl;
	ss << "VstDebug.PluginIndex: " << ini.VstDebug.PluginIndex << std::endl;

	ss << "WinPos: " << ini.WinPos.X << "," << ini.WinPos.Y << std::endl;
	ss << "WinSize: " << ini.WinSize.Width << "," << ini.WinSize.Height << std::endl;

	return true;
}
