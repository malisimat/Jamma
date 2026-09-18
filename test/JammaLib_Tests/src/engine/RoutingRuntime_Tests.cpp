#include "engine/RoutingRuntime.h"
#include "engine/Station.h"
#include "gui/GuiHud.h"

class RoutingRuntimeTest : public testing::Test
{
protected:
	static io::JamFile::Station StationDescriptor(std::string name)
	{
		io::JamFile::Station station{};
		station.Name = std::move(name);
		return station;
	}

	static io::RigFile::Trigger TriggerDescriptor(std::string name,
		std::optional<std::string> target,
		io::RigFile::Trigger::MidiInputMode midiMode = io::RigFile::Trigger::MidiInputMode::None)
	{
		io::RigFile::Trigger trigger{};
		trigger.Name = std::move(name);
		trigger.StationTarget = std::move(target);
		trigger.MidiInputs = midiMode;
		return trigger;
	}

	static std::shared_ptr<engine::Station> RuntimeStation(std::string name)
	{
		engine::StationParams params;
		params.Name = std::move(name);
		params.Size = { 100u, 100u };
		audio::MergeMixBehaviourParams merge;
		return std::make_shared<engine::Station>(params, engine::Station::GetMixerParams(params.Size, merge));
	}
};

TEST_F(RoutingRuntimeTest, BuildsEveryTriggerAndResolvesTargetsByName)
{
	io::RigFile rig{};
	rig.Triggers = {
		TriggerDescriptor("to-drums", "Drums"),
		TriggerDescriptor("to-bass", "Bass"),
		TriggerDescriptor("missing", "Missing"),
		TriggerDescriptor("unbound", std::string())
	};
	const std::vector<io::JamFile::Station> stations = {
		StationDescriptor("Bass"),
		StationDescriptor("Drums")
	};

	const auto runtime = engine::RoutingRuntime::BuildInitial(rig, stations, 2u, {}, engine::TriggerParams());

	ASSERT_EQ(4u, runtime.Graph.Triggers.size());
	ASSERT_EQ(4u, runtime.Triggers.size());
	ASSERT_TRUE(runtime.Triggers[0].StationIndex.has_value());
	EXPECT_EQ(1u, runtime.Triggers[0].StationIndex.value());
	ASSERT_TRUE(runtime.Triggers[1].StationIndex.has_value());
	EXPECT_EQ(0u, runtime.Triggers[1].StationIndex.value());
	EXPECT_FALSE(runtime.Triggers[2].StationIndex.has_value());
	EXPECT_FALSE(runtime.Triggers[3].StationIndex.has_value());
	EXPECT_EQ(io::RigRouting::Warning::TargetMissing, runtime.Graph.Triggers[2].Reason);
	EXPECT_EQ(io::RigRouting::Warning::None, runtime.Graph.Triggers[3].Reason);
}

TEST_F(RoutingRuntimeTest, ResolvesLegacyTargetWithoutAdoptingUnsavedMigration)
{
	io::RigFile rig{};
	rig.Triggers = { TriggerDescriptor("legacy", std::nullopt) };

	const auto runtime = engine::RoutingRuntime::BuildInitial(rig,
		{ StationDescriptor("Station") },
		0u,
		{},
		engine::TriggerParams());

	ASSERT_EQ(1u, runtime.Triggers.size());
	EXPECT_EQ(0u, runtime.Triggers[0].StationIndex.value());
	EXPECT_FALSE(runtime.Rig.Triggers[0].StationTarget.has_value());
	EXPECT_EQ(io::RigRouting::Warning::LegacyStationTargetMigrated, runtime.Graph.Triggers[0].Reason);
}

TEST_F(RoutingRuntimeTest, RetainsUnavailableSourcesAndManyToOneMembershipValues)
{
	io::RigFile rig{};
	auto first = TriggerDescriptor("first", "Shared", io::RigFile::Trigger::MidiInputMode::Selected);
	first.InputChannels = { 0u, 3u };
	first.MidiInputDevices = { "Missing keyboard" };
	auto second = TriggerDescriptor("second", "Shared", io::RigFile::Trigger::MidiInputMode::Any);
	rig.Triggers = { first, second };

	const auto runtime = engine::RoutingRuntime::BuildInitial(rig,
		{ StationDescriptor("Shared"), StationDescriptor("Other") },
		1u,
		{ "Present keyboard" },
		engine::TriggerParams());

	ASSERT_EQ(2u, runtime.Triggers.size());
	EXPECT_EQ(0u, runtime.Triggers[0].StationIndex.value());
	EXPECT_EQ(0u, runtime.Triggers[1].StationIndex.value());
	ASSERT_EQ(3u, runtime.Graph.Triggers[0].Sources.size());
	EXPECT_TRUE(runtime.Graph.Triggers[0].Sources[0].Available);
	EXPECT_FALSE(runtime.Graph.Triggers[0].Sources[1].Available);
	EXPECT_FALSE(runtime.Graph.Triggers[0].Sources[2].Available);
	ASSERT_EQ(1u, runtime.Graph.Triggers[1].Sources.size());
	EXPECT_EQ("*", runtime.Graph.Triggers[1].Sources[0].MidiDevice);
}

TEST_F(RoutingRuntimeTest, PreservesExplicitMidiModesForLiveInputEligibility)
{
	auto noneConfig = TriggerDescriptor("none", "Station", io::RigFile::Trigger::MidiInputMode::None);
	auto anyConfig = TriggerDescriptor("any", "Station", io::RigFile::Trigger::MidiInputMode::Any);
	auto selectedConfig = TriggerDescriptor("selected", "Station", io::RigFile::Trigger::MidiInputMode::Selected);
	selectedConfig.MidiInputDevices = { "Keys" };

	auto none = engine::Trigger::FromFile(engine::TriggerParams(), noneConfig);
	auto any = engine::Trigger::FromFile(engine::TriggerParams(), anyConfig);
	auto selected = engine::Trigger::FromFile(engine::TriggerParams(), selectedConfig);
	ASSERT_TRUE(none.has_value());
	ASSERT_TRUE(any.has_value());
	ASSERT_TRUE(selected.has_value());

	auto station = RuntimeStation("Station");
	station->AddTrigger(none.value());
	EXPECT_FALSE(station->AcceptsLiveMidiFromDevice("Keys"));
	station->AddTrigger(selected.value());
	EXPECT_TRUE(station->AcceptsLiveMidiFromDevice("Keys"));
	EXPECT_FALSE(station->AcceptsLiveMidiFromDevice("Other"));
	station->AddTrigger(any.value());
	EXPECT_TRUE(station->AcceptsLiveMidiFromDevice("Other"));

	auto noRoutes = RuntimeStation("No routes");
	EXPECT_FALSE(noRoutes->AcceptsLiveMidiFromDevice("Keys"));
}

TEST_F(RoutingRuntimeTest, HudCableRoutesContainOnlyResolvedGraphEdges)
{
	engine::RoutingGraph graph;
	io::RigRouting::TriggerResolution resolved;
	resolved.TriggerIndex = 0u;
	resolved.TriggerName = "resolved";
	resolved.StationIndex = 3u;
	resolved.Sources.push_back({ io::RigRouting::SourceKind::Adc, 1u, {}, true });
	io::RigRouting::TriggerResolution unbound;
	unbound.TriggerIndex = 1u;
	unbound.TriggerName = "unbound";
	unbound.Sources.push_back({ io::RigRouting::SourceKind::Midi, 0u, "Keys", false });
	graph.Triggers = { resolved, unbound };

	const auto routes = gui::GuiHud::BuildCableRoutes(graph);

	ASSERT_EQ(3u, routes.size());
	EXPECT_EQ(gui::GuiHud::CableRoute::Kind::Capture, routes[0].RouteKind);
	EXPECT_EQ(0u, routes[0].TriggerIndex);
	EXPECT_EQ(gui::GuiHud::CableRoute::Kind::Station, routes[1].RouteKind);
	EXPECT_EQ(3u, routes[1].StationIndex.value());
	EXPECT_EQ(gui::GuiHud::CableRoute::Kind::Capture, routes[2].RouteKind);
	EXPECT_EQ(1u, routes[2].TriggerIndex);
	EXPECT_FALSE(routes[2].Source->Available);
}
