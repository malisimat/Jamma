
#include "gtest/gtest.h"
#include <regex>
#include "resources/ResourceLib.h"
#include "io/Json.h"
#include "io/JamFile.h"

using io::Json;
using io::JamFile;

const std::string LoopString = "{\"name\":\"%NAME%\",\"length\":220,\"index\":%INDEX%,\"masterloopcount\":7,\"level\":0.56,\"speed\":1.2,\"mutegroups\":11,\"selectgroups\":15,\"muted\":false,\"mix\":{\"type\":\"pan\",\"chans\":[0.2,0.8]}}";

TEST(JamFile, ParsesLoop) {
	auto str = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop"), std::regex("%INDEX%"), "2");
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto loop = JamFile::Loop::FromJson(json);

	ASSERT_TRUE(loop.has_value());
	ASSERT_EQ(0, loop.value().Name.compare("loop"));
	ASSERT_EQ(2, loop.value().Index);
	ASSERT_EQ(7, loop.value().MasterLoopCount);
	ASSERT_EQ(0.56, loop.value().Level);
	ASSERT_EQ(1.2, loop.value().Speed);
	ASSERT_EQ(11, loop.value().MuteGroups);
	ASSERT_EQ(15, loop.value().SelectGroups);
	ASSERT_EQ(false, loop.value().Muted);

	auto loopMix = loop.value().Mix;
	auto loopMixParams = std::get<std::vector<double>>(loopMix.Params);

	ASSERT_EQ(2, loopMixParams.size());
	ASSERT_EQ(0.2, loopMixParams[0]);
	ASSERT_EQ(0.8, loopMixParams[1]);
}

TEST(JamFile, ParsesLoopTake) {
	auto loop1 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop1"), std::regex("%INDEX%"), "1");
	auto loop2 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop2"), std::regex("%INDEX%"), "2");
	auto loop3 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop3"), std::regex("%INDEX%"), "3");

	auto str = "{\"name\":\"take\",\"loops\":[" + loop1 + "," + loop2 + "," + loop3 + "]}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto take = JamFile::LoopTake::FromJson(json);

	ASSERT_TRUE(take.has_value());
	ASSERT_EQ(0, take.value().Name.compare("take"));
	ASSERT_EQ(3, take.value().Loops.size());

	ASSERT_EQ(0, take.value().Loops[0].Name.compare("loop1"));
	ASSERT_EQ(1, take.value().Loops[0].Index);
	ASSERT_EQ(1.2, take.value().Loops[0].Speed);

	ASSERT_EQ(0, take.value().Loops[1].Name.compare("loop2"));
	ASSERT_EQ(2, take.value().Loops[1].Index);
	ASSERT_EQ(1.2, take.value().Loops[1].Speed);

	ASSERT_EQ(0, take.value().Loops[2].Name.compare("loop3"));
	ASSERT_EQ(3, take.value().Loops[2].Index);
	ASSERT_EQ(1.2, take.value().Loops[2].Speed);
}

TEST(JamFile, ParsesStation) {
	auto loop1 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop1"), std::regex("%INDEX%"), "1");
	auto loop2 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop2"), std::regex("%INDEX%"), "2");
	auto take1 = "{\"name\":\"take1\",\"loops\":[" + loop1 + "," + loop2 + "]}";

	auto loop3 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop3"), std::regex("%INDEX%"), "3");
	auto take2 = "{\"name\":\"take2\",\"loops\":[" + loop3 + "]}";

	auto str = "{\"name\":\"station\",\"takes\":[" + take1 + "," + take2 + "]}";
	auto testStream = std::stringstream(str);
	auto json = std::get<Json::JsonPart>(Json::FromStream(std::move(testStream)).value());
	auto station = JamFile::Station::FromJson(json);

	ASSERT_TRUE(station.has_value());
	ASSERT_EQ(0, station.value().Name.compare("station"));
	ASSERT_EQ(2, station.value().LoopTakes.size());

	ASSERT_EQ(0, station.value().LoopTakes[0].Name.compare("take1"));
	ASSERT_EQ(2, station.value().LoopTakes[0].Loops.size());

	ASSERT_EQ(0, station.value().LoopTakes[0].Loops[0].Name.compare("loop1"));
	ASSERT_EQ(1, station.value().LoopTakes[0].Loops[0].Index);
	ASSERT_EQ(1.2, station.value().LoopTakes[0].Loops[0].Speed);

	ASSERT_EQ(0, station.value().LoopTakes[0].Loops[1].Name.compare("loop2"));
	ASSERT_EQ(2, station.value().LoopTakes[0].Loops[1].Index);
	ASSERT_EQ(1.2, station.value().LoopTakes[0].Loops[1].Speed);

	ASSERT_EQ(0, station.value().LoopTakes[1].Name.compare("take2"));
	ASSERT_EQ(1, station.value().LoopTakes[1].Loops.size());

	ASSERT_EQ(0, station.value().LoopTakes[1].Loops[0].Name.compare("loop3"));
	ASSERT_EQ(3, station.value().LoopTakes[1].Loops[0].Index);
	ASSERT_EQ(1.2, station.value().LoopTakes[1].Loops[0].Speed);
}

TEST(JamFile, ParsesFile) {

	auto loop1 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop1"), std::regex("%INDEX%"), "1");
	auto loop2 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop2"), std::regex("%INDEX%"), "2");
	auto take1 = "{\"name\":\"take1\",\"loops\":[" + loop1 + "," + loop2 + "]}";

	auto loop3 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop3"), std::regex("%INDEX%"), "3");
	auto take2 = "{\"name\":\"take2\",\"loops\":[" + loop3 + "]}";

	auto station1 = "{\"name\":\"station1\",\"takes\":[" + take1 + "," + take2 + "]}";

	auto loop4 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop4"), std::regex("%INDEX%"), "4");
	auto loop5 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop5"), std::regex("%INDEX%"), "5");
	auto take3 = "{\"name\":\"take3\",\"loops\":[" + loop4 + "," + loop5 + "]}";
	
	auto loop6 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop6"), std::regex("%INDEX%"), "6");
	auto take4 = "{\"name\":\"take4\",\"loops\":[" + loop6 + "]}";

	auto loop7 = std::regex_replace(std::regex_replace(LoopString, std::regex("%NAME%"), "loop7"), std::regex("%INDEX%"), "7");
	auto take5 = "{\"name\":\"take5\",\"loops\":[" + loop7 + "]}";

	auto station2 = "{\"name\":\"station2\",\"takes\":[" + take3 + "," + take4 + "," + take5 + "]}";

	auto quant = "\"quantisesamps\":4321,\"quantisation\":\"power\"";
	auto str = "{\"name\":\"jam\",\"stations\":[" + station1 + "," + station2 + "]," + quant + "}";
	auto testStream = std::stringstream(str);
	auto jam = JamFile::FromStream(std::move(testStream));

	ASSERT_TRUE(jam.has_value());
	ASSERT_EQ(JamFile::VERSION_V, jam.value().Version);
	ASSERT_EQ(0, jam.value().Name.compare("jam"));
	ASSERT_EQ(2, jam.value().Stations.size());


	ASSERT_EQ(0, jam.value().Stations[0].Name.compare("station1"));
	ASSERT_EQ(2, jam.value().Stations[0].LoopTakes.size());

	ASSERT_EQ(0, jam.value().Stations[0].LoopTakes[0].Name.compare("take1"));
	ASSERT_EQ(2, jam.value().Stations[0].LoopTakes[0].Loops.size());

	ASSERT_EQ(0, jam.value().Stations[0].LoopTakes[0].Loops[0].Name.compare("loop1"));
	ASSERT_EQ(1, jam.value().Stations[0].LoopTakes[0].Loops[0].Index);
	ASSERT_EQ(1.2, jam.value().Stations[0].LoopTakes[0].Loops[0].Speed);

	ASSERT_EQ(0, jam.value().Stations[0].LoopTakes[0].Loops[1].Name.compare("loop2"));
	ASSERT_EQ(2, jam.value().Stations[0].LoopTakes[0].Loops[1].Index);
	ASSERT_EQ(1.2, jam.value().Stations[0].LoopTakes[0].Loops[1].Speed);

	ASSERT_EQ(0, jam.value().Stations[0].LoopTakes[1].Name.compare("take2"));
	ASSERT_EQ(1, jam.value().Stations[0].LoopTakes[1].Loops.size());

	ASSERT_EQ(0, jam.value().Stations[0].LoopTakes[1].Loops[0].Name.compare("loop3"));
	ASSERT_EQ(3, jam.value().Stations[0].LoopTakes[1].Loops[0].Index);
	ASSERT_EQ(1.2, jam.value().Stations[0].LoopTakes[1].Loops[0].Speed);


	ASSERT_EQ(0, jam.value().Stations[1].Name.compare("station2"));
	ASSERT_EQ(3, jam.value().Stations[1].LoopTakes.size());

	ASSERT_EQ(0, jam.value().Stations[1].LoopTakes[0].Name.compare("take3"));
	ASSERT_EQ(2, jam.value().Stations[1].LoopTakes[0].Loops.size());

	ASSERT_EQ(0, jam.value().Stations[1].LoopTakes[0].Loops[0].Name.compare("loop4"));
	ASSERT_EQ(4, jam.value().Stations[1].LoopTakes[0].Loops[0].Index);
	ASSERT_EQ(1.2, jam.value().Stations[1].LoopTakes[0].Loops[0].Speed);

	ASSERT_EQ(0, jam.value().Stations[1].LoopTakes[0].Loops[1].Name.compare("loop5"));
	ASSERT_EQ(5, jam.value().Stations[1].LoopTakes[0].Loops[1].Index);
	ASSERT_EQ(1.2, jam.value().Stations[1].LoopTakes[0].Loops[1].Speed);

	ASSERT_EQ(0, jam.value().Stations[1].LoopTakes[1].Name.compare("take4"));
	ASSERT_EQ(1, jam.value().Stations[1].LoopTakes[1].Loops.size());

	ASSERT_EQ(0, jam.value().Stations[1].LoopTakes[1].Loops[0].Name.compare("loop6"));
	ASSERT_EQ(6, jam.value().Stations[1].LoopTakes[1].Loops[0].Index);
	ASSERT_EQ(1.2, jam.value().Stations[1].LoopTakes[1].Loops[0].Speed);

	ASSERT_EQ(0, jam.value().Stations[1].LoopTakes[2].Name.compare("take5"));
	ASSERT_EQ(1, jam.value().Stations[1].LoopTakes[2].Loops.size());

	ASSERT_EQ(0, jam.value().Stations[1].LoopTakes[2].Loops[0].Name.compare("loop7"));
	ASSERT_EQ(7, jam.value().Stations[1].LoopTakes[2].Loops[0].Index);
	ASSERT_EQ(1.2, jam.value().Stations[1].LoopTakes[2].Loops[0].Speed);

	ASSERT_EQ(4321, jam.value().QuantiseSamps);
	ASSERT_EQ(engine::Timer::QUANTISE_POWER, jam.value().Quantisation);
}

TEST(JamFile, RoundTripsFileWithIntegerValuedDoubles) {
	JamFile jam;
	jam.Version = JamFile::VERSION_V;
	jam.Name = "jam\"name\\path";
	jam.TimerTicks = 12;
	jam.QuantiseSamps = 960;
	jam.Quantisation = engine::Timer::QUANTISE_MULTIPLE;

	JamFile::Loop loop;
	loop.Name = "loop\"1.wav";
	loop.Length = 220;
	loop.Index = 3;
	loop.MasterLoopCount = 1;
	loop.Level = 1.0;
	loop.Speed = 2.0;
	loop.MuteGroups = 0;
	loop.SelectGroups = 0;
	loop.Muted = false;
	loop.Mix.Mix = JamFile::LoopMix::MIX_PAN;
	loop.Mix.Params = std::vector<double>{ 1.0, 0.0 };

	JamFile::LoopTake take;
	take.Name = "take";
	take.Loops.push_back(loop);

	JamFile::Station station;
	station.Name = "station";
	station.StationType = 0;
	station.LoopTakes.push_back(take);

	jam.Stations.push_back(station);

	std::stringstream out;
	ASSERT_TRUE(JamFile::ToStream(jam, out));

	auto parsed = JamFile::FromStream(std::move(out));
	ASSERT_TRUE(parsed.has_value());
	ASSERT_EQ("jam\"name\\path", parsed->Name);
	ASSERT_EQ(12, parsed->TimerTicks);
	ASSERT_EQ(960, parsed->QuantiseSamps);
	ASSERT_EQ(engine::Timer::QUANTISE_MULTIPLE, parsed->Quantisation);
	ASSERT_EQ(1, parsed->Stations.size());
	ASSERT_EQ(1, parsed->Stations[0].LoopTakes.size());
	ASSERT_EQ(1, parsed->Stations[0].LoopTakes[0].Loops.size());

	const auto& parsedLoop = parsed->Stations[0].LoopTakes[0].Loops[0];
	ASSERT_EQ("loop\"1.wav", parsedLoop.Name);
	ASSERT_EQ(1.0, parsedLoop.Level);
	ASSERT_EQ(2.0, parsedLoop.Speed);
	ASSERT_EQ(JamFile::LoopMix::MIX_PAN, parsedLoop.Mix.Mix);
	ASSERT_EQ(1, parsedLoop.Mix.Params.index());

	auto panParamsPtr = std::get_if<std::vector<double>>(&parsedLoop.Mix.Params);
	ASSERT_NE(nullptr, panParamsPtr);
	auto panParams = *panParamsPtr;
	ASSERT_EQ(2, panParams.size());
	ASSERT_EQ(1.0, panParams[0]);
	ASSERT_EQ(0.0, panParams[1]);
}
