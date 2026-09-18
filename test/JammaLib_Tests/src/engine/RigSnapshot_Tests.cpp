#include "engine/RigCoordinator.h"
#include "../TestRigMembership.h"
#include "engine/Station.h"
#include "gui/GuiHud.h"

class RigSnapshotTest : public testing::Test
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

	static engine::RigCoordinator::SnapshotPtr BuildSnapshot(const io::RigFile& rig,
		const std::vector<io::JamFile::Station>& descriptors,
		unsigned int adcChannels,
		const std::vector<std::string>& midiDevices)
	{
		std::vector<std::shared_ptr<engine::Station>> stations;
		for (const auto& descriptor : descriptors)
			stations.push_back(RuntimeStation(descriptor.Name));
		engine::RigCoordinator coordinator;
		if (!coordinator.BuildInitial(rig, descriptors, stations, adcChannels, midiDevices,
			engine::TriggerParams(), [](const io::RigFile&) { return false; }))
			return {};
		return coordinator.Accepted();
	}
};

TEST_F(RigSnapshotTest, BuildsEveryTriggerAndResolvesTargetsByName)
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

	const auto runtime = BuildSnapshot(rig, stations, 2u, {});

	ASSERT_TRUE(runtime);
	ASSERT_EQ(4u, runtime->Graph.Triggers.size());
	ASSERT_EQ(4u, runtime->Triggers.size());
	ASSERT_TRUE(runtime->Triggers[0].StationIndex.has_value());
	EXPECT_EQ(1u, runtime->Triggers[0].StationIndex.value());
	ASSERT_TRUE(runtime->Triggers[1].StationIndex.has_value());
	EXPECT_EQ(0u, runtime->Triggers[1].StationIndex.value());
	EXPECT_FALSE(runtime->Triggers[2].StationIndex.has_value());
	EXPECT_FALSE(runtime->Triggers[3].StationIndex.has_value());
	EXPECT_EQ(io::RigFileRouting::Warning::TargetMissing, runtime->Graph.Triggers[2].Reason);
	EXPECT_EQ(io::RigFileRouting::Warning::None, runtime->Graph.Triggers[3].Reason);
}

TEST_F(RigSnapshotTest, ResolvesLegacyTargetWithoutAdoptingUnsavedMigration)
{
	io::RigFile rig{};
	rig.Triggers = { TriggerDescriptor("legacy", std::nullopt) };

	const auto runtime = BuildSnapshot(rig,
		{ StationDescriptor("Station") },
		0u,
		{});

	ASSERT_TRUE(runtime);
	ASSERT_EQ(1u, runtime->Triggers.size());
	EXPECT_EQ(0u, runtime->Triggers[0].StationIndex.value());
	EXPECT_FALSE(runtime->Rig.Triggers[0].StationTarget.has_value());
	EXPECT_EQ(io::RigFileRouting::Warning::LegacyStationTargetMigrated, runtime->Graph.Triggers[0].Reason);
}

TEST_F(RigSnapshotTest, RetainsUnavailableSourcesAndManyToOneMembershipValues)
{
	io::RigFile rig{};
	auto first = TriggerDescriptor("first", "Shared", io::RigFile::Trigger::MidiInputMode::Selected);
	first.InputChannels = { 0u, 3u };
	first.MidiInputDevices = { "Missing keyboard" };
	auto second = TriggerDescriptor("second", "Shared", io::RigFile::Trigger::MidiInputMode::Any);
	rig.Triggers = { first, second };

	const auto runtime = BuildSnapshot(rig,
		{ StationDescriptor("Shared"), StationDescriptor("Other") },
		1u,
		{ "Present keyboard" });

	ASSERT_TRUE(runtime);
	ASSERT_EQ(2u, runtime->Triggers.size());
	EXPECT_EQ(0u, runtime->Triggers[0].StationIndex.value());
	EXPECT_EQ(0u, runtime->Triggers[1].StationIndex.value());
	ASSERT_EQ(3u, runtime->Graph.Triggers[0].Sources.size());
	EXPECT_TRUE(runtime->Graph.Triggers[0].Sources[0].Available);
	EXPECT_FALSE(runtime->Graph.Triggers[0].Sources[1].Available);
	EXPECT_FALSE(runtime->Graph.Triggers[0].Sources[2].Available);
	ASSERT_EQ(1u, runtime->Graph.Triggers[1].Sources.size());
	EXPECT_EQ("*", runtime->Graph.Triggers[1].Sources[0].MidiDevice);
}

TEST_F(RigSnapshotTest, PreservesExplicitMidiModesForLiveInputEligibility)
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
	AddTestRigTrigger(station, none.value());
	EXPECT_FALSE(station->AcceptsLiveMidiFromDevice("Keys"));
	AddTestRigTrigger(station, selected.value());
	EXPECT_TRUE(station->AcceptsLiveMidiFromDevice("Keys"));
	EXPECT_FALSE(station->AcceptsLiveMidiFromDevice("Other"));
	AddTestRigTrigger(station, any.value());
	EXPECT_TRUE(station->AcceptsLiveMidiFromDevice("Other"));

	auto noRoutes = RuntimeStation("No routes");
	EXPECT_FALSE(noRoutes->AcceptsLiveMidiFromDevice("Keys"));
}

TEST_F(RigSnapshotTest, HudCableRoutesContainOnlyResolvedGraphEdges)
{
	engine::RoutingGraph graph;
	io::RigFileRouting::TriggerResolution resolved;
	resolved.TriggerIndex = 0u;
	resolved.TriggerName = "resolved";
	resolved.StationIndex = 3u;
	resolved.Sources.push_back({ io::RigFileRouting::SourceKind::Adc, 1u, {}, true });
	io::RigFileRouting::TriggerResolution unbound;
	unbound.TriggerIndex = 1u;
	unbound.TriggerName = "unbound";
	unbound.Sources.push_back({ io::RigFileRouting::SourceKind::Midi, 0u, "Keys", false });
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

TEST_F(RigSnapshotTest, ReplacesCompleteStationMembershipAndResetRetainsPublishedRig)
{
	auto station = RuntimeStation("Station");
	const auto descriptor = StationDescriptor("Station");
	io::RigFile initial{};
	initial.Triggers = { TriggerDescriptor("first", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { descriptor }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	auto membership = station->TriggerMembershipSnapshot();
	ASSERT_TRUE(membership);
	ASSERT_EQ(1u, membership->size());
	station->Reset();
	membership = station->TriggerMembershipSnapshot();
	ASSERT_TRUE(membership);
	EXPECT_EQ(1u, membership->size());

	io::RigFile replacement{};
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.SubmitCandidate(replacement,
			[](std::uint64_t) { return true; },
			[](const io::RigFile&) { return true; }));
	const auto pending = coordinator.Pending();
	ASSERT_TRUE(pending);
	coordinator.ApplyPendingAtAudioBoundary();
	membership = station->TriggerMembershipSnapshot();
	ASSERT_TRUE(membership);
	EXPECT_TRUE(membership->empty());
}

TEST_F(RigSnapshotTest, RequiresAudioAcknowledgementBeforeInputAndBothBeforePromotion)
{
	auto station = RuntimeStation("Station");
	io::RigFile initial{};
	initial.Triggers = { TriggerDescriptor("first", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto acceptedRevision = coordinator.Accepted()->Revision;
	io::RigFile candidate{};
	candidate.Triggers = { TriggerDescriptor("replacement", "Station") };
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.SubmitCandidate(candidate,
			[](std::uint64_t) { return true; },
			[](const io::RigFile&) { return true; }));
	const auto pendingRevision = coordinator.Pending()->Revision;

	EXPECT_FALSE(coordinator.AcknowledgeInput(pendingRevision));
	EXPECT_FALSE(coordinator.PromoteAcknowledged());
	EXPECT_EQ(acceptedRevision, coordinator.Accepted()->Revision);
	coordinator.ApplyPendingAtAudioBoundary();
	EXPECT_EQ(pendingRevision, coordinator.AudioAcknowledgement());
	EXPECT_FALSE(coordinator.PromoteAcknowledged());
	ASSERT_TRUE(coordinator.AcknowledgeInput(pendingRevision));
	ASSERT_TRUE(coordinator.PromoteAcknowledged());
	EXPECT_EQ(pendingRevision, coordinator.Accepted()->Revision);
	EXPECT_FALSE(coordinator.Pending());
}

TEST_F(RigSnapshotTest, RejectedCandidatesConsumeRevisionsAndPersistenceFailureRollsBack)
{
	auto station = RuntimeStation("Station");
	io::RigFile initial{};
	initial.Triggers = { TriggerDescriptor("accepted", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto accepted = coordinator.Accepted();
	const auto originalMembership = station->TriggerMembershipSnapshot();
	std::uint64_t rejectedRevision = 0u;
	EXPECT_EQ(engine::RigCoordinator::EditResult::QuiescenceRejected,
		coordinator.SubmitCandidate(initial,
			[&rejectedRevision](std::uint64_t revision) { rejectedRevision = revision; return false; },
			[](const io::RigFile&) { return true; }));
	std::uint64_t persistenceRevision = 0u;
	EXPECT_EQ(engine::RigCoordinator::EditResult::PersistenceFailed,
		coordinator.SubmitCandidate(initial,
			[&persistenceRevision](std::uint64_t revision) { persistenceRevision = revision; return true; },
			[](const io::RigFile&) { return false; }));
	EXPECT_GT(persistenceRevision, rejectedRevision);
	EXPECT_EQ(accepted, coordinator.Accepted());
	EXPECT_FALSE(coordinator.Pending());
	EXPECT_EQ(originalMembership, station->TriggerMembershipSnapshot());

	std::uint64_t successfulRevision = 0u;
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.SubmitCandidate(initial,
			[&successfulRevision](std::uint64_t revision) { successfulRevision = revision; return true; },
			[](const io::RigFile&) { return true; }));
	EXPECT_GT(successfulRevision, persistenceRevision);
	EXPECT_EQ(successfulRevision, coordinator.Pending()->Revision);
}

TEST_F(RigSnapshotTest, ShutdownAndReleaseAreIdempotent)
{
	auto station = RuntimeStation("Station");
	io::RigFile rig{};
	rig.Triggers = { TriggerDescriptor("accepted", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(rig, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	coordinator.Shutdown();
	coordinator.Shutdown();
	EXPECT_FALSE(coordinator.EditsEnabled());
	EXPECT_EQ(engine::RigCoordinator::EditResult::EditsDisabled,
		coordinator.SubmitCandidate(rig,
			[](std::uint64_t) { return true; },
			[](const io::RigFile&) { return true; }));
	coordinator.ReleaseAfterReadersStopped();
	coordinator.ReleaseAfterReadersStopped();
	EXPECT_FALSE(coordinator.Accepted());
	EXPECT_FALSE(coordinator.Pending());
}
