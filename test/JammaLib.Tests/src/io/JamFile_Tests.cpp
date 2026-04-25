
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

// ---------------------------------------------------------------------------
// ToStream -> FromStream round-trip tests
// ---------------------------------------------------------------------------

namespace
{
	// Helper: build a minimal Loop with a pan mix.
	JamFile::Loop MakePanLoop(const std::string& name,
	                          unsigned long length,
	                          unsigned long index,
	                          double level,
	                          double speed)
	{
		JamFile::LoopMix mix;
		mix.Mix = JamFile::LoopMix::MIX_PAN;
		mix.Params = std::vector<double>{ 0.25, 0.75 };

		JamFile::Loop loop;
		loop.Name            = name;
		loop.Length          = length;
		loop.Index           = index;
		loop.MasterLoopCount = 2;
		loop.Level           = level;
		loop.Speed           = speed;
		loop.MuteGroups      = 0;
		loop.SelectGroups    = 0;
		loop.Muted           = false;
		loop.Mix             = mix;
		return loop;
	}

	// Helper: build a minimal Loop with a wire mix.
	JamFile::Loop MakeWireLoop(const std::string& name)
	{
		JamFile::LoopMix mix;
		mix.Mix    = JamFile::LoopMix::MIX_WIRE;
		mix.Params = std::vector<unsigned long>{ 0, 1 };

		JamFile::Loop loop;
		loop.Name            = name;
		loop.Length          = 44100;
		loop.Index           = 0;
		loop.MasterLoopCount = 0;
		loop.Level           = 1.0;
		loop.Speed           = 1.0;
		loop.MuteGroups      = 0;
		loop.SelectGroups    = 0;
		loop.Muted           = true;
		loop.Mix             = mix;
		return loop;
	}
} // anonymous namespace

TEST(JamFile, ToStream_RoundTrip_Basic) {
	JamFile::LoopTake take;
	take.Name  = "take1";
	take.Loops = { MakePanLoop("loop1.wav", 100000, 42, 0.75, 1.0) };

	JamFile::Station station;
	station.Name       = "station1";
	station.StationType = 0;
	station.LoopTakes  = { take };

	JamFile jam;
	jam.Version       = JamFile::VERSION_V;
	jam.Name          = "mysession";
	jam.TimerTicks    = 12345;
	jam.QuantiseSamps = 77911;
	jam.Quantisation  = engine::Timer::QUANTISE_MULTIPLE;
	jam.Stations      = { station };

	std::stringstream ss;
	ASSERT_TRUE(JamFile::ToStream(jam, ss));

	auto result = JamFile::FromStream(std::move(ss));
	ASSERT_TRUE(result.has_value());

	ASSERT_EQ(0, result.value().Name.compare("mysession"));
	ASSERT_EQ(12345ul, result.value().TimerTicks);
	ASSERT_EQ(77911u, result.value().QuantiseSamps);
	ASSERT_EQ(engine::Timer::QUANTISE_MULTIPLE, result.value().Quantisation);

	ASSERT_EQ(1u, result.value().Stations.size());
	ASSERT_EQ(0, result.value().Stations[0].Name.compare("station1"));

	ASSERT_EQ(1u, result.value().Stations[0].LoopTakes.size());
	ASSERT_EQ(0, result.value().Stations[0].LoopTakes[0].Name.compare("take1"));

	ASSERT_EQ(1u, result.value().Stations[0].LoopTakes[0].Loops.size());
	const auto& lp = result.value().Stations[0].LoopTakes[0].Loops[0];
	ASSERT_EQ(0, lp.Name.compare("loop1.wav"));
	ASSERT_EQ(100000ul, lp.Length);
	ASSERT_EQ(42ul, lp.Index);
	ASSERT_EQ(2ul, lp.MasterLoopCount);
	ASSERT_DOUBLE_EQ(0.75, lp.Level);
	ASSERT_DOUBLE_EQ(1.0,  lp.Speed);
	ASSERT_EQ(false, lp.Muted);

	ASSERT_EQ(JamFile::LoopMix::MIX_PAN, lp.Mix.Mix);
	const auto& chans = std::get<std::vector<double>>(lp.Mix.Params);
	ASSERT_EQ(2u, chans.size());
	ASSERT_DOUBLE_EQ(0.25, chans[0]);
	ASSERT_DOUBLE_EQ(0.75, chans[1]);
}

TEST(JamFile, ToStream_RoundTrip_WireMix) {
	JamFile::LoopTake take;
	take.Name  = "take1";
	take.Loops = { MakeWireLoop("loop1.wav") };

	JamFile::Station station;
	station.Name        = "station1";
	station.StationType = 0;
	station.LoopTakes   = { take };

	JamFile jam;
	jam.Version       = JamFile::VERSION_V;
	jam.Name          = "wiresession";
	jam.TimerTicks    = 0;
	jam.QuantiseSamps = 0;
	jam.Quantisation  = engine::Timer::QUANTISE_OFF;
	jam.Stations      = { station };

	std::stringstream ss;
	ASSERT_TRUE(JamFile::ToStream(jam, ss));

	auto result = JamFile::FromStream(std::move(ss));
	ASSERT_TRUE(result.has_value());

	ASSERT_EQ(0, result.value().Name.compare("wiresession"));
	ASSERT_EQ(engine::Timer::QUANTISE_OFF, result.value().Quantisation);

	ASSERT_EQ(1u, result.value().Stations.size());
	ASSERT_EQ(1u, result.value().Stations[0].LoopTakes.size());
	ASSERT_EQ(1u, result.value().Stations[0].LoopTakes[0].Loops.size());

	const auto& lp = result.value().Stations[0].LoopTakes[0].Loops[0];
	ASSERT_EQ(true, lp.Muted);
	ASSERT_EQ(JamFile::LoopMix::MIX_WIRE, lp.Mix.Mix);
	const auto& chans = std::get<std::vector<unsigned long>>(lp.Mix.Params);
	ASSERT_EQ(2u, chans.size());
	ASSERT_EQ(0ul, chans[0]);
	ASSERT_EQ(1ul, chans[1]);
}

TEST(JamFile, ToStream_RoundTrip_MultipleStations) {
	JamFile::LoopTake take1;
	take1.Name  = "take1";
	take1.Loops = { MakePanLoop("a.wav", 1000, 1, 0.5, 1.0),
	                MakePanLoop("b.wav", 2000, 2, 0.8, 0.5) };

	JamFile::LoopTake take2;
	take2.Name  = "take2";
	take2.Loops = { MakePanLoop("c.wav", 3000, 3, 1.0, 2.0) };

	JamFile::Station st1;
	st1.Name        = "drums";
	st1.StationType = 0;
	st1.LoopTakes   = { take1, take2 };

	JamFile::LoopTake take3;
	take3.Name  = "take3";
	take3.Loops = { MakePanLoop("d.wav", 4000, 4, 0.6, 1.0) };

	JamFile::Station st2;
	st2.Name        = "bass";
	st2.StationType = 0;
	st2.LoopTakes   = { take3 };

	JamFile jam;
	jam.Version       = JamFile::VERSION_V;
	jam.Name          = "multisession";
	jam.TimerTicks    = 99;
	jam.QuantiseSamps = 4410;
	jam.Quantisation  = engine::Timer::QUANTISE_POWER;
	jam.Stations      = { st1, st2 };

	std::stringstream ss;
	ASSERT_TRUE(JamFile::ToStream(jam, ss));

	auto result = JamFile::FromStream(std::move(ss));
	ASSERT_TRUE(result.has_value());

	ASSERT_EQ(0, result.value().Name.compare("multisession"));
	ASSERT_EQ(99ul, result.value().TimerTicks);
	ASSERT_EQ(4410u, result.value().QuantiseSamps);
	ASSERT_EQ(engine::Timer::QUANTISE_POWER, result.value().Quantisation);

	ASSERT_EQ(2u, result.value().Stations.size());

	ASSERT_EQ(0, result.value().Stations[0].Name.compare("drums"));
	ASSERT_EQ(2u, result.value().Stations[0].LoopTakes.size());
	ASSERT_EQ(2u, result.value().Stations[0].LoopTakes[0].Loops.size());
	ASSERT_EQ(1u, result.value().Stations[0].LoopTakes[1].Loops.size());
	ASSERT_EQ(0, result.value().Stations[0].LoopTakes[0].Loops[0].Name.compare("a.wav"));
	ASSERT_EQ(1ul, result.value().Stations[0].LoopTakes[0].Loops[0].Index);
	ASSERT_EQ(0, result.value().Stations[0].LoopTakes[0].Loops[1].Name.compare("b.wav"));
	ASSERT_EQ(2ul, result.value().Stations[0].LoopTakes[0].Loops[1].Index);
	ASSERT_EQ(0, result.value().Stations[0].LoopTakes[1].Loops[0].Name.compare("c.wav"));
	ASSERT_EQ(3ul, result.value().Stations[0].LoopTakes[1].Loops[0].Index);

	ASSERT_EQ(0, result.value().Stations[1].Name.compare("bass"));
	ASSERT_EQ(1u, result.value().Stations[1].LoopTakes.size());
	ASSERT_EQ(0, result.value().Stations[1].LoopTakes[0].Loops[0].Name.compare("d.wav"));
	ASSERT_EQ(4ul, result.value().Stations[1].LoopTakes[0].Loops[0].Index);
}

TEST(JamFile, ToStream_RoundTrip_EmptyStations) {
	JamFile jam;
	jam.Version       = JamFile::VERSION_V;
	jam.Name          = "empty";
	jam.TimerTicks    = 0;
	jam.QuantiseSamps = 0;
	jam.Quantisation  = engine::Timer::QUANTISE_OFF;

	std::stringstream ss;
	ASSERT_TRUE(JamFile::ToStream(jam, ss));

	auto result = JamFile::FromStream(std::move(ss));
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(0, result.value().Name.compare("empty"));
	ASSERT_EQ(0u, result.value().Stations.size());
	ASSERT_EQ(engine::Timer::QUANTISE_OFF, result.value().Quantisation);
}

TEST(JamFile, ToStream_RoundTrip_DoublesPrecision) {
	// Verify that level/speed survive the ToStream→FromStream round-trip
	// accurately regardless of the active locale.
	JamFile::LoopTake take;
	take.Name  = "take1";
	take.Loops = { MakePanLoop("loop.wav", 1000, 0, 0.123456789, 0.987654321) };

	JamFile::Station station;
	station.Name        = "st";
	station.StationType = 0;
	station.LoopTakes   = { take };

	JamFile jam;
	jam.Version       = JamFile::VERSION_V;
	jam.Name          = "precision";
	jam.TimerTicks    = 0;
	jam.QuantiseSamps = 0;
	jam.Quantisation  = engine::Timer::QUANTISE_OFF;
	jam.Stations      = { station };

	std::stringstream ss;
	ASSERT_TRUE(JamFile::ToStream(jam, ss));

	auto result = JamFile::FromStream(std::move(ss));
	ASSERT_TRUE(result.has_value());
	const auto& lp = result.value().Stations[0].LoopTakes[0].Loops[0];
	ASSERT_NEAR(0.123456789, lp.Level, 1e-9);
	ASSERT_NEAR(0.987654321, lp.Speed, 1e-9);
}