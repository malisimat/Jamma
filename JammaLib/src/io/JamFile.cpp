///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "JamFile.h"
#include <iomanip>

using namespace io;
using audio::BehaviourParams;

const std::string JamFile::DefaultJson = "{\"name\":\"default\",\"ninjam\":{\"host\":\"ninjam.com:2049\",\"user\":\"jamma_guest\",\"pass\":\"\",\"workdir\":\"\"},\"stations\":[{\"name\":\"HiHat\",\"stationtype\":0,\"takes\":[{\"name\":\"Take1\",\"loops\":[{\"name\":\"Loop1.wav\",\"length\":155822,\"mix\":{\"type\":\"pan\",\"chans\":[0.5,0.5]}}]}]}],\"quantisesamps\":77911,\"quantisation\":\"multiple\"}";

std::optional<JamFile> JamFile::FromStream(std::stringstream ss)
{
	auto root = Json::FromStream(std::move(ss));

	if (!root.has_value())
		return std::nullopt;

	if (root.value().index() != 6)
		return std::nullopt;

	auto jamParams = std::get<Json::JsonPart>(root.value());

	if (jamParams.KeyValues.find("name") == jamParams.KeyValues.end())
		return std::nullopt;

	if (jamParams.KeyValues["name"].index() != 4)
		return std::nullopt;

	JamFile jam;
	jam.Version = VERSION_V;
	jam.TimerTicks = 0;
	jam.QuantiseSamps = 0;
	jam.Quantisation = engine::Timer::QUANTISE_OFF;
	jam.Name = std::get<std::string>(jamParams.KeyValues["name"]);

	auto iter = jamParams.KeyValues.find("ninjam");
	if (iter != jamParams.KeyValues.end())
	{
		if (jamParams.KeyValues["ninjam"].index() == 6)
		{
			auto ninjamOpt = NinjamConfig::FromJson(std::get<Json::JsonPart>(jamParams.KeyValues["ninjam"]));
			if (ninjamOpt.has_value())
				jam.Ninjam = ninjamOpt.value();
		}
	}

	iter = jamParams.KeyValues.find("stations");
	if (iter != jamParams.KeyValues.end())
	{
		if (jamParams.KeyValues["stations"].index() == 5)
		{
			auto stationArr = std::get<Json::JsonArray>(jamParams.KeyValues["stations"]);
			if (stationArr.Array.index() == 5)
			{
				auto stations = std::get<std::vector<Json::JsonPart>>(stationArr.Array);

				for (auto stationJson : stations)
				{
					auto stationOpt = Station::FromJson(stationJson);
					if (stationOpt.has_value())
						jam.Stations.push_back(stationOpt.value());
				}
			}
		}
	}

	iter = jamParams.KeyValues.find("timerticks");
	if (iter != jamParams.KeyValues.end())
	{
		if (jamParams.KeyValues["timerticks"].index() == 2)
			jam.TimerTicks = std::get<unsigned long>(jamParams.KeyValues["timerticks"]);
	}

	iter = jamParams.KeyValues.find("quantisesamps");
	if (iter != jamParams.KeyValues.end())
	{
		if (jamParams.KeyValues["quantisesamps"].index() == 2)
			jam.QuantiseSamps = std::get<unsigned long>(jamParams.KeyValues["quantisesamps"]);
	}

	std::string quantiseStr = "";
	iter = jamParams.KeyValues.find("quantisation");
	if (iter != jamParams.KeyValues.end())
	{
		if (jamParams.KeyValues["quantisation"].index() == 4)
			quantiseStr = std::get<std::string>(jamParams.KeyValues["quantisation"]);
	}

	if (quantiseStr.compare("multiple") == 0)
		jam.Quantisation = engine::Timer::QUANTISE_MULTIPLE;
	else if (quantiseStr.compare("power") == 0)
		jam.Quantisation = engine::Timer::QUANTISE_POWER;
	else
		jam.Quantisation = engine::Timer::QUANTISE_OFF;

	return jam;
}

bool JamFile::ToStream(JamFile jam, std::stringstream& ss)
{
	auto escapeJsonString = [](const std::string& s) -> std::string {
		std::string escaped;
		escaped.reserve(s.size());
		const char* HEX_DIGITS = "0123456789abcdef";

		for (unsigned char c : s)
		{
			switch (c)
			{
			case '\"': escaped += "\\\""; break;
			case '\\': escaped += "\\\\"; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (c < 0x20)
				{
					escaped += "\\u00";
					escaped += HEX_DIGITS[(c >> 4) & 0x0f];
					escaped += HEX_DIGITS[c & 0x0f];
				}
				else
				{
					escaped += static_cast<char>(c);
				}
				break;
			}
		}

		return escaped;
	};

	auto quoted = [&](const std::string& s) { return "\"" + escapeJsonString(s) + "\""; };
	auto formatDouble = [](double value) -> std::string {
		std::ostringstream out;
		out << std::setprecision(15) << std::defaultfloat << value;
		auto str = out.str();
		if (str.find('.') == std::string::npos && str.find('e') == std::string::npos && str.find('E') == std::string::npos)
			str += ".0";
		return str;
	};
	auto kvStr = [&](const std::string& key, const std::string& value)
		{ return quoted(key) + ":" + quoted(value); };
	auto kvUlong = [&](const std::string& key, unsigned long value)
		{ return quoted(key) + ":" + std::to_string(value); };
	auto kvDouble = [&](const std::string& key, double value)
		{ return quoted(key) + ":" + formatDouble(value); };
	auto kvBool = [&](const std::string& key, bool value)
		{ return quoted(key) + ":" + (value ? "true" : "false"); };

	auto quantStr = [](engine::Timer::QuantisationType quant) -> std::string {
		switch (quant)
		{
		case engine::Timer::QUANTISE_MULTIPLE:
			return "multiple";
		case engine::Timer::QUANTISE_POWER:
			return "power";
		case engine::Timer::QUANTISE_OFF:
		default:
			return "off";
		}
	};

	auto mixToJson = [&](const JamFile::LoopMix& mix) -> std::string {
		std::string chans;
		const auto mixType = (mix.Mix == LoopMix::MIX_WIRE) ? "wire" : "pan";

		if (mix.Mix == LoopMix::MIX_WIRE)
		{
			if (auto values = std::get_if<std::vector<unsigned long>>(&mix.Params))
			{
				for (size_t i = 0; i < values->size(); ++i)
				{
					if (i > 0) chans += ",";
					chans += std::to_string((*values)[i]);
				}
			}
		}
		else
		{
			if (auto values = std::get_if<std::vector<double>>(&mix.Params))
			{
				for (size_t i = 0; i < values->size(); ++i)
				{
					if (i > 0) chans += ",";
					chans += formatDouble((*values)[i]);
				}
			}
		}

		return "{" + kvStr("type", mixType) + "," + quoted("chans") + ":[" + chans + "]}";
	};

	auto ninjamToJson = [&](const JamFile::NinjamConfig& ninjam) -> std::string {
		auto out = std::string("{") + kvStr("host", ninjam.Host)
			+ "," + kvStr("user", ninjam.User)
			+ "," + kvStr("pass", ninjam.Pass)
			+ "," + kvStr("workdir", ninjam.WorkDir);
		if (ninjam.Bpm.has_value())
			out += "," + kvDouble("bpm", ninjam.Bpm.value());
		if (ninjam.Bpi.has_value())
			out += "," + kvUlong("bpi", ninjam.Bpi.value());
		out += "}";
		return out;
	};

	ss << "{";
	ss << kvStr("name", jam.Name) << ",";
	ss << kvUlong("timerticks", jam.TimerTicks) << ",";
	ss << kvUlong("quantisesamps", jam.QuantiseSamps) << ",";
	ss << kvStr("quantisation", quantStr(jam.Quantisation)) << ",";
	if (jam.Ninjam.has_value())
		ss << quoted("ninjam") << ":" << ninjamToJson(jam.Ninjam.value()) << ",";
	ss << quoted("stations") << ":[";

	for (size_t stationIndex = 0; stationIndex < jam.Stations.size(); ++stationIndex)
	{
		const auto& station = jam.Stations[stationIndex];
		if (stationIndex > 0) ss << ",";
		ss << "{" << kvStr("name", station.Name) << ","
			<< kvUlong("stationtype", station.StationType) << ","
			<< quoted("takes") << ":[";

		for (size_t takeIndex = 0; takeIndex < station.LoopTakes.size(); ++takeIndex)
		{
			const auto& take = station.LoopTakes[takeIndex];
			if (takeIndex > 0) ss << ",";
			ss << "{" << kvStr("name", take.Name) << ","
				<< quoted("loops") << ":[";

			for (size_t loopIndex = 0; loopIndex < take.Loops.size(); ++loopIndex)
			{
				const auto& loop = take.Loops[loopIndex];
				if (loopIndex > 0) ss << ",";
				ss << "{"
					<< kvStr("name", loop.Name) << ","
					<< kvUlong("length", loop.Length) << ","
					<< kvUlong("index", loop.Index) << ","
					<< kvUlong("masterloopcount", loop.MasterLoopCount) << ","
					<< kvDouble("level", loop.Level) << ","
					<< kvDouble("speed", loop.Speed) << ","
					<< kvUlong("mutegroups", loop.MuteGroups) << ","
					<< kvUlong("selectgroups", loop.SelectGroups) << ","
					<< kvBool("muted", loop.Muted) << ","
					<< quoted("mix") << ":" << mixToJson(loop.Mix)
					<< "}";
			}

			ss << "]}";
		}

		ss << "]}";
	}

	ss << "]}";
	return true;
}

std::optional<JamFile::NinjamConfig> JamFile::NinjamConfig::FromJson(Json::JsonPart json)
{
	NinjamConfig config;

	auto iter = json.KeyValues.find("host");
	if ((iter != json.KeyValues.end()) && (json.KeyValues["host"].index() == 4))
		config.Host = std::get<std::string>(json.KeyValues["host"]);

	iter = json.KeyValues.find("user");
	if ((iter != json.KeyValues.end()) && (json.KeyValues["user"].index() == 4))
		config.User = std::get<std::string>(json.KeyValues["user"]);

	iter = json.KeyValues.find("pass");
	if ((iter != json.KeyValues.end()) && (json.KeyValues["pass"].index() == 4))
		config.Pass = std::get<std::string>(json.KeyValues["pass"]);

	iter = json.KeyValues.find("workdir");
	if ((iter != json.KeyValues.end()) && (json.KeyValues["workdir"].index() == 4))
		config.WorkDir = std::get<std::string>(json.KeyValues["workdir"]);

	iter = json.KeyValues.find("bpm");
	if (iter != json.KeyValues.end())
	{
		// JsonValue layout: bool=0, long=1, unsigned long=2, double=3, string=4
		const auto idx = json.KeyValues["bpm"].index();
		if (idx == 3)
			config.Bpm = std::get<double>(json.KeyValues["bpm"]);
		else if (idx == 2)
			config.Bpm = static_cast<double>(std::get<unsigned long>(json.KeyValues["bpm"]));
		else if (idx == 1)
			config.Bpm = static_cast<double>(std::get<long>(json.KeyValues["bpm"]));
	}

	iter = json.KeyValues.find("bpi");
	if ((iter != json.KeyValues.end()) && (json.KeyValues["bpi"].index() == 2))
		config.Bpi = std::get<unsigned long>(json.KeyValues["bpi"]);

	if (config.Host.empty() && config.User.empty() && config.Pass.empty() && config.WorkDir.empty())
		return std::nullopt;

	return config;
}

std::optional<JamFile::LoopMix> JamFile::LoopMix::FromJson(Json::JsonPart json)
{
	std::string typeStr;
	std::vector<Loop> loops;

	auto iter = json.KeyValues.find("type");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["type"].index() == 4)
			typeStr = std::get<std::string>(json.KeyValues["type"]);
	}

	LoopMix mix;

	if (typeStr.compare("wire") == 0)
		mix.Mix = MIX_WIRE;
	else if (typeStr.compare("pan") == 0)
		mix.Mix = MIX_PAN;
	else
		return std::nullopt;

	switch (mix.Mix)
	{
	case MIX_WIRE:
		iter = json.KeyValues.find("chans");
		if (iter == json.KeyValues.end())
			return std::nullopt;

		if (json.KeyValues["chans"].index() == 5)
		{
			auto arr = std::get<Json::JsonArray>(json.KeyValues["chans"]);
			if (arr.Array.index() == 2)
				mix.Params = std::get<std::vector<unsigned long>>(arr.Array);
		}
		break;
	case MIX_PAN:
		iter = json.KeyValues.find("chans");
		if (iter == json.KeyValues.end())
			return std::nullopt;

		if (json.KeyValues["chans"].index() == 5)
		{
			auto arr = std::get<Json::JsonArray>(json.KeyValues["chans"]);
			if (arr.Array.index() == 3)
				mix.Params = std::get<std::vector<double>>(arr.Array);
		}
		break;
	}

	return mix;
}

std::optional<JamFile::Loop> JamFile::Loop::FromJson(Json::JsonPart json)
{
	std::string name;
	unsigned long length = 0;
	unsigned long index = 0;
	unsigned long masterLoopCount = 0;
	double level = 1.0;
	double speed = 1.0;
	unsigned int muteGroups = 0;
	unsigned int selectGroups = 0;
	bool isMuted = false;
	LoopMix mix;

	auto iter = json.KeyValues.find("name");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["name"].index() == 4)
			name = std::get<std::string>(json.KeyValues["name"]);
	}

	iter = json.KeyValues.find("length");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["length"].index() == 2)
			length = std::get<unsigned long>(json.KeyValues["length"]);
	}

	if ((0 == length) || name.empty())
		return std::nullopt;

	iter = json.KeyValues.find("index");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["index"].index() == 2)
			index = std::get<unsigned long>(json.KeyValues["index"]);
	}

	iter = json.KeyValues.find("masterloopcount");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["masterloopcount"].index() == 2)
			masterLoopCount = std::get<unsigned long>(json.KeyValues["masterloopcount"]);
	}

	iter = json.KeyValues.find("level");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["level"].index() == 3)
			level = std::get<double>(json.KeyValues["level"]);
	}

	iter = json.KeyValues.find("speed");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["speed"].index() == 3)
			speed = std::get<double>(json.KeyValues["speed"]);
	}

	iter = json.KeyValues.find("mutegroups");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["mutegroups"].index() == 2)
			muteGroups = std::get<unsigned long>(json.KeyValues["mutegroups"]);
	}

	iter = json.KeyValues.find("selectgroups");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["selectgroups"].index() == 2)
			selectGroups = std::get<unsigned long>(json.KeyValues["selectgroups"]);
	}

	iter = json.KeyValues.find("muted");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["muted"].index() == 0)
			isMuted = std::get<bool>(json.KeyValues["muted"]);
	}

	iter = json.KeyValues.find("mix");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["mix"].index() == 6)
		{
			auto mixOpt = LoopMix::FromJson(std::get<Json::JsonPart>(json.KeyValues["mix"]));
			if (mixOpt.has_value())
				mix = mixOpt.value();
		}
	}

	Loop loop;
	loop.Name = name;
	loop.Length = length;
	loop.Index = index;
	loop.MasterLoopCount = masterLoopCount;
	loop.Level = level;
	loop.Speed = speed;
	loop.MuteGroups = muteGroups;
	loop.SelectGroups = selectGroups;
	loop.Muted = isMuted;
	loop.Mix = mix;
	return loop;
}

std::optional<JamFile::LoopTake> JamFile::LoopTake::FromJson(Json::JsonPart json)
{
	std::string name;
	std::vector<Loop> loops;

	auto iter = json.KeyValues.find("name");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["name"].index() == 4)
			name = std::get<std::string>(json.KeyValues["name"]);
	}

	iter = json.KeyValues.find("loops");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["loops"].index() == 5)
		{
			auto jsonArray = std::get<Json::JsonArray>(json.KeyValues["loops"]);
			
			if (jsonArray.Array.index() == 5)
			{
				auto loopArray = std::get<std::vector<Json::JsonPart>>(jsonArray.Array);
				for (auto loopJson : loopArray)
				{
					auto loop = Loop::FromJson(loopJson);
					if (loop.has_value())
						loops.push_back(loop.value());
				}
			}
		}
	}

	if (loops.empty() || name.empty())
		return std::nullopt;

	LoopTake take;
	take.Name = name;
	take.Loops = loops;
	return take;
}

std::optional<JamFile::Station> JamFile::Station::FromJson(Json::JsonPart json)
{
	std::string name;
	unsigned int stationType = 0;
	std::vector<LoopTake> takes;

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

	iter = json.KeyValues.find("takes");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["takes"].index() == 5)
		{
			auto jsonArray = std::get<Json::JsonArray>(json.KeyValues["takes"]);

			if (jsonArray.Array.index() == 5)
			{
				auto takeArray = std::get<std::vector<Json::JsonPart>>(jsonArray.Array);
				for (auto takeJson : takeArray)
				{
					auto take = LoopTake::FromJson(takeJson);
					if (take.has_value())
						takes.push_back(take.value());
				}
			}
		}
	}

	if (name.empty())
		return std::nullopt;

	Station station;
	station.Name = name;
	station.StationType = stationType;
	station.LoopTakes = takes;
	return station;
}