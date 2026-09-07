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
	Json::JsonPart json;
	json.KeyValues["rig"] = roamingPath + "\\default.rig";
	json.KeyValues["jam"] = roamingPath + "\\default.jam";
	json.KeyValues["jamload"] = 1l;
	json.KeyValues["rigload"] = 0l;
	json.KeyValues["win"] = Json::JsonArray{ 4u, std::vector<long>{ 0l, 0l, 1400l, 1000l } };

	std::stringstream stream;
	Json::ToStream(json, stream);
	return stream.str();
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

	iter = iniParams.KeyValues.find("logging");
	if (iter != iniParams.KeyValues.end())
	{
		if (iniParams.KeyValues["logging"].index() == 6)
		{
			auto loggingJson = std::get<Json::JsonPart>(iniParams.KeyValues["logging"]);

			auto midiIter = loggingJson.KeyValues.find("midi");
			if (midiIter != loggingJson.KeyValues.end())
			{
				if (loggingJson.KeyValues["midi"].index() == 4)
					ini.Logging.Midi = std::get<std::string>(loggingJson.KeyValues["midi"]);
			}

			auto audioIter = loggingJson.KeyValues.find("audio");
			if (audioIter != loggingJson.KeyValues.end())
			{
				if (loggingJson.KeyValues["audio"].index() == 4)
					ini.Logging.Audio = std::get<std::string>(loggingJson.KeyValues["audio"]);
			}

			auto eventIter = loggingJson.KeyValues.find("event");
			if (eventIter != loggingJson.KeyValues.end())
			{
				if (loggingJson.KeyValues["event"].index() == 4)
					ini.Logging.Event = std::get<std::string>(loggingJson.KeyValues["event"]);
			}

			auto uiIter = loggingJson.KeyValues.find("ui");
			if (uiIter != loggingJson.KeyValues.end())
			{
				if (loggingJson.KeyValues["ui"].index() == 4)
					ini.Logging.Ui = std::get<std::string>(loggingJson.KeyValues["ui"]);
			}
		}
	}

	return ini;
}

bool InitFile::ToStream(InitFile ini, std::stringstream& ss)
{
	Json::JsonPart root;
	root.KeyValues["jam"] = utils::EncodeUtf8(ini.Jam);
	root.KeyValues["jamload"] = static_cast<unsigned long>(ini.JamLoadType);
	root.KeyValues["rig"] = utils::EncodeUtf8(ini.Rig);
	root.KeyValues["rigload"] = static_cast<unsigned long>(ini.RigLoadType);
	root.KeyValues["win"] = Json::JsonArray{ 4u, std::vector<long>{ ini.WinPos.X, ini.WinPos.Y, static_cast<long>(ini.WinSize.Width), static_cast<long>(ini.WinSize.Height) } };

	Json::JsonPart logging;
	logging.KeyValues["midi"] = ini.Logging.Midi;
	logging.KeyValues["audio"] = ini.Logging.Audio;
	logging.KeyValues["event"] = ini.Logging.Event;
	logging.KeyValues["ui"] = ini.Logging.Ui;
	root.KeyValues["logging"] = logging;

	return Json::ToStream(root, ss);
}
