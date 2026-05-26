///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "RigFile.h"

#include <algorithm>

namespace
{
	std::optional<std::string> GetJsonString(const io::Json::JsonPart& json, const std::string& key)
	{
		auto iter = json.KeyValues.find(key);
		if ((iter == json.KeyValues.end()) || (iter->second.index() != 4))
			return std::nullopt;

		return std::get<std::string>(iter->second);
	}

	std::optional<unsigned int> GetJsonUnsigned(const io::Json::JsonPart& json, const std::string& key)
	{
		auto iter = json.KeyValues.find(key);
		if ((iter == json.KeyValues.end()) || (iter->second.index() != 2))
			return std::nullopt;

		return static_cast<unsigned int>(std::get<unsigned long>(iter->second));
	}

	std::string NormaliseJsonStringArrayValue(std::string value)
	{
		if ((value.size() >= 2u) && (value.front() == '"') && (value.back() == '"'))
			value = value.substr(1u, value.size() - 2u);

		return value;
	}
}

using namespace io;

const std::string RigFile::DefaultJson = "{\"name\":\"default\",\"user\":{\"audio\":{\"name\":\"default\",\"bufsize\":512,\"inlatency\":4600,\"outlatency\":6000,\"numchannelsin\":2,\"numchannelsout\":2},\"midi\":{\"devices\":[{\"name\":\"default\",\"enabled\":true}]},\"loop\":{\"fadeSamps\":800,\"seedGrainMinMs\":400,\"seedGrainTargetMaxMs\":3000,\"seedBpmMin\":80,\"seedQuantisation\":\"power\"},\"trigger\":{\"preDelay\":400,\"debounceSamps\":280}},\"triggers\":[{\"name\":\"Trig1\",\"stationtype\":0,\"pairs\":[{\"activatedown\":49,\"activateup\":49,\"ditchdown\":50,\"ditchup\":50}],\"input\":[0,1]}]}";

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

	ss << "=== MIDI ===" << std::endl;
	ss << "Name: " << rig.User.Midi.Name << std::endl;
	ss << "Enabled: " << rig.User.Midi.Enabled << std::endl;
	ss << "Devices: " << rig.User.Midi.Devices.size() << std::endl;

	return true;
}

std::optional<RigFile::TriggerPair> RigFile::TriggerPair::FromJson(Json::JsonPart json)
{
	unsigned int activateDown = 0;
	unsigned int activateUp = 0;
	unsigned int ditchDown = 0;
	unsigned int ditchUp = 0;
	auto hasActivateDown = false;
	auto hasActivateUp = false;
	auto hasDitchDown = false;
	auto hasDitchUp = false;
	auto source = SOURCE_KEYBOARD;
	auto device = std::string();

	auto iter = json.KeyValues.find("source");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["source"].index() == 4)
		{
			auto sourceText = std::get<std::string>(json.KeyValues["source"]);
			if (sourceText == "serial")
			{
				source = SOURCE_SERIAL;
				device = "default";
			}
		}
	}

	iter = json.KeyValues.find("device");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["device"].index() == 4)
		{
			auto parsedDevice = std::get<std::string>(json.KeyValues["device"]);
			if (!parsedDevice.empty())
				device = parsedDevice;
			else if (source == SOURCE_SERIAL)
				device = "default";
		}
	}

	iter = json.KeyValues.find("activatedown");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["activatedown"].index() == 2)
		{
			activateDown = std::get<unsigned long>(json.KeyValues["activatedown"]);
			hasActivateDown = true;
		}
	}

	iter = json.KeyValues.find("activateup");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["activateup"].index() == 2)
		{
			activateUp = std::get<unsigned long>(json.KeyValues["activateup"]);
			hasActivateUp = true;
		}
	}

	iter = json.KeyValues.find("ditchdown");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["ditchdown"].index() == 2)
		{
			ditchDown = std::get<unsigned long>(json.KeyValues["ditchdown"]);
			hasDitchDown = true;
		}
	}

	iter = json.KeyValues.find("ditchup");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["ditchup"].index() == 2)
		{
			ditchUp = std::get<unsigned long>(json.KeyValues["ditchup"]);
			hasDitchUp = true;
		}
	}

	if (!hasActivateDown && !hasActivateUp)
		return std::nullopt;

	if (!hasDitchDown && !hasDitchUp)
		return std::nullopt;

	TriggerPair pair;
	pair.ActivateDown = activateDown;
	pair.ActivateUp = activateUp;
	pair.DitchDown = ditchDown;
	pair.DitchUp = ditchUp;
	pair.Source = source;
	pair.Device = device;
	return pair;
}

std::optional<RigFile::Trigger> RigFile::Trigger::FromJson(Json::JsonPart json)
{
	std::string name;
	unsigned int stationType = 0;
	std::vector<TriggerPair> pairs;
	std::vector<unsigned int> inputChannels;
	std::vector<unsigned int> midiInputChannels;
	std::vector<std::string> midiInputDevices;
	std::optional<MidiTriggerBinding> midiTrigger;

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

	iter = json.KeyValues.find("midiinput");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["midiinput"].index() == 5)
		{
			auto jsonArray = std::get<Json::JsonArray>(json.KeyValues["midiinput"]);

			if (jsonArray.Array.index() == 2)
			{
				auto inChans = std::get<std::vector<unsigned long>>(jsonArray.Array);
				for (auto chan : inChans)
				{
					if ((chan < 1ul) || (chan > 16ul))
						continue;

					const auto zeroBasedChan = static_cast<unsigned int>(chan - 1ul);
					if (midiInputChannels.end() == std::find(midiInputChannels.begin(), midiInputChannels.end(), zeroBasedChan))
						midiInputChannels.push_back(zeroBasedChan);
				}
			}
		}
	}

	iter = json.KeyValues.find("midiinputdevices");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["midiinputdevices"].index() == 5)
		{
			auto jsonArray = std::get<Json::JsonArray>(json.KeyValues["midiinputdevices"]);

			if (jsonArray.Array.index() == 4)
			{
				auto devices = std::get<std::vector<std::string>>(jsonArray.Array);
				for (auto device : devices)
				{
					device = NormaliseJsonStringArrayValue(std::move(device));

					if (device.empty())
						continue;

					if (midiInputDevices.end() == std::find(midiInputDevices.begin(), midiInputDevices.end(), device))
						midiInputDevices.push_back(device);
				}
			}
		}
	}

	iter = json.KeyValues.find("trigger");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["trigger"].index() == 6)
		{
			auto triggerJson = std::get<Json::JsonPart>(json.KeyValues["trigger"]);
			midiTrigger = MidiTriggerBinding::FromJson(triggerJson);
		}
	}

	if ((pairs.empty() && !midiTrigger.has_value()) || name.empty())
		return std::nullopt;

	Trigger trigger;
	trigger.Name = name;
	trigger.StationType = stationType;
	trigger.TriggerPairs = pairs;
	trigger.InputChannels = inputChannels;
	trigger.MidiInputChannels = midiInputChannels;
	trigger.MidiInputDevices = midiInputDevices;
	trigger.MidiTrigger = midiTrigger;
	return trigger;
}

std::optional<RigFile::Trigger::MidiTriggerBindingSpec> RigFile::Trigger::MidiTriggerBindingSpec::FromJson(Json::JsonPart json)
{
	auto kind = GetJsonString(json, "kind");
	auto id = GetJsonUnsigned(json, "id");
	if (!kind.has_value() || !id.has_value() || (id.value() > 127u))
		return std::nullopt;

	MidiTriggerBindingSpec binding{};
	binding.Id = id.value();
	binding.Channel = 0u;
	binding.State = 1u;
	binding.MatchAnyChannel = true;

	if ((0 == kind.value().compare("note")) ||
		(0 == kind.value().compare("noteon")) ||
		(0 == kind.value().compare("note-on")) ||
		(0 == kind.value().compare("note on")))
		binding.Kind = NOTE;
	else if ((0 == kind.value().compare("noteoff")) ||
		(0 == kind.value().compare("note-off")) ||
		(0 == kind.value().compare("note off")))
	{
		binding.Kind = NOTE;
		binding.State = 0u;
	}
	else if (0 == kind.value().compare("cc"))
		binding.Kind = CC;
	else
		return std::nullopt;

	auto channelIter = json.KeyValues.find("channel");
	if (channelIter != json.KeyValues.end())
	{
		auto channel = GetJsonUnsigned(json, "channel");
		if (!channel.has_value() || (channel.value() < 1u) || (channel.value() > 16u))
			return std::nullopt;

		binding.Channel = channel.value() - 1u;
		binding.MatchAnyChannel = false;
	}

	return binding;
}

std::optional<RigFile::Trigger::MidiTriggerBinding> RigFile::Trigger::MidiTriggerBinding::FromJson(Json::JsonPart json)
{
	auto type = GetJsonString(json, "type");
	if (!type.has_value() || (0 != type.value().compare("midi")))
		return std::nullopt;

	auto activateIter = json.KeyValues.find("activate");
	auto ditchIter = json.KeyValues.find("ditch");
	if ((activateIter == json.KeyValues.end()) || (ditchIter == json.KeyValues.end()))
		return std::nullopt;
	if ((activateIter->second.index() != 6) || (ditchIter->second.index() != 6))
		return std::nullopt;

	auto activate = MidiTriggerBindingSpec::FromJson(std::get<Json::JsonPart>(activateIter->second));
	auto ditch = MidiTriggerBindingSpec::FromJson(std::get<Json::JsonPart>(ditchIter->second));
	if (!activate.has_value() || !ditch.has_value())
		return std::nullopt;

	MidiTriggerBinding binding{};
	auto device = GetJsonString(json, "device");
	binding.Device = device.has_value() ? device.value() : "default";
	binding.Activate = activate.value();
	binding.Ditch = ditch.value();
	return binding;
}
