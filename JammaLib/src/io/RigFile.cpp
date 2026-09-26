///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "RigFile.h"
#include "../utils/StringUtils.h"

#include <algorithm>
#include <unordered_set>

using namespace io;

const std::string RigFile::DefaultJson = "{\"name\":\"default\",\"user\":{\"audio\":{\"name\":\"default\",\"bufsize\":512,\"inlatency\":4600,\"outlatency\":6000,\"numchannelsin\":2,\"numchannelsout\":2},\"midi\":{\"devices\":[{\"name\":\"default\",\"enabled\":true}],\"channelOverrideTriggers\":false,\"channelOverrideLive\":true},\"loop\":{\"fadeSamps\":800,\"seedGrainMinMs\":400,\"seedGrainTargetMaxMs\":3000,\"seedBpmMin\":80,\"seedQuantisation\":\"power\"},\"trigger\":{\"preDelay\":400,\"debounceSamps\":280}},\"triggers\":[{\"name\":\"Trig1\",\"stationtype\":0,\"midiinputmode\":\"none\",\"midiinputdevices\":[],\"pairs\":[{\"activatedown\":49,\"activateup\":49,\"ditchdown\":50,\"ditchup\":50}],\"input\":[0,1]}]}";

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
	rig.Version = CurrentVersion;
	auto version = rigParams.KeyValues.find("version");
	if (version != rigParams.KeyValues.end())
	{
		if (version->second.index() != 4)
			return std::nullopt;
		rig.Version = std::get<std::string>(version->second);
		if (rig.Version.empty())
			return std::nullopt;
	}
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
	ss << "Devices: " << rig.User.Midi.Devices.size() << std::endl;
	for (const auto& device : rig.User.Midi.Devices)
	{
		ss << "Device: " << device.Name
			<< " Enabled: " << device.Enabled << std::endl;
	}
	ss << "Channel override triggers: " << rig.User.Midi.ChannelOverrideTriggers << std::endl;
	ss << "Channel override live: " << rig.User.Midi.ChannelOverrideLive << std::endl;

	return true;
}

bool RigFile::ToJsonStream(const RigFile& rig, std::stringstream& ss)
{
	auto string = [](const std::string& value) {
		std::string out = "\"";
		const char* digits = "0123456789abcdef";
		for (const unsigned char character : value)
		{
			switch (character)
			{
			case '\"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (character < 0x20u)
				{
					out += "\\u00";
					out += digits[(character >> 4u) & 0x0fu];
					out += digits[character & 0x0fu];
				}
				else out += static_cast<char>(character);
			}
		}
		return out + "\"";
	};
	auto key = [&](const char* name) { return string(name) + ":"; };
	auto boolean = [](bool value) { return value ? "true" : "false"; };
	auto midiMode = [](Trigger::MidiInputMode mode) {
		switch (mode)
		{
		case Trigger::MidiInputMode::None: return "none";
		case Trigger::MidiInputMode::Selected: return "selected";
		default: return "none";
		}
	};
	auto midiSpec = [&](const Trigger::MidiTriggerBindingSpec& spec) {
		std::string kind = spec.Kind == NOTE ? (spec.State == 0u ? "noteoff" : "note") : "cc";
		std::string out = "{" + key("kind") + string(kind);
		if (!spec.MatchAnyChannel)
			out += "," + key("channel") + std::to_string(spec.Channel + 1u);
		out += "," + key("id") + std::to_string(spec.Id) + "}";
		return out;
	};

	ss << "{" << key("version") << string(rig.Version) << "," << key("name") << string(rig.Name) << "," << key("user") << "{";
	const auto& user = rig.User;
	ss << key("audio") << "{" << key("name") << string(user.Audio.Name)
		<< "," << key("samplerate") << user.Audio.SampleRate
		<< "," << key("bufsize") << user.Audio.BufSize
		<< "," << key("inlatency") << user.Audio.LatencyIn
		<< "," << key("outlatency") << user.Audio.LatencyOut
		<< "," << key("numbuffers") << user.Audio.NumBuffers
		<< "," << key("numchannelsin") << user.Audio.NumChannelsIn
		<< "," << key("numchannelsout") << user.Audio.NumChannelsOut << "},";
	ss << key("loop") << "{" << key("fadeSamps") << user.Loop.FadeSamps
		<< "," << key("seedGrainMinMs") << user.Loop.SeedGrainMinMs
		<< "," << key("seedGrainTargetMaxMs") << user.Loop.SeedGrainTargetMaxMs
		<< "," << key("seedBpmMin") << user.Loop.SeedBpmMin
		<< "," << key("seedQuantisation") << string(user.Loop.SeedUsesPowers ? "power" : "multiple") << "},";
	ss << key("trigger") << "{" << key("preDelay") << user.Trigger.PreDelay
		<< "," << key("debounceSamps") << user.Trigger.DebounceSamps << "},";
	ss << key("midi") << "{" << key("devices") << "[";
	for (size_t i = 0; i < user.Midi.Devices.size(); ++i)
	{
		if (i) ss << ",";
		ss << "{" << key("name") << string(user.Midi.Devices[i].Name)
			<< "," << key("enabled") << boolean(user.Midi.Devices[i].Enabled) << "}";
	}
	ss << "]," << key("channelOverrideTriggers") << boolean(user.Midi.ChannelOverrideTriggers)
		<< "," << key("channelOverrideLive") << boolean(user.Midi.ChannelOverrideLive) << "},";
	ss << key("serial") << "{" << key("devices") << "[";
	for (size_t i = 0; i < user.Serial.Devices.size(); ++i)
	{
		if (i) ss << ",";
		const auto& device = user.Serial.Devices[i];
		ss << "{" << key("name") << string(device.Name) << "," << key("port") << string(device.Port)
			<< "," << key("baudrate") << device.BaudRate << "," << key("enabled") << boolean(device.Enabled) << "}";
	}
	ss << "]}}," << key("triggers") << "[";
	for (size_t i = 0; i < rig.Triggers.size(); ++i)
	{
		if (i) ss << ",";
		const auto& trigger = rig.Triggers[i];
		ss << "{" << key("id") << string(trigger.Id)
			<< "," << key("name") << string(trigger.Name)
			<< "," << key("stationtype") << trigger.StationType;
		if (trigger.StationTarget.has_value())
			ss << "," << key("stationtarget") << string(trigger.StationTarget.value());
		ss << "," << key("midiinputmode") << string(midiMode(trigger.MidiInputs));
		ss << "," << key("pairs") << "[";
		for (size_t pairIndex = 0; pairIndex < trigger.TriggerPairs.size(); ++pairIndex)
		{
			if (pairIndex) ss << ",";
			const auto& pair = trigger.TriggerPairs[pairIndex];
			ss << "{" << key("source") << string(pair.Source == TriggerPair::SOURCE_SERIAL ? "serial" : "keyboard")
				<< "," << key("device") << string(pair.Device)
				<< "," << key("activatedown") << pair.ActivateDown << "," << key("activateup") << pair.ActivateUp
				<< "," << key("ditchdown") << pair.DitchDown << "," << key("ditchup") << pair.DitchUp << "}";
		}
		ss << "]," << key("input") << "[";
		for (size_t channelIndex = 0; channelIndex < trigger.InputChannels.size(); ++channelIndex)
		{
			if (channelIndex) ss << ",";
			ss << trigger.InputChannels[channelIndex];
		}
		ss << "]," << key("midiinputdevices") << "[";
		for (size_t deviceIndex = 0; deviceIndex < trigger.MidiInputDevices.size(); ++deviceIndex)
		{
			if (deviceIndex) ss << ",";
			ss << string(trigger.MidiInputDevices[deviceIndex]);
		}
		ss << "]";
		if (trigger.MidiTrigger.has_value())
		{
			const auto& binding = trigger.MidiTrigger.value();
			ss << "," << key("trigger") << "{" << key("type") << string("midi")
				<< "," << key("device") << string(binding.Device)
				<< "," << key("activate") << midiSpec(binding.Activate)
				<< "," << key("ditch") << midiSpec(binding.Ditch) << "}";
		}
		ss << "}";
	}
	ss << "]}";
	return ss.good();
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
	std::string id;
	std::string name;
	unsigned int stationType = 0;
	std::vector<TriggerPair> pairs;
	std::vector<unsigned int> inputChannels;
	std::vector<std::string> midiInputDevices;
	std::optional<std::string> stationTarget;
	MidiInputMode midiInputs = MidiInputMode::None;
	std::optional<MidiTriggerBinding> midiTrigger;

	auto iter = json.KeyValues.find("id");
	if (iter != json.KeyValues.end())
	{
		if (iter->second.index() != 4)
			return std::nullopt;
		id = std::get<std::string>(iter->second);
	}

	iter = json.KeyValues.find("name");
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
					device = Json::NormaliseStringArrayValue(std::move(device));

					if (device.empty())
						continue;
					if (device == "*")
						continue;

					if (midiInputDevices.end() == std::find(midiInputDevices.begin(), midiInputDevices.end(), device))
						midiInputDevices.push_back(device);
				}
			}
		}
	}

	iter = json.KeyValues.find("stationtarget");
	if (iter != json.KeyValues.end())
	{
		if (iter->second.index() != 4)
			return std::nullopt;
		stationTarget = std::get<std::string>(iter->second);
	}

	iter = json.KeyValues.find("midiinputmode");
	if (iter != json.KeyValues.end())
	{
		if (iter->second.index() != 4)
			return std::nullopt;
		const auto& mode = std::get<std::string>(iter->second);
		if (mode == "none")
			midiInputs = MidiInputMode::None;
		else if (mode == "any")
			// Legacy wildcard routes are retained as disabled rather than broaden capture.
			midiInputs = MidiInputMode::None;
		else if (mode == "selected")
			midiInputs = MidiInputMode::Selected;
		else
			return std::nullopt;
	}
	else if (!midiInputDevices.empty())
		midiInputs = MidiInputMode::Selected;

	if (midiInputs == MidiInputMode::Selected && midiInputDevices.empty())
		return std::nullopt;
	if (midiInputs != MidiInputMode::Selected && !midiInputDevices.empty())
		return std::nullopt;

	iter = json.KeyValues.find("trigger");
	if (iter != json.KeyValues.end())
	{
		if (iter->second.index() != 6)
			return std::nullopt;

		midiTrigger = MidiTriggerBinding::FromJson(std::get<Json::JsonPart>(iter->second));
		if (!midiTrigger.has_value())
			return std::nullopt;
	}

	if (name.empty())
		return std::nullopt;

	Trigger trigger;
	trigger.Id = id;
	trigger.Name = name;
	trigger.StationType = stationType;
	trigger.TriggerPairs = pairs;
	trigger.InputChannels = inputChannels;
	trigger.MidiInputDevices = midiInputDevices;
	trigger.StationTarget = stationTarget;
	trigger.MidiInputs = midiInputs;
	trigger.MidiTrigger = midiTrigger;
	return trigger;
}

std::optional<RigFile::Trigger::MidiTriggerBindingSpec> RigFile::Trigger::MidiTriggerBindingSpec::FromJson(Json::JsonPart json)
{
	auto kind = Json::GetString(json, "kind");
	auto id = Json::GetUnsigned(json, "id");
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
		auto channel = Json::GetUnsigned(json, "channel");
		if (!channel.has_value() || (channel.value() < 1u) || (channel.value() > 16u))
			return std::nullopt;

		binding.Channel = channel.value() - 1u;
		binding.MatchAnyChannel = false;
	}

	return binding;
}

std::optional<RigFile::Trigger::MidiTriggerBinding> RigFile::Trigger::MidiTriggerBinding::FromJson(Json::JsonPart json)
{
	auto type = Json::GetString(json, "type");
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
	auto device = Json::GetString(json, "device");
	binding.Device = device.has_value() ? device.value() : "default";
	binding.Activate = activate.value();
	binding.Ditch = ditch.value();
	return binding;
}

RigFileRouting::Resolution RigFileRouting::Resolve(const RigFile& rig,
	const std::vector<JamFile::Station>& stations,
	unsigned int availableAdcChannels,
	const std::vector<std::string>& availableMidiDevices)
{
	Resolution result{ rig };
	std::unordered_set<std::string> triggerIds;
	triggerIds.reserve(rig.Triggers.size());
	for (const auto& trigger : rig.Triggers)
	{
		if (!trigger.Id.empty() && !triggerIds.insert(trigger.Id).second)
		{
			result.IsValid = false;
			return result;
		}
	}
	result.Triggers.reserve(rig.Triggers.size());
	for (size_t triggerIndex = 0; triggerIndex < rig.Triggers.size(); ++triggerIndex)
	{
		const auto& trigger = rig.Triggers[triggerIndex];
		TriggerResolution resolved;
		resolved.TriggerIndex = triggerIndex;
		resolved.TriggerName = trigger.Name;

		auto& candidateTrigger = result.CandidateRig.Triggers[triggerIndex];
		if (candidateTrigger.Id.empty())
		{
			do candidateTrigger.Id = utils::GetGuid();
			while (candidateTrigger.Id.empty() || !triggerIds.insert(candidateTrigger.Id).second);
			result.RequiresSave = true;
		}
		candidateTrigger.InputChannels.clear();
		for (const auto channel : trigger.InputChannels)
		{
			if (std::find(candidateTrigger.InputChannels.begin(), candidateTrigger.InputChannels.end(), channel) != candidateTrigger.InputChannels.end())
				continue;
			candidateTrigger.InputChannels.push_back(channel);
			resolved.Sources.push_back(Source{ SourceKind::Adc, channel, {}, channel < availableAdcChannels });
		}
		if (trigger.MidiInputs == RigFile::Trigger::MidiInputMode::Selected)
		{
			candidateTrigger.MidiInputDevices.clear();
			for (const auto& device : trigger.MidiInputDevices)
			{
				if (device.empty() || std::find(candidateTrigger.MidiInputDevices.begin(), candidateTrigger.MidiInputDevices.end(), device) != candidateTrigger.MidiInputDevices.end())
					continue;
				candidateTrigger.MidiInputDevices.push_back(device);
				resolved.Sources.push_back(Source{ SourceKind::Midi, 0u, device,
					std::find(availableMidiDevices.begin(), availableMidiDevices.end(), device) != availableMidiDevices.end() });
			}
		}

		if (!trigger.StationTarget.has_value())
		{
			if (triggerIndex < stations.size())
			{
				const auto& legacyName = stations[triggerIndex].Name;
				resolved.TargetName = legacyName;
				const auto matchCount = std::count_if(stations.begin(), stations.end(), [&](const JamFile::Station& station) {
					return station.Name == legacyName;
				});
				if (matchCount == 1u)
				{
					resolved.StationIndex = triggerIndex;
					resolved.Reason = Warning::LegacyStationTargetMigrated;
					result.CandidateRig.Triggers[triggerIndex].StationTarget = legacyName;
					result.RequiresSave = true;
				}
				else
					resolved.Reason = Warning::TargetAmbiguous;
			}
			else
				resolved.Reason = Warning::TargetMissing;
		}
		else if (trigger.StationTarget->empty())
			resolved.TargetName = trigger.StationTarget;
		else
		{
			resolved.TargetName = trigger.StationTarget;
			std::optional<size_t> match;
			for (size_t stationIndex = 0; stationIndex < stations.size(); ++stationIndex)
			{
				if (stations[stationIndex].Name != trigger.StationTarget.value())
					continue;
				if (match.has_value())
				{
					match.reset();
					resolved.Reason = Warning::TargetAmbiguous;
					break;
				}
				match = stationIndex;
			}
			if (resolved.Reason != Warning::TargetAmbiguous)
			{
				resolved.StationIndex = match;
				if (!match.has_value())
					resolved.Reason = Warning::TargetMissing;
			}
		}
		result.Triggers.push_back(std::move(resolved));
	}
	return result;
}

std::string RigFileRouting::NextTriggerName(const RigFile& rig)
{
	for (unsigned int suffix = 1u;; ++suffix)
	{
		const auto candidate = "Trigger-" + std::to_string(suffix);
		const auto found = std::find_if(rig.Triggers.begin(), rig.Triggers.end(), [&](const RigFile::Trigger& trigger) {
			return trigger.Name == candidate;
		});
		if (found == rig.Triggers.end())
			return candidate;
	}
}

RigFile RigFileRouting::WithUnboundTrigger(const RigFile& rig)
{
	auto candidate = rig;
	RigFile::Trigger trigger{};
	do trigger.Id = utils::GetGuid();
	while (trigger.Id.empty() || std::any_of(rig.Triggers.begin(), rig.Triggers.end(), [&](const RigFile::Trigger& existing) {
		return existing.Id == trigger.Id;
	}));
	trigger.Name = NextTriggerName(rig);
	trigger.StationTarget = std::string();
	trigger.MidiInputs = RigFile::Trigger::MidiInputMode::None;
	candidate.Triggers.push_back(std::move(trigger));
	return candidate;
}

std::optional<RigFile> RigFileRouting::WithoutTrigger(const RigFile& rig, size_t triggerIndex)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	candidate.Triggers.erase(candidate.Triggers.begin() + triggerIndex);
	return candidate;
}

std::optional<RigFile> RigFileRouting::WithStationTarget(const RigFile& rig, size_t triggerIndex, std::string target)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	candidate.Triggers[triggerIndex].StationTarget = std::move(target);
	return candidate;
}

std::optional<RigFile> RigFileRouting::WithAdcInput(const RigFile& rig, size_t triggerIndex, unsigned int channel)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	auto& channels = candidate.Triggers[triggerIndex].InputChannels;
	if (std::find(channels.begin(), channels.end(), channel) != channels.end()) return std::nullopt;
	channels.push_back(channel);
	return candidate;
}

std::optional<RigFile> RigFileRouting::WithoutAdcInput(const RigFile& rig, size_t triggerIndex, unsigned int channel)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	auto& channels = candidate.Triggers[triggerIndex].InputChannels;
	auto found = std::find(channels.begin(), channels.end(), channel);
	if (found == channels.end()) return std::nullopt;
	channels.erase(found);
	return candidate;
}

std::optional<RigFile> RigFileRouting::WithMidiInput(const RigFile& rig, size_t triggerIndex, std::string device)
{
	if (triggerIndex >= rig.Triggers.size() || device.empty() || device == "*") return std::nullopt;
	auto candidate = rig;
	auto& trigger = candidate.Triggers[triggerIndex];
	if (std::find(trigger.MidiInputDevices.begin(), trigger.MidiInputDevices.end(), device) != trigger.MidiInputDevices.end()) return std::nullopt;
	trigger.MidiInputs = RigFile::Trigger::MidiInputMode::Selected;
	trigger.MidiInputDevices.push_back(std::move(device));
	return candidate;
}

std::optional<RigFile> RigFileRouting::WithoutMidiInput(const RigFile& rig, size_t triggerIndex, const std::string& device)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	auto& trigger = candidate.Triggers[triggerIndex];
	auto found = std::find(trigger.MidiInputDevices.begin(), trigger.MidiInputDevices.end(), device);
	if (found == trigger.MidiInputDevices.end()) return std::nullopt;
	trigger.MidiInputDevices.erase(found);
	if (trigger.MidiInputDevices.empty()) trigger.MidiInputs = RigFile::Trigger::MidiInputMode::None;
	return candidate;
}
