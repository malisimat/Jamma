///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "JamFile.h"
#include <sstream>
#include <iomanip>
#include <locale>

using namespace io;
using audio::BehaviourParams;

const std::string JamFile::DefaultJson = "{\"name\":\"default\",\"stations\":[{\"name\":\"HiHat\",\"stationtype\":0,\"takes\":[{\"name\":\"Take1\",\"loops\":[{\"name\":\"Loop1.wav\",\"length\":155822,\"mix\":{\"type\":\"pan\",\"chans\":[0.5,0.5]}}]}]}],\"quantisesamps\":77911,\"quantisation\":\"multiple\"}";

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
	jam.Name = std::get<std::string>(jamParams.KeyValues["name"]);

	auto iter = jamParams.KeyValues.find("stations");
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
	// Local helpers
	auto quoted  = [](const std::string& s) { return '"' + s + '"'; };
	auto kvStr   = [&](const std::string& k, const std::string& v)
	                   { return quoted(k) + ":" + quoted(v); };
	auto kvUlong = [&](const std::string& k, unsigned long v)
	                   { return quoted(k) + ":" + std::to_string(v); };
	auto kvDouble = [&](const std::string& k, double v)
	                   {
	                       std::ostringstream o;
	                       o.imbue(std::locale::classic());
	                       o << std::fixed << std::setprecision(10) << v;
	                       return quoted(k) + ":" + o.str();
	                   };
	auto kvBool  = [&](const std::string& k, bool v)
	                   { return quoted(k) + ":" + (v ? "true" : "false"); };

	auto quantStr = [](engine::Timer::QuantisationType q) -> std::string {
		if (q == engine::Timer::QUANTISE_MULTIPLE) return "multiple";
		if (q == engine::Timer::QUANTISE_POWER)    return "power";
		return "off";
	};

	auto mixStr = [&](const JamFile::LoopMix& m) -> std::string {
		const std::string typeStr = (m.Mix == LoopMix::MIX_WIRE) ? "wire" : "pan";
		std::string chans;
		if (m.Mix == LoopMix::MIX_WIRE)
		{
			const auto& v = std::get<std::vector<unsigned long>>(m.Params);
			for (size_t i = 0; i < v.size(); ++i)
				chans += (i ? "," : "") + std::to_string(v[i]);
		}
		else
		{
			const auto& v = std::get<std::vector<double>>(m.Params);
			for (size_t i = 0; i < v.size(); ++i)
			{
				std::ostringstream o;
				o.imbue(std::locale::classic());
				o << std::fixed << std::setprecision(10) << v[i];
				chans += (i ? "," : "") + o.str();
			}
		}
		return "{" + kvStr("type", typeStr) + "," + quoted("chans") + ":[" + chans + "]}";
	};

	ss << "{";
	ss << kvStr("name", jam.Name) << ",";
	ss << kvUlong("timerticks", jam.TimerTicks) << ",";
	ss << kvUlong("quantisesamps", jam.QuantiseSamps) << ",";
	ss << kvStr("quantisation", quantStr(jam.Quantisation)) << ",";
	ss << quoted("stations") << ":["; 

	for (size_t si = 0; si < jam.Stations.size(); ++si)
	{
		const auto& st = jam.Stations[si];
		if (si) ss << ",";
		ss << "{" << kvStr("name", st.Name) << ","
		          << kvUlong("stationtype", st.StationType) << ","
		          << quoted("takes") << ":["; 

		for (size_t ti = 0; ti < st.LoopTakes.size(); ++ti)
		{
			const auto& take = st.LoopTakes[ti];
			if (ti) ss << ",";
			ss << "{" << kvStr("name", take.Name) << ","
			          << quoted("loops") << ":["; 

			for (size_t li = 0; li < take.Loops.size(); ++li)
			{
				const auto& lp = take.Loops[li];
				if (li) ss << ",";
				ss << "{"
				   << kvStr("name",             lp.Name)               << ","
				   << kvUlong("length",          lp.Length)             << ","
				   << kvUlong("index",           lp.Index)              << ","
				   << kvUlong("masterloopcount", lp.MasterLoopCount)    << ","
				   << kvDouble("level",          lp.Level)              << ","
				   << kvDouble("speed",          lp.Speed)              << ","
				   << kvUlong("mutegroups",      lp.MuteGroups)         << ","
				   << kvUlong("selectgroups",    lp.SelectGroups)       << ","
				   << kvBool("muted",            lp.Muted)              << ","
				   << quoted("mix") << ":" << mixStr(lp.Mix)
				   << "}";
			}
			ss << "]}";	// close loops array + take object
		}
		ss << "]}";		// close takes array + station object
	}
	ss << "]}";			// close stations array + root object
	return true;
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