///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "RigFile.h"

using namespace io;

const std::string RigFile::DefaultJson = "{\"name\":\"default\",\"user\":{\"audio\":{\"name\":\"default\",\"bufsize\":512,\"inlatency\":4600,\"outlatency\":6000,\"numchannelsin\":2,\"numchannelsout\":2},\"loop\":{\"fadeTime\":400},\"trigger\":{\"preDelay\":400,\"debounceSamps\":280}},\"triggers\":[{\"name\":\"Trig1\",\"stationtype\":0,\"pairs\":[{\"activatedown\":49,\"activateup\":49,\"ditchdown\":50,\"ditchup\":50}],\"input\":[0,1]}"; 

std::optional<RigFile> RigFile::FromStream(std::stringstream ss)
{
	auto root = Json::FromStream(std::move(ss));

	if (!root.has_value())
		return std::nullopt;

	if (root.value().index() != 6)
		return std::nullopt;

	auto rigParams = std::get<Json::JsonPart>(root.value());

	if (rigParams.KeyValues.find("name") == rigParams.KeyValues.end())
		return std::nullopt;

	if (rigParams.KeyValues["name"].index() != 4)
		return std::nullopt;

	RigFile rig;
	rig.Version = VERSION_V;
	rig.Name = std::get<std::string>(rigParams.KeyValues["name"]);

	auto gotUser = false;
	auto iter = rigParams.KeyValues.find("user");
	if (iter != rigParams.KeyValues.end())
	{
		if (rigParams.KeyValues["user"].index() == 6)
		{
			auto userJson = std::get<Json::JsonPart>(rigParams.KeyValues["user"]);
			auto userOpt = UserConfig::FromJson(userJson);

			if (userOpt.has_value())
			{
				rig.User = userOpt.value();
				gotUser = true;
			}
		}
	}

	if (!gotUser)
		return std::nullopt;

	iter = rigParams.KeyValues.find("triggers");
	if (iter != rigParams.KeyValues.end())
	{
		if (rigParams.KeyValues["triggers"].index() == 5)
		{
			auto triggerArr = std::get<Json::JsonArray>(rigParams.KeyValues["triggers"]);
			if (triggerArr.Array.index() == 5)
			{
				auto triggers = std::get<std::vector<Json::JsonPart>>(triggerArr.Array);

				for (auto triggerJson : triggers)
				{
					auto triggerOpt = Trigger::FromJson(triggerJson);
					if (triggerOpt.has_value())
						rig.Triggers.push_back(triggerOpt.value());
				}
			}
		}
	}

	return rig;
}

bool RigFile::ToStream(RigFile rig, std::stringstream& ss)
{
	ss << "Version: " << rig.Version << std::endl;
	ss << "Name: " << rig.Name << std::endl;

	ss << "=== Audio ===" << std::endl;
	ss << "Audio Name: " << rig.User.Audio.Name << std::endl;
	ss << "SampleRate: " << rig.User.Audio.SampleRate << std::endl;
	ss << "NumBuffers: " << rig.User.Audio.NumBuffers << std::endl;
	ss << "NumChannelsIn: " << rig.User.Audio.NumChannelsIn << std::endl;
	ss << "NumChannelsOut: " << rig.User.Audio.NumChannelsOut << std::endl;
	ss << "LatencyIn: " << rig.User.Audio.LatencyIn << std::endl;
	ss << "LatencyOut: " << rig.User.Audio.LatencyOut << std::endl;

	ss << "=== Loop ===" << std::endl;
	ss << "FadeSamps: " << rig.User.Loop.FadeSamps << std::endl;

	ss << "=== Trigger ===" << std::endl;
	ss << "DebounceSamps: " << rig.User.Trigger.DebounceSamps << std::endl;
	ss << "PreDelay: " << rig.User.Trigger.PreDelay << std::endl;

	return true;
}

std::optional<RigFile::TriggerPair> RigFile::TriggerPair::FromJson(Json::JsonPart json)
{
	unsigned int activateDown = 0;
	unsigned int activateUp = 0;
	unsigned int ditchDown = 0;
	unsigned int ditchUp = 0;

	auto iter = json.KeyValues.find("activatedown");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["activatedown"].index() == 2)
			activateDown = std::get<unsigned long>(json.KeyValues["activatedown"]);
	}

	iter = json.KeyValues.find("activateup");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["activateup"].index() == 2)
			activateUp = std::get<unsigned long>(json.KeyValues["activateup"]);
	}

	iter = json.KeyValues.find("ditchdown");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["ditchdown"].index() == 2)
			ditchDown = std::get<unsigned long>(json.KeyValues["ditchdown"]);
	}

	iter = json.KeyValues.find("ditchup");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["ditchup"].index() == 2)
			ditchUp = std::get<unsigned long>(json.KeyValues["ditchup"]);
	}

	if ((0 == activateDown) && (0 == activateUp))
		return std::nullopt;

	if ((0 == ditchDown) && (0 == ditchUp))
		return std::nullopt;

	TriggerPair pair;
	pair.ActivateDown = activateDown;
	pair.ActivateUp = activateUp;
	pair.DitchDown = ditchDown;
	pair.DitchUp = ditchUp;
	return pair;
}

std::optional<RigFile::Trigger> RigFile::Trigger::FromJson(Json::JsonPart json)
{
	std::string name;
	unsigned int stationType = 0;
	std::vector<TriggerPair> pairs;
	std::vector<unsigned int> inputChannels;

	auto iter = json.KeyValues.find("name");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["name"].index() == 4)
			name = std::get<std::string>(json.KeyValues["name"]);
	}

	iter = json.KeyValues.find("stationtype");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["stationtype"].index() == 2)
			stationType = std::get<unsigned long>(json.KeyValues["stationtype"]);
	}

	iter = json.KeyValues.find("pairs");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["pairs"].index() == 5)
		{
			auto jsonArray = std::get<Json::JsonArray>(json.KeyValues["pairs"]);
			
			if (jsonArray.Array.index() == 5)
			{
				auto pairArray = std::get<std::vector<Json::JsonPart>>(jsonArray.Array);
				for (auto pairJson : pairArray)
				{
					auto pair = TriggerPair::FromJson(pairJson);
					if (pair.has_value())
						pairs.push_back(pair.value());
				}
			}
		}
	}

	iter = json.KeyValues.find("input");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["input"].index() == 5)
		{
			auto jsonArray = std::get<Json::JsonArray>(json.KeyValues["input"]);

			if (jsonArray.Array.index() == 2)
			{
				auto inChans = std::get<std::vector<unsigned long>>(jsonArray.Array);
				for (auto chan : inChans)
				{
					if (inputChannels.end() == std::find(inputChannels.begin(), inputChannels.end(), chan))
						inputChannels.push_back((unsigned int)chan);
				}
			}
		}
	}

	if (pairs.empty() || name.empty())
		return std::nullopt;

	Trigger trigger;
	trigger.Name = name;
	trigger.StationType = stationType;
	trigger.TriggerPairs = pairs;
	trigger.InputChannels = inputChannels;
	return trigger;
}
