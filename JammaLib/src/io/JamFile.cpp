///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "JamFile.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <filesystem>
#include <regex>
#include <string>
#include "../utils/MathUtils.h"
#include "../utils/StringUtils.h"

using namespace io;
using audio::BehaviourParams;

const std::string JamFile::DefaultJson = "{\"name\":\"default\",\"ninjam\":{\"host\":\"ninjam.com:2049\",\"user\":\"jamma_guest\",\"pass\":\"\",\"workdir\":\"\"},\"stations\":[{\"name\":\"HiHat\",\"stationtype\":0,\"takes\":[{\"name\":\"Take1\",\"loops\":[{\"name\":\"Loop1.wav\",\"length\":155822,\"mix\":{\"type\":\"pan\",\"chans\":[0.5,0.5]}}]}]}],\"quantisesamps\":77911,\"quantisation\":\"multiple\"}";

std::int32_t JamFile::ParseInt32Clamped(const Json::JsonValue& value, std::int32_t fallback) noexcept
{
	long long parsed = fallback;
	switch (value.index())
	{
	case 1:
		parsed = std::get<long>(value);
		break;
	case 2:
	{
		const auto unsignedValue = std::get<unsigned long>(value);
		parsed = unsignedValue > static_cast<unsigned long>((std::numeric_limits<std::int32_t>::max)()) ?
			static_cast<long long>((std::numeric_limits<std::int32_t>::max)()) :
			static_cast<long long>(unsignedValue);
		break;
	}
	case 3:
		{
			const auto doubleValue = std::get<double>(value);
			if (!std::isfinite(doubleValue))
				return fallback;

			const auto maxInt32Double = static_cast<double>((std::numeric_limits<std::int32_t>::max)());
			const auto minInt32Double = static_cast<double>((std::numeric_limits<std::int32_t>::min)());
			if (doubleValue >= maxInt32Double)
				return (std::numeric_limits<std::int32_t>::max)();
			if (doubleValue <= minInt32Double)
				return (std::numeric_limits<std::int32_t>::min)();

			parsed = static_cast<long long>(doubleValue);
			break;
		}
	default:
		break;
	}

	if (parsed > static_cast<long long>((std::numeric_limits<std::int32_t>::max)()))
		return (std::numeric_limits<std::int32_t>::max)();
	if (parsed < static_cast<long long>((std::numeric_limits<std::int32_t>::min)()))
		return (std::numeric_limits<std::int32_t>::min)();
	return static_cast<std::int32_t>(parsed);
}

bool JamFile::IsSafeSidecarPath(const std::string& path) noexcept
{
	if (path.empty() || path.size() > 1024u)
		return false;
	const std::filesystem::path candidate(path);
	if (candidate.is_absolute() || candidate.has_root_name() || candidate.has_root_directory())
		return false;
	for (const auto& component : candidate)
	{
		if (component == "..")
			return false;
	}
	return candidate.lexically_normal() == candidate && candidate.filename() != ".";
}

std::optional<std::uint64_t> JamFile::ParseStrictUint64(const std::string& text) noexcept
{
	if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
		return std::nullopt;
	try
	{
		std::size_t consumed = 0u;
		const auto value = std::stoull(text, &consumed, 10);
		return consumed == text.size() ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(value)) : std::nullopt;
	}
	catch (const std::exception&) { return std::nullopt; }
}

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

	JamFile jam{};
	jam.Version = VERSION_LEGACY;
	jam.FormatMajor = 0u;
	jam.FormatMinor = 0u;
	jam.FormatPatch = 0u;
	jam.TimerTicks = 0;
	jam.QuantiseSamps = 0;
	jam.GlobalMidiQuantStateValue = GlobalMidiQuantState::Off;
	jam.GlobalPhaseOffsetSamps = 0;
	jam.TransportOffsetLoopFrac = 0.0;
	jam.Quantisation = utils::Timer::QUANTISE_OFF;
	jam.Name = std::get<std::string>(jamParams.KeyValues["name"]);

	const auto versionIter = jamParams.KeyValues.find("formatVersion");
	const auto isCurrentSchema = versionIter != jamParams.KeyValues.end();
	if (isCurrentSchema)
	{
		if (versionIter->second.index() != 4)
		{
			std::cout << "JamFile: invalid formatVersion" << std::endl;
			return std::nullopt;
		}
		const auto versionText = std::get<std::string>(versionIter->second);
		const std::regex versionPattern("^([0-9]+)\\.([0-9]+)\\.([0-9]+)$");
		std::smatch match;
		if (!std::regex_match(versionText, match, versionPattern))
		{
			std::cout << "JamFile: malformed formatVersion '" << versionText << "'" << std::endl;
			return std::nullopt;
		}
		try
		{
			const auto parseComponent = [](const std::ssub_match& item) -> unsigned int
			{
				const auto parsed = std::stoull(item.str());
				if (parsed > (std::numeric_limits<unsigned int>::max)())
					throw std::out_of_range("format version component");
				return static_cast<unsigned int>(parsed);
			};
			jam.FormatMajor = parseComponent(match[1]);
			jam.FormatMinor = parseComponent(match[2]);
			jam.FormatPatch = parseComponent(match[3]);
		}
		catch (const std::exception&)
		{
			std::cout << "JamFile: invalid formatVersion '" << versionText << "'" << std::endl;
			return std::nullopt;
		}
		if (jam.FormatMajor > CurrentFormatMajor)
		{
			std::cout << "JamFile: formatVersion '" << versionText << "' has unsupported newer major" << std::endl;
			return std::nullopt;
		}
		jam.Version = VERSION_V;
		if (jam.FormatMajor != CurrentFormatMajor || jam.FormatMinor != CurrentFormatMinor || jam.FormatPatch != CurrentFormatPatch)
			std::cout << "JamFile: best-effort load for formatVersion '" << versionText << "'" << std::endl;
	}

	auto iter = jamParams.KeyValues.find("ninjam");
	if (!isCurrentSchema && iter != jamParams.KeyValues.end())
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

				if (stations.size() > MaxStations)
				{
					std::cout << "JamFile: station count exceeds limit" << std::endl;
					return std::nullopt;
				}
				for (auto stationJson : stations)
				{
					auto stationOpt = Station::FromJson(stationJson);
					if (stationOpt.has_value())
						jam.Stations.push_back(stationOpt.value());
					else
						std::cout << "JamFile: skipped invalid station" << std::endl;
				}
			}
		}
	}

	if (!isCurrentSchema)
	{
		iter = jamParams.KeyValues.find("timerticks");
		if (iter != jamParams.KeyValues.end() && jamParams.KeyValues["timerticks"].index() == 2)
			jam.TimerTicks = std::get<unsigned long>(jamParams.KeyValues["timerticks"]);
		iter = jamParams.KeyValues.find("quantisesamps");
		if (iter != jamParams.KeyValues.end() && jamParams.KeyValues["quantisesamps"].index() == 2)
			jam.QuantiseSamps = std::get<unsigned long>(jamParams.KeyValues["quantisesamps"]);
	}

	// Current manifests carry all local transport data in one object. Legacy
	// top-level fields above are intentionally still accepted as 0.0.0 input.
	iter = jamParams.KeyValues.find("transport");
	if (iter != jamParams.KeyValues.end())
	{
		if (iter->second.index() != 6)
		{
			std::cout << "JamFile: invalid transport object" << std::endl;
			return std::nullopt;
		}
		const auto& transport = std::get<Json::JsonPart>(iter->second);
		const auto readUnsigned = [](const Json::JsonPart& object, const char* key, unsigned long& out) -> bool
		{
			auto found = object.KeyValues.find(key);
			if (found == object.KeyValues.end() || found->second.index() != 2)
				return false;
			out = std::get<unsigned long>(found->second);
			return true;
		};
		unsigned long masterLength = 0u;
		unsigned long quantise = 0u;
		std::uint64_t absolute = 0u;
		const auto absoluteIter = transport.KeyValues.find("absoluteSamplePos");
		if (!readUnsigned(transport, "masterLengthSamps", masterLength) || !readUnsigned(transport, "quantiseSamps", quantise)
			|| absoluteIter == transport.KeyValues.end() || absoluteIter->second.index() != 4 || masterLength == 0u || masterLength > MaxLoopLengthSamps)
		{
			std::cout << "JamFile: invalid essential transport field" << std::endl;
			return std::nullopt;
		}
		const auto absoluteValue = ParseStrictUint64(std::get<std::string>(absoluteIter->second));
		if (!absoluteValue.has_value())
		{
			std::cout << "JamFile: invalid absoluteSamplePos" << std::endl;
			return std::nullopt;
		}
		absolute = *absoluteValue;
		jam.MasterLengthSamps = masterLength;
		jam.QuantiseSamps = static_cast<unsigned int>(quantise);
		jam.AbsoluteSamplePos = absolute;
		if (jam.AbsoluteSamplePos % jam.MasterLengthSamps >= jam.MasterLengthSamps)
			return std::nullopt;
		const auto quantisationIter = transport.KeyValues.find("quantisation");
		if (quantisationIter != transport.KeyValues.end() && quantisationIter->second.index() == 4)
		{
			const auto& text = std::get<std::string>(quantisationIter->second);
			jam.Quantisation = text == "multiple" ? utils::Timer::QUANTISE_MULTIPLE : text == "power" ? utils::Timer::QUANTISE_POWER : utils::Timer::QUANTISE_OFF;
		}
		const auto midiStateIter = transport.KeyValues.find("globalMidiQuantState");
		if (midiStateIter != transport.KeyValues.end() && midiStateIter->second.index() == 4)
		{
			const auto& text = std::get<std::string>(midiStateIter->second);
			jam.GlobalMidiQuantStateValue = text == "all" ? GlobalMidiQuantState::All : text == "mixed" ? GlobalMidiQuantState::Mixed : GlobalMidiQuantState::Off;
		}
		const auto globalOffsetIter = transport.KeyValues.find("globalPhaseOffsetSamps");
		if (globalOffsetIter != transport.KeyValues.end())
			jam.GlobalPhaseOffsetSamps = ParseInt32Clamped(globalOffsetIter->second, 0);
		const auto offsetIter = transport.KeyValues.find("transportOffsetLoopFrac");
		if (offsetIter != transport.KeyValues.end())
		{
			const auto value = offsetIter->second.index() == 3 ? std::get<double>(offsetIter->second) : 0.0;
			if (!std::isfinite(value) || value < -1.0 || value > 1.0)
			{
				std::cout << "JamFile: invalid transport offset" << std::endl;
				return std::nullopt;
			}
			jam.TransportOffsetLoopFrac = value;
		}
	}
	else if (isCurrentSchema)
	{
		std::cout << "JamFile: current format requires transport" << std::endl;
		return std::nullopt;
	}

	iter = jamParams.KeyValues.find("globalmidiquantstate");
	if (!isCurrentSchema && iter != jamParams.KeyValues.end())
	{
		const auto& value = jamParams.KeyValues["globalmidiquantstate"];
		auto parsed = static_cast<int>(GlobalMidiQuantState::Mixed);
		bool parsedValue = false;

		if (value.index() == 4)
		{
			auto state = std::get<std::string>(value);
			std::transform(state.begin(), state.end(), state.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (state == "off")
			{
				parsed = static_cast<int>(GlobalMidiQuantState::Off);
				parsedValue = true;
			}
			else if ((state == "mixed") || (state == "x"))
			{
				parsed = static_cast<int>(GlobalMidiQuantState::Mixed);
				parsedValue = true;
			}
			else if (state == "all")
			{
				parsed = static_cast<int>(GlobalMidiQuantState::All);
				parsedValue = true;
			}
		}
		else if (value.index() == 1)
		{
			parsed = static_cast<int>(std::get<long>(value));
			parsedValue = true;
		}
		else if (value.index() == 2)
		{
			parsed = static_cast<int>(std::get<unsigned long>(value));
			parsedValue = true;
		}
		else if (value.index() == 3)
		{
			parsed = static_cast<int>(std::get<double>(value));
			parsedValue = true;
		}

		if (parsedValue)
		{
			switch (parsed)
			{
			case static_cast<int>(GlobalMidiQuantState::Off):
				jam.GlobalMidiQuantStateValue = GlobalMidiQuantState::Off;
				break;
			case static_cast<int>(GlobalMidiQuantState::All):
				jam.GlobalMidiQuantStateValue = GlobalMidiQuantState::All;
				break;
			case static_cast<int>(GlobalMidiQuantState::Mixed):
			default:
				jam.GlobalMidiQuantStateValue = GlobalMidiQuantState::Mixed;
				break;
			}
		}
	}

	if (!isCurrentSchema)
	{
		iter = jamParams.KeyValues.find("globalphaseoffsetsamps");
		if (iter != jamParams.KeyValues.end())
			jam.GlobalPhaseOffsetSamps = ParseInt32Clamped(jamParams.KeyValues["globalphaseoffsetsamps"], 0);

		iter = jamParams.KeyValues.find("transportoffsetloopfrac");
		if (iter != jamParams.KeyValues.end())
		{
			const auto& value = jamParams.KeyValues["transportoffsetloopfrac"];
			auto parsed = 0.0;
			switch (value.index())
			{
			case 1:
				parsed = static_cast<double>(std::get<long>(value));
				break;
			case 2:
				parsed = static_cast<double>(std::get<unsigned long>(value));
				break;
			case 3:
				parsed = std::get<double>(value);
				break;
			default:
				break;
			}

			jam.TransportOffsetLoopFrac = std::isfinite(parsed) ?
				std::clamp(parsed, -1.0, 1.0) : 0.0;
		}
	}

	if (!isCurrentSchema)
	{
		std::string quantiseStr = "";
		iter = jamParams.KeyValues.find("quantisation");
		if (iter != jamParams.KeyValues.end() && jamParams.KeyValues["quantisation"].index() == 4)
			quantiseStr = std::get<std::string>(jamParams.KeyValues["quantisation"]);
		if (quantiseStr.compare("multiple") == 0)
			jam.Quantisation = utils::Timer::QUANTISE_MULTIPLE;
		else if (quantiseStr.compare("power") == 0)
			jam.Quantisation = utils::Timer::QUANTISE_POWER;
		else
			jam.Quantisation = utils::Timer::QUANTISE_OFF;
	}

	if (isCurrentSchema && jam.Stations.empty())
	{
		std::cout << "JamFile: no constructible stations" << std::endl;
		return std::nullopt;
	}
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
	auto kvInt = [&](const std::string& key, std::int32_t value)
		{ return quoted(key) + ":" + std::to_string(value); };
	auto kvDouble = [&](const std::string& key, double value)
		{ return quoted(key) + ":" + formatDouble(value); };
	auto kvBool = [&](const std::string& key, bool value)
		{ return quoted(key) + ":" + (value ? "true" : "false"); };
	auto kvIntArray = [&](const std::string& key, const std::vector<int>& values)
		{
			std::string out = quoted(key) + ":[";
			for (size_t i = 0; i < values.size(); ++i)
			{
				if (i > 0) out += ",";
				out += std::to_string(values[i]);
			}
			out += "]";
			return out;
		};

	auto quantStr = [](utils::Timer::QuantisationType quant) -> std::string {
		switch (quant)
		{
		case utils::Timer::QUANTISE_MULTIPLE:
			return "multiple";
		case utils::Timer::QUANTISE_POWER:
			return "power";
		case utils::Timer::QUANTISE_OFF:
		default:
			return "off";
		}
	};

	auto midiGlobalQuantStr = [](JamFile::GlobalMidiQuantState state) -> std::string {
		switch (state)
		{
		case JamFile::GlobalMidiQuantState::Off:
			return "off";
		case JamFile::GlobalMidiQuantState::All:
			return "all";
		case JamFile::GlobalMidiQuantState::Mixed:
		default:
			return "mixed";
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

	auto vstEntryToJson = [&](const JamFile::VstEntry& entry) -> std::string {
		std::string out = "{" + kvStr("path", entry.Path)
			+ "," + kvBool("bypass", entry.Bypass)
			+ "," + kvStr("state", entry.State);
		out += "}";
		return out;
	};

	auto vstChainToJson = [&](const std::vector<JamFile::VstEntry>& chain) -> std::string {
		std::string out = "[";
		for (size_t i = 0; i < chain.size(); ++i)
		{
			if (i > 0) out += ",";
			out += vstEntryToJson(chain[i]);
		}
		out += "]";
		return out;
	};
	auto midiStreamsToJson = [&](const std::vector<JamFile::MidiStream>& streams) -> std::string {
		std::string out = "[";
		for (size_t i = 0; i < streams.size(); ++i)
		{
			const auto& stream = streams[i];
			if (i > 0) out += ",";
			out += "{" + kvStr("sidecar", stream.SidecarPath) + "," + kvUlong("channel", stream.Channel) + ","
				+ kvStr("device", stream.Device) + "," + kvUlong("logicalLength", stream.LogicalLength) + ","
				+ kvStr("automationGlobalSampleOrigin", std::to_string(stream.AutomationGlobalSampleOrigin)) + "}";
		}
		return out + "]";
	};
	auto midiRoutesToJson = [&](const std::vector<JamFile::MidiRoute>& routes) -> std::string {
		std::string out = "[";
		for (size_t i = 0; i < routes.size(); ++i)
		{
			if (i > 0) out += ",";
			const auto& route = routes[i];
			out += "{" + kvUlong("outputIndex", route.OutputIndex) + "," + kvBool("live", route.IsLive) + ","
				+ kvUlong("pluginIndex", route.PluginIndex) + "}";
		}
		return out + "]";
	};

	if (jam.MasterLengthSamps == 0u || jam.MasterLengthSamps > MaxLoopLengthSamps)
	{
		std::cout << "JamFile: refusing to write invalid local master length" << std::endl;
		return false;
	}
	for (const auto& station : jam.Stations)
	{
		if (station.LoopTakes.size() > MaxTakesPerStation)
			return false;
		for (const auto& take : station.LoopTakes)
		{
			if (take.Loops.size() > MaxLoopsPerTake || take.MidiStreams.size() > MaxMidiStreamsPerTake)
				return false;
			for (const auto& loop : take.Loops)
				if (loop.Length == 0u || loop.Length > MaxLoopLengthSamps || loop.BodyPlayIndex >= loop.Length || !IsSafeSidecarPath(loop.Name))
					return false;
			for (const auto& stream : take.MidiStreams)
				if (stream.LogicalLength == 0u || stream.Channel > 15u || !IsSafeSidecarPath(stream.SidecarPath))
					return false;
		}
	}
	const auto transportOffsetLoopFrac = std::isfinite(jam.TransportOffsetLoopFrac) ?
		std::clamp(jam.TransportOffsetLoopFrac, -1.0, 1.0) : 0.0;

	ss << "{";
	ss << kvStr("formatVersion", "0.1.0") << ",";
	ss << kvStr("name", jam.Name) << ",";
	ss << quoted("transport") << ":{";
	ss << kvUlong("masterLengthSamps", jam.MasterLengthSamps) << ",";
	ss << kvUlong("quantiseSamps", jam.QuantiseSamps) << ",";
	ss << kvStr("quantisation", quantStr(jam.Quantisation)) << ",";
	ss << kvStr("globalMidiQuantState", midiGlobalQuantStr(jam.GlobalMidiQuantStateValue)) << ",";
	ss << kvInt("globalPhaseOffsetSamps", jam.GlobalPhaseOffsetSamps) << ",";
	ss << kvStr("absoluteSamplePos", std::to_string(jam.AbsoluteSamplePos)) << ",";
	ss << kvDouble("transportOffsetLoopFrac", transportOffsetLoopFrac);
	ss << "},";
	ss << quoted("stations") << ":[";

	for (size_t stationIndex = 0; stationIndex < jam.Stations.size(); ++stationIndex)
	{
		const auto& station = jam.Stations[stationIndex];
		if (stationIndex > 0) ss << ",";
		ss << "{" << kvStr("name", station.Name) << ","
			<< kvUlong("stationtype", station.StationType) << ","
			<< kvInt("stationphaseoffsetsamps", station.StationPhaseOffsetSamps) << ","
			<< quoted("takes") << ":[";

		for (size_t takeIndex = 0; takeIndex < station.LoopTakes.size(); ++takeIndex)
		{
			const auto& take = station.LoopTakes[takeIndex];
			if (takeIndex > 0) ss << ",";
			ss << "{" << kvStr("name", take.Name) << ","
				<< kvBool("midiquantenabled", take.MidiQuantEnabled) << ","
				<< kvInt("midiquantfraction", static_cast<std::int32_t>(take.MidiQuantFraction)) << ","
				<< kvInt("takephaseoffsetsamps", take.TakePhaseOffsetSamps) << ","
				<< quoted("loops") << ":[";

			for (size_t loopIndex = 0; loopIndex < take.Loops.size(); ++loopIndex)
			{
				const auto& loop = take.Loops[loopIndex];
				if (loopIndex > 0) ss << ",";
				ss << "{"
					<< kvStr("name", loop.Name) << ","
					<< kvStr("sidecar", loop.Name) << ","
					<< kvStr("id", loop.Id) << ","
					<< kvUlong("channel", loop.Channel) << ","
					<< kvUlong("length", loop.Length) << ","
					<< kvUlong("bodyPlayIndex", loop.BodyPlayIndex) << ","
					<< kvDouble("level", loop.Level) << ","
					<< kvDouble("speed", loop.Speed) << ","
					<< kvUlong("mutegroups", loop.MuteGroups) << ","
					<< kvUlong("selectgroups", loop.SelectGroups) << ","
					<< kvBool("muted", loop.Muted) << ","
					<< quoted("mix") << ":" << mixToJson(loop.Mix);
				if (!loop.VstChain.empty())
					ss << "," << quoted("vst") << ":" << vstChainToJson(loop.VstChain);
				ss << "}";
			}

			ss << "]";
			ss << "," << kvUlong("midiPlayIndex", take.MidiPlayIndex)
				<< "," << kvUlong("midiPlayLength", take.MidiPlayLength)
				<< "," << kvStr("midiQuantTransportStart", std::to_string(take.MidiQuantTransportStart))
				<< "," << quoted("midiStreams") << ":" << midiStreamsToJson(take.MidiStreams);
			if (!take.VstChain.empty())
				ss << "," << quoted("vst") << ":" << vstChainToJson(take.VstChain);
			ss << "}";
		}

		ss << "]";
		if (!station.AllowedMidiChannels.empty())
			ss << "," << kvIntArray("allowedmidichannels", station.AllowedMidiChannels);
		if (!station.MidiRoutes.empty())
			ss << "," << quoted("midiRoutes") << ":" << midiRoutesToJson(station.MidiRoutes);
		if (!station.VstChain.empty())
			ss << "," << quoted("vst") << ":" << vstChainToJson(station.VstChain);
		ss << "}";
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

std::optional<JamFile::VstEntry> JamFile::VstEntry::FromJson(Json::JsonPart json)
{
	VstEntry entry;

	auto iter = json.KeyValues.find("path");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["path"].index() == 4)
			entry.Path = std::get<std::string>(json.KeyValues["path"]);
	}

	if (entry.Path.empty())
		return std::nullopt;

	iter = json.KeyValues.find("bypass");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["bypass"].index() == 0)
			entry.Bypass = std::get<bool>(json.KeyValues["bypass"]);
	}

	iter = json.KeyValues.find("state");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["state"].index() == 4)
			entry.State = std::get<std::string>(json.KeyValues["state"]);
	}

	return entry;
}

std::string JamFile::VstEntry::EncodeState(const std::vector<std::uint8_t>& blob)
{
	return utils::Base64Encode(blob);
}

std::vector<std::uint8_t> JamFile::VstEntry::DecodeState() const
{
	return utils::Base64Decode(State);
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
	unsigned long bodyPlayIndex = 0;
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

	if ((0 == length) || length > MaxLoopLengthSamps || name.empty() || !IsSafeSidecarPath(name))
		return std::nullopt;

	iter = json.KeyValues.find("sidecar");
	if (iter != json.KeyValues.end())
	{
		if (iter->second.index() != 4)
			return std::nullopt;
		name = std::get<std::string>(iter->second);
		if (!IsSafeSidecarPath(name))
			return std::nullopt;
	}

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

	iter = json.KeyValues.find("bodyPlayIndex");
	if (iter != json.KeyValues.end())
	{
		if (iter->second.index() != 2)
			return std::nullopt;
		bodyPlayIndex = std::get<unsigned long>(iter->second);
		if (bodyPlayIndex >= length)
			return std::nullopt;
	}
	else
		bodyPlayIndex = index < length ? index : 0u;

	iter = json.KeyValues.find("level");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["level"].index() == 3)
			level = std::get<double>(json.KeyValues["level"]);
	}
	if (!std::isfinite(level))
		return std::nullopt;

	iter = json.KeyValues.find("speed");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["speed"].index() == 3)
			speed = std::get<double>(json.KeyValues["speed"]);
	}
	if (!std::isfinite(speed) || speed <= 0.0)
		return std::nullopt;

	std::string id;
	unsigned int channel = 0u;
	iter = json.KeyValues.find("id");
	if (iter != json.KeyValues.end())
	{
		if (iter->second.index() != 4 || std::get<std::string>(iter->second).size() > 1024u)
			return std::nullopt;
		id = std::get<std::string>(iter->second);
	}
	iter = json.KeyValues.find("channel");
	if (iter != json.KeyValues.end())
	{
		if (iter->second.index() != 2 || std::get<unsigned long>(iter->second) >= MaxLoopsPerTake)
			return std::nullopt;
		channel = static_cast<unsigned int>(std::get<unsigned long>(iter->second));
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

	std::vector<VstEntry> vstChain;
	iter = json.KeyValues.find("vst");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["vst"].index() == 5)
		{
			auto arr = std::get<Json::JsonArray>(json.KeyValues["vst"]);
			if (arr.Array.index() == 5)
			{
				for (auto& entryJson : std::get<std::vector<Json::JsonPart>>(arr.Array))
				{
					auto entryOpt = VstEntry::FromJson(entryJson);
					if (entryOpt.has_value())
						vstChain.push_back(entryOpt.value());
				}
			}
		}
	}

	Loop loop;
	loop.Name = name;
	loop.Id = id;
	loop.Channel = channel;
	loop.Length = length;
	loop.Index = index;
	loop.MasterLoopCount = masterLoopCount;
	loop.BodyPlayIndex = bodyPlayIndex;
	loop.Level = level;
	loop.Speed = speed;
	loop.MuteGroups = muteGroups;
	loop.SelectGroups = selectGroups;
	loop.Muted = isMuted;
	loop.Mix = mix;
	loop.VstChain = vstChain;
	return loop;
}

std::optional<JamFile::LoopTake> JamFile::LoopTake::FromJson(Json::JsonPart json)
{
	std::string name;
	std::vector<Loop> loops;
	std::vector<VstEntry> vstChain;
	bool midiQuantEnabled = false;
	int midiQuantFraction = static_cast<int>(midi::MidiQuantisationFraction::Quarter);
	std::int32_t takePhaseOffsetSamps = 0;

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
				if (loopArray.size() > MaxLoopsPerTake)
					return std::nullopt;
				for (auto loopJson : loopArray)
				{
					auto loop = Loop::FromJson(loopJson);
					if (loop.has_value())
						loops.push_back(loop.value());
				}
			}
		}
	}

	if (name.empty())
		return std::nullopt;

	iter = json.KeyValues.find("takephaseoffsetsamps");
	if (iter != json.KeyValues.end())
		takePhaseOffsetSamps = ParseInt32Clamped(json.KeyValues["takephaseoffsetsamps"], 0);

	iter = json.KeyValues.find("midiquantenabled");
	if (iter != json.KeyValues.end())
	{
		auto& value = json.KeyValues["midiquantenabled"];
		switch (value.index())
		{
		case 0:
			midiQuantEnabled = std::get<bool>(value);
			break;
		case 1:
			midiQuantEnabled = (0 != std::get<long>(value));
			break;
		case 2:
			midiQuantEnabled = (0ul != std::get<unsigned long>(value));
			break;
		case 3:
			midiQuantEnabled = (0.0 != std::get<double>(value));
			break;
		default:
			break;
		}
	}

	iter = json.KeyValues.find("midiquantfraction");
	if (iter != json.KeyValues.end())
	{
		auto& value = json.KeyValues["midiquantfraction"];
		if (value.index() == 1)
			midiQuantFraction = static_cast<int>(std::get<long>(value));
		else if (value.index() == 2)
			midiQuantFraction = static_cast<int>(std::get<unsigned long>(value));
		else if (value.index() == 3)
			midiQuantFraction = static_cast<int>(std::get<double>(value));

		midiQuantFraction = midi::MidiQuantisation::FractionIndex(
			midi::MidiQuantisation::ClampFractionIndex(midiQuantFraction));
	}

	iter = json.KeyValues.find("vst");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["vst"].index() == 5)
		{
			auto arr = std::get<Json::JsonArray>(json.KeyValues["vst"]);
			if (arr.Array.index() == 5)
			{
				for (auto& entryJson : std::get<std::vector<Json::JsonPart>>(arr.Array))
				{
					auto entryOpt = VstEntry::FromJson(entryJson);
					if (entryOpt.has_value())
						vstChain.push_back(entryOpt.value());
				}
			}
		}
	}

	std::vector<MidiStream> midiStreams;
	unsigned long midiPlayIndex = 0u;
	unsigned long midiPlayLength = 0u;
	std::uint64_t midiQuantTransportStart = 0u;
	if ((iter = json.KeyValues.find("midiPlayIndex")) != json.KeyValues.end() && iter->second.index() == 2)
		midiPlayIndex = std::get<unsigned long>(iter->second);
	if ((iter = json.KeyValues.find("midiPlayLength")) != json.KeyValues.end() && iter->second.index() == 2)
		midiPlayLength = std::get<unsigned long>(iter->second);
	if ((iter = json.KeyValues.find("midiQuantTransportStart")) != json.KeyValues.end())
	{
		if (iter->second.index() != 4)
			return std::nullopt;
		const auto parsed = ParseStrictUint64(std::get<std::string>(iter->second));
		if (!parsed.has_value()) return std::nullopt;
		midiQuantTransportStart = *parsed;
	}
	if ((iter = json.KeyValues.find("midiStreams")) != json.KeyValues.end())
	{
		if (iter->second.index() != 5)
			return std::nullopt;
		const auto& array = std::get<Json::JsonArray>(iter->second);
		// Json's legacy array representation has no element-type for an empty
		// array.  An empty current-schema stream list is valid for an audio take.
		const auto hasLegacyEmptyArray = array.Array.index() == 0u
			&& std::get<std::vector<bool>>(array.Array).empty();
		if (!hasLegacyEmptyArray && (array.Array.index() != 5 || std::get<std::vector<Json::JsonPart>>(array.Array).size() > MaxMidiStreamsPerTake))
			return std::nullopt;
		const auto* streamObjects = hasLegacyEmptyArray ? nullptr :
			std::get_if<std::vector<Json::JsonPart>>(&array.Array);
		if (streamObjects)
		for (const auto& streamJson : *streamObjects)
		{
			const auto sidecar = Json::GetString(streamJson, "sidecar");
			const auto device = Json::GetString(streamJson, "device");
			const auto origin = Json::GetString(streamJson, "automationGlobalSampleOrigin");
			const auto channel = Json::GetUnsigned(streamJson, "channel");
			const auto length = Json::GetUnsigned(streamJson, "logicalLength");
			if (!sidecar || !origin || !channel || !length || !IsSafeSidecarPath(*sidecar) || *channel > 15u || *length == 0u || *length > MaxLoopLengthSamps)
			{
				std::cout << "JamFile: skipped invalid MIDI stream" << std::endl;
				continue;
			}
			const auto parsedOrigin = ParseStrictUint64(*origin);
			if (parsedOrigin.has_value())
			{
				MidiStream stream;
				stream.SidecarPath = *sidecar;
				stream.Device = device.value_or("");
				stream.Channel = *channel;
				stream.LogicalLength = *length;
				stream.AutomationGlobalSampleOrigin = *parsedOrigin;
				midiStreams.push_back(std::move(stream));
			}
			else
			{
				std::cout << "JamFile: skipped MIDI stream with invalid origin" << std::endl;
			}
		}
	}
	if (loops.empty() && midiStreams.empty())
		return std::nullopt;

	LoopTake take;
	take.Name = name;
	take.Loops = loops;
	take.VstChain = vstChain;
	take.MidiQuantEnabled = midiQuantEnabled;
	take.MidiQuantFraction = midiQuantFraction;
	take.TakePhaseOffsetSamps = takePhaseOffsetSamps;
	take.MidiPlayIndex = midiPlayIndex;
	take.MidiPlayLength = midiPlayLength;
	take.MidiQuantTransportStart = midiQuantTransportStart;
	take.MidiStreams = std::move(midiStreams);
	return take;
}

std::optional<JamFile::Station> JamFile::Station::FromJson(Json::JsonPart json)
{
	std::string name;
	unsigned int stationType = 0;
	std::vector<LoopTake> takes;
	std::int32_t stationPhaseOffsetSamps = 0;
	std::vector<int> allowedMidiChannels;

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
				if (takeArray.size() > MaxTakesPerStation)
					return std::nullopt;
				for (auto takeJson : takeArray)
				{
					auto take = LoopTake::FromJson(takeJson);
					if (take.has_value())
						takes.push_back(take.value());
				}
			}
		}
	}

	iter = json.KeyValues.find("stationphaseoffsetsamps");
	if (iter != json.KeyValues.end())
		stationPhaseOffsetSamps = ParseInt32Clamped(json.KeyValues["stationphaseoffsetsamps"], 0);

	iter = json.KeyValues.find("allowedmidichannels");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["allowedmidichannels"].index() == 5)
		{
			auto arr = std::get<Json::JsonArray>(json.KeyValues["allowedmidichannels"]);
			if (arr.Array.index() == 2)
			{
				for (auto value : std::get<std::vector<unsigned long>>(arr.Array))
				{
					const auto channel = static_cast<int>(value);
					if (channel >= 1 && channel <= 16)
						allowedMidiChannels.push_back(channel);
				}
			}
			else if (arr.Array.index() == 1)
			{
				for (auto value : std::get<std::vector<long>>(arr.Array))
				{
					const auto channel = static_cast<int>(value);
					if (channel >= 1 && channel <= 16)
						allowedMidiChannels.push_back(channel);
				}
			}
			else if (arr.Array.index() == 3)
			{
				for (auto value : std::get<std::vector<double>>(arr.Array))
				{
					const auto channel = static_cast<int>(value);
					if (channel >= 1 && channel <= 16)
						allowedMidiChannels.push_back(channel);
				}
			}
		}
	}

	std::vector<VstEntry> vstChain;
	iter = json.KeyValues.find("vst");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["vst"].index() == 5)
		{
			auto arr = std::get<Json::JsonArray>(json.KeyValues["vst"]);
			if (arr.Array.index() == 5)
			{
				for (auto& entryJson : std::get<std::vector<Json::JsonPart>>(arr.Array))
				{
					auto entryOpt = VstEntry::FromJson(entryJson);
					if (entryOpt.has_value())
						vstChain.push_back(entryOpt.value());
				}
			}
		}
	}

	std::vector<MidiRoute> midiRoutes;
	iter = json.KeyValues.find("midiRoutes");
	if (iter != json.KeyValues.end() && iter->second.index() == 5)
	{
		const auto& routes = std::get<Json::JsonArray>(iter->second);
		if (routes.Array.index() == 5)
		{
			for (const auto& routeJson : std::get<std::vector<Json::JsonPart>>(routes.Array))
			{
				const auto output = Json::GetUnsigned(routeJson, "outputIndex");
				const auto plugin = Json::GetUnsigned(routeJson, "pluginIndex");
				auto liveIter = routeJson.KeyValues.find("live");
				if (output.has_value() && plugin.has_value()
					&& liveIter != routeJson.KeyValues.end() && liveIter->second.index() == 0)
					midiRoutes.push_back(MidiRoute{ *output, std::get<bool>(liveIter->second), *plugin });
				else
					std::cout << "JamFile: skipped invalid MIDI route" << std::endl;
			}
		}
	}

	if (name.empty())
		return std::nullopt;

	Station station;
	station.Name = name;
	station.StationType = stationType;
	station.LoopTakes = takes;
	station.VstChain = vstChain;
	station.StationPhaseOffsetSamps = stationPhaseOffsetSamps;
	std::sort(allowedMidiChannels.begin(), allowedMidiChannels.end());
	allowedMidiChannels.erase(std::unique(allowedMidiChannels.begin(), allowedMidiChannels.end()), allowedMidiChannels.end());
	station.AllowedMidiChannels = std::move(allowedMidiChannels);
	station.MidiRoutes = std::move(midiRoutes);
	return station;
}
