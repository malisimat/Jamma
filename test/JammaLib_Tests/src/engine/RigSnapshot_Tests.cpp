#include "engine/RigCoordinator.h"
#include "audio/AudioHost.h"
#include "engine/Station.h"
#include "gui/GuiHud.h"

namespace audio
{
	class RigAudioBoundaryTestAccess
	{
	public:
		static void ApplyPending(AudioHost& host) noexcept
		{
			host.ApplyPendingRigSnapshotAtAudioBoundary();
		}

		static void PublishQuiescence(AudioHost& host) noexcept
		{
			host.PublishRigTriggerQuiescenceAtAudioBoundary();
		}
	};
}

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
		trigger.Id = name + "-id";
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

TEST_F(RigSnapshotTest, ResolvesLegacyTargetAndKeepsNormalizedRuntimeIdentity)
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
	EXPECT_EQ("Station", runtime->Rig.Triggers[0].StationTarget.value());
	EXPECT_FALSE(runtime->Rig.Triggers[0].Id.empty());
	EXPECT_EQ(io::RigFileRouting::Warning::LegacyStationTargetMigrated, runtime->Graph.Triggers[0].Reason);
}

TEST_F(RigSnapshotTest, RetainsUnavailableSourcesWithoutCreatingWildcardMidiRoutes)
{
	io::RigFile rig{};
	auto first = TriggerDescriptor("first", "Shared", io::RigFile::Trigger::MidiInputMode::Selected);
	first.InputChannels = { 0u, 3u };
	first.MidiInputDevices = { "Missing keyboard" };
	auto second = TriggerDescriptor("second", "Shared");
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
	EXPECT_TRUE(runtime->Graph.Triggers[1].Sources.empty());
	ASSERT_EQ(1u, runtime->InputDispatch.LiveMidi.size());
	ASSERT_EQ("Present keyboard", runtime->InputDispatch.LiveMidi[0].DeviceName);
	EXPECT_TRUE(runtime->InputDispatch.LiveMidi[0].Recipients.empty());
}

TEST_F(RigSnapshotTest, LiveMidiEligibilityRequiresAnExplicitDeviceSelection)
{
	auto noneConfig = TriggerDescriptor("none", "Station", io::RigFile::Trigger::MidiInputMode::None);
	auto selectedConfig = TriggerDescriptor("selected", "Station", io::RigFile::Trigger::MidiInputMode::Selected);
	selectedConfig.MidiInputDevices = { "Keys" };

	io::RigFile rig{};
	rig.Triggers = { noneConfig, selectedConfig };
	const auto snapshot = BuildSnapshot(rig, { StationDescriptor("Station") }, 0u,
		{ "Keys", "Other" });
	ASSERT_TRUE(snapshot);
	ASSERT_EQ(2u, snapshot->InputDispatch.LiveMidi.size());
	ASSERT_EQ(1u, snapshot->InputDispatch.LiveMidi[0].Recipients.size());
	EXPECT_EQ("Station", snapshot->InputDispatch.LiveMidi[0].Recipients[0]->Name());
	EXPECT_TRUE(snapshot->InputDispatch.LiveMidi[1].Recipients.empty());
}

TEST_F(RigSnapshotTest, CaptureAndStationEditsReuseTriggerAndStageNewMidiDispatch)
{
	auto firstStation = RuntimeStation("First");
	auto secondStation = RuntimeStation("Second");
	io::RigFile initial{};
	auto trigger = TriggerDescriptor("record", "First", io::RigFile::Trigger::MidiInputMode::Selected);
	trigger.InputChannels = { 0u };
	trigger.MidiInputDevices = { "Keys A" };
	initial.Triggers = { trigger };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial,
		{ StationDescriptor("First"), StationDescriptor("Second") },
		{ firstStation, secondStation }, 2u, { "Keys A", "Keys B" },
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto accepted = coordinator.Accepted();
	ASSERT_TRUE(accepted);

	auto candidate = initial;
	candidate.Triggers[0].InputChannels.push_back(1u);
	candidate.Triggers[0].MidiInputDevices = { "Keys B" };
	candidate.Triggers[0].StationTarget = "Second";
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(candidate));
	const auto staged = coordinator.Quiescing();
	ASSERT_TRUE(staged);
	EXPECT_EQ(accepted->Triggers[0].Instance, staged->Triggers[0].Instance);
	EXPECT_TRUE(staged->RetiredTriggerChecks.empty());
	ASSERT_EQ(1u, staged->RetainedTriggerRouteChanges.size());
	EXPECT_EQ(0u, staged->RetainedTriggerRouteChanges[0].CandidateIndex);
	EXPECT_EQ((std::vector<unsigned int>{ 0u, 1u }), staged->Triggers[0].InputChannels);
	EXPECT_EQ(secondStation, staged->Triggers[0].Receiver);
	ASSERT_EQ(2u, staged->InputDispatch.LiveMidi.size());
	EXPECT_TRUE(staged->InputDispatch.LiveMidi[0].Recipients.empty());
	ASSERT_EQ(1u, staged->InputDispatch.LiveMidi[1].Recipients.size());
	EXPECT_EQ(secondStation, staged->InputDispatch.LiveMidi[1].Recipients[0]);
}

TEST_F(RigSnapshotTest, ReorderPreservesStableTriggerInstancesAndDisplayOrder)
{
	io::RigFile initial{};
	initial.Triggers = {
		TriggerDescriptor("first", "Station"),
		TriggerDescriptor("second", "Station")
	};
	auto station = RuntimeStation("Station");
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto accepted = coordinator.Accepted();

	auto candidate = accepted->Rig;
	std::swap(candidate.Triggers[0], candidate.Triggers[1]);
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(candidate));
	const auto staged = coordinator.Quiescing();
	ASSERT_TRUE(staged);
	EXPECT_EQ("second-id", staged->Triggers[0].Id);
	EXPECT_EQ("first-id", staged->Triggers[1].Id);
	EXPECT_EQ(accepted->Triggers[1].Instance, staged->Triggers[0].Instance);
	EXPECT_EQ(accepted->Triggers[0].Instance, staged->Triggers[1].Instance);
	EXPECT_TRUE(staged->RetiredTriggerChecks.empty());
}

TEST_F(RigSnapshotTest, DeletionAndActivationReplacementAreClassifiedByStableId)
{
	io::RigFile initial{};
	initial.Triggers = {
		TriggerDescriptor("first", "Station"),
		TriggerDescriptor("second", "Station")
	};
	auto station = RuntimeStation("Station");
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	const auto accepted = coordinator.Accepted();

	auto deletion = accepted->Rig;
	deletion.Triggers.erase(deletion.Triggers.begin());
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(deletion));
	auto staged = coordinator.Quiescing();
	ASSERT_TRUE(staged);
	ASSERT_EQ(1u, staged->Triggers.size());
	EXPECT_EQ(accepted->Triggers[1].Instance, staged->Triggers[0].Instance);
	ASSERT_EQ(1u, staged->RetiredTriggerChecks.size());
	EXPECT_EQ(accepted->Triggers[0].Instance, staged->RetiredTriggerChecks[0].AcceptedInstance);
	ASSERT_EQ(engine::RigCoordinator::EditResult::QuiescenceRejected,
		coordinator.CompleteQuiescence(staged->Revision, false, [](const io::RigFile&) { return true; }));

	auto replacement = accepted->Rig;
	replacement.Triggers[0].Name = "changed activation";
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(replacement));
	staged = coordinator.Quiescing();
	ASSERT_TRUE(staged);
	EXPECT_NE(accepted->Triggers[0].Instance, staged->Triggers[0].Instance);
	EXPECT_EQ(accepted->Triggers[1].Instance, staged->Triggers[1].Instance);
	ASSERT_EQ(1u, staged->RetiredTriggerChecks.size());
	EXPECT_EQ(accepted->Triggers[0].Instance, staged->RetiredTriggerChecks[0].AcceptedInstance);
}

TEST_F(RigSnapshotTest, DuplicateTriggerIdsFailInitialAndCandidateValidation)
{
	io::RigFile duplicate{};
	duplicate.Triggers = {
		TriggerDescriptor("first", "Station"),
		TriggerDescriptor("second", "Station")
	};
	duplicate.Triggers[1].Id = duplicate.Triggers[0].Id;
	auto station = RuntimeStation("Station");
	engine::RigCoordinator invalid;
	EXPECT_FALSE(invalid.BuildInitial(duplicate, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	io::RigFile valid{};
	valid.Triggers = { TriggerDescriptor("first", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(valid, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	duplicate = coordinator.Accepted()->Rig;
	duplicate.Triggers.push_back(duplicate.Triggers[0]);
	EXPECT_EQ(engine::RigCoordinator::EditResult::ValidationFailed, coordinator.SubmitCandidate(duplicate));
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

TEST(GuiHudLayout, RevealScrollOffsetClampsAndRevealsItems)
{
	EXPECT_EQ(0, gui::GuiHud::RevealScrollOffset(-30, 200, 500, 10, 110));
	EXPECT_EQ(80, gui::GuiHud::RevealScrollOffset(0, 200, 500, 180, 280));
	EXPECT_EQ(120, gui::GuiHud::RevealScrollOffset(160, 200, 500, 120, 220));
	EXPECT_EQ(300, gui::GuiHud::RevealScrollOffset(450, 200, 500, 450, 550));
	EXPECT_EQ(0, gui::GuiHud::RevealScrollOffset(30, 500, 200, 0, 100));
}

TEST_F(RigSnapshotTest, SnapshotOwnsCompleteTriggerListIndependentOfStationReset)
{
	auto station = RuntimeStation("Station");
	const auto descriptor = StationDescriptor("Station");
	io::RigFile initial{};
	initial.Triggers = { TriggerDescriptor("first", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { descriptor }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	const auto accepted = coordinator.Accepted();
	ASSERT_TRUE(accepted);
	ASSERT_EQ(1u, accepted->Triggers.size());
	const auto retainedTrigger = accepted->Triggers[0].Instance;
	station->Reset();
	EXPECT_EQ(retainedTrigger, accepted->Triggers[0].Instance);

	io::RigFile replacement{};
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.SubmitCandidate(replacement));
	const auto quiescing = coordinator.Quiescing();
	ASSERT_TRUE(quiescing);
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteQuiescence(quiescing->Revision, true,
			[](const io::RigFile&) { return true; }));
	const auto pending = coordinator.Pending();
	ASSERT_TRUE(pending);
	coordinator.ApplyPendingAtAudioBoundary();
	EXPECT_TRUE(pending->Triggers.empty());
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
		coordinator.SubmitCandidate(candidate));
	const auto pendingRevision = coordinator.Quiescing()->Revision;
	EXPECT_FALSE(coordinator.Pending());
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteQuiescence(pendingRevision, true,
			[](const io::RigFile&) { return true; }));

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
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(initial));
	const auto rejectedRevision = coordinator.Quiescing()->Revision;
	EXPECT_EQ(engine::RigCoordinator::EditResult::QuiescenceRejected,
		coordinator.CompleteQuiescence(rejectedRevision, false,
			[](const io::RigFile&) { return true; }));
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(initial));
	const auto persistenceRevision = coordinator.Quiescing()->Revision;
	EXPECT_EQ(engine::RigCoordinator::EditResult::PersistenceFailed,
		coordinator.CompleteQuiescence(persistenceRevision, true,
			[](const io::RigFile&) { return false; }));
	EXPECT_GT(persistenceRevision, rejectedRevision);
	EXPECT_EQ(accepted, coordinator.Accepted());
	EXPECT_FALSE(coordinator.Pending());

	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(initial));
	const auto successfulRevision = coordinator.Quiescing()->Revision;
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteQuiescence(successfulRevision, true,
			[](const io::RigFile&) { return true; }));
	EXPECT_GT(successfulRevision, persistenceRevision);
	EXPECT_EQ(successfulRevision, coordinator.Pending()->Revision);
}

TEST_F(RigSnapshotTest, RapidSecondEditIsRejectedUntilFirstEditPromotes)
{
	auto station = RuntimeStation("Station");
	io::RigFile initial{};
	initial.Triggers = { TriggerDescriptor("accepted", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	auto first = initial;
	first.Triggers[0].Name = "first";
	auto second = initial;
	second.Triggers[0].Name = "second";
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(first));
	const auto firstRevision = coordinator.Quiescing()->Revision;
	EXPECT_EQ(engine::RigCoordinator::EditResult::EditsDisabled, coordinator.SubmitCandidate(second));
	EXPECT_EQ(firstRevision, coordinator.Quiescing()->Revision);
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteQuiescence(firstRevision, true, [](const io::RigFile&) { return true; }));
	coordinator.ApplyPendingAtAudioBoundary();
	ASSERT_TRUE(coordinator.AcknowledgeInput(firstRevision));
	ASSERT_TRUE(coordinator.PromoteAcknowledged());
	EXPECT_EQ("first", coordinator.Accepted()->Rig.Triggers[0].Name);
	EXPECT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(second));
}

TEST_F(RigSnapshotTest, ShutdownDuringQuiescenceOrPendingCannotPublishCandidate)
{
	auto makeCoordinator = [this]()
	{
		auto coordinator = std::make_unique<engine::RigCoordinator>();
		io::RigFile initial{};
		initial.Triggers = { TriggerDescriptor("accepted", "Station") };
		const auto station = RuntimeStation("Station");
		if (!coordinator->BuildInitial(initial, { StationDescriptor("Station") }, { station }, 0u, {},
			engine::TriggerParams(), [](const io::RigFile&) { return true; })) return coordinator;
		coordinator->SubmitCandidate(initial);
		return coordinator;
	};

	auto quiescing = makeCoordinator();
	const auto quiescingRevision = quiescing->Quiescing()->Revision;
	quiescing->Shutdown();
	EXPECT_EQ(engine::RigCoordinator::EditResult::QuiescenceRejected,
		quiescing->CompleteQuiescence(quiescingRevision, true, [](const io::RigFile&) { return true; }));
	EXPECT_FALSE(quiescing->Pending());

	auto pending = makeCoordinator();
	const auto pendingRevision = pending->Quiescing()->Revision;
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
		pending->CompleteQuiescence(pendingRevision, true, [](const io::RigFile&) { return true; }));
	pending->Shutdown();
	pending->ReleaseAfterReadersStopped();
	EXPECT_FALSE(pending->Pending());
	EXPECT_FALSE(pending->Accepted());
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
		coordinator.SubmitCandidate(rig));
	coordinator.ReleaseAfterReadersStopped();
	coordinator.ReleaseAfterReadersStopped();
	EXPECT_FALSE(coordinator.Accepted());
	EXPECT_FALSE(coordinator.Pending());
}

TEST_F(RigSnapshotTest, PersistenceWaitsForFreshQuiescenceAndRejectionRestoresEdits)
{
	auto station = RuntimeStation("Station");
	io::RigFile rig{};
	rig.Triggers = { TriggerDescriptor("accepted", "Station") };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(rig, { StationDescriptor("Station") }, { station }, 0u, {},
		engine::TriggerParams(), [](const io::RigFile&) { return true; }));

	unsigned int saves = 0u;
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(rig));
	const auto firstRevision = coordinator.Quiescing()->Revision;
	EXPECT_EQ(0u, saves);
	EXPECT_EQ(engine::RigCoordinator::EditResult::QuiescenceRejected,
		coordinator.CompleteQuiescence(firstRevision, false,
			[&saves](const io::RigFile&) { ++saves; return true; }));
	EXPECT_EQ(0u, saves);
	EXPECT_TRUE(coordinator.EditsEnabled());

	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(rig));
	const auto secondRevision = coordinator.Quiescing()->Revision;
	EXPECT_GT(secondRevision, firstRevision);
	EXPECT_EQ(engine::RigCoordinator::EditResult::Pending,
		coordinator.CompleteQuiescence(secondRevision, true,
			[&saves](const io::RigFile&) { ++saves; return true; }));
	EXPECT_EQ(1u, saves);
}

TEST_F(RigSnapshotTest, AudioBoundaryPublishesFreshTriggerQuiescence)
{
	io::RigFile rig{};
	rig.Triggers = { TriggerDescriptor("accepted", "Station") };
	const auto snapshot = BuildSnapshot(rig, { StationDescriptor("Station") }, 0u, {});
	ASSERT_TRUE(snapshot);
	ASSERT_EQ(1u, snapshot->Triggers.size());

	audio::AudioHost host(io::UserConfig{});
	host.PublishPendingRigSnapshot(snapshot);
	audio::RigAudioBoundaryTestAccess::ApplyPending(host);
	ASSERT_EQ(snapshot->Revision, host.AppliedRigRevision());

	auto sameTriggerCandidate = std::make_shared<engine::RigSnapshot>();
	sameTriggerCandidate->Revision = 41u;
	host.RequestRigTriggerQuiescence(41u, snapshot, sameTriggerCandidate);
	audio::RigAudioBoundaryTestAccess::PublishQuiescence(host);
	EXPECT_EQ(41u, host.QuiescedRigRevision());
	EXPECT_EQ(0u, host.RejectedRigRevision());

	base::Action action;
	ASSERT_TRUE(snapshot->Triggers[0].Instance->QueueExternalControlAction(true, true, action).IsEaten);
	auto replacement = std::make_shared<engine::RigSnapshot>();
	replacement->Revision = 42u;
	replacement->RetiredTriggerChecks = { { snapshot->Triggers[0].Instance } };
	host.RequestRigTriggerQuiescence(42u, snapshot, replacement);
	audio::RigAudioBoundaryTestAccess::PublishQuiescence(host);
	EXPECT_EQ(0u, host.QuiescedRigRevision());
	EXPECT_EQ(42u, host.RejectedRigRevision());
}

TEST_F(RigSnapshotTest, RetainedTriggerRecordsAcrossRealRoutePublicationAndDitchesLifo)
{
	auto stationA = RuntimeStation("A");
	auto stationB = RuntimeStation("B");
	const std::vector<io::JamFile::Station> stationFiles{
		StationDescriptor("A"), StationDescriptor("B") };
	auto descriptor = TriggerDescriptor("record", "A");
	descriptor.InputChannels = { 0u };
	io::RigFile initial{};
	initial.Triggers = { descriptor };
	engine::RigCoordinator coordinator;
	ASSERT_TRUE(coordinator.BuildInitial(initial, stationFiles, { stationA, stationB },
		2u, {}, engine::TriggerParams(), [](const io::RigFile&) { return true; }));
	audio::AudioHost host(io::UserConfig{});
	host.PublishPendingRigSnapshot(coordinator.Accepted());
	audio::RigAudioBoundaryTestAccess::ApplyPending(host);

	auto trigger = coordinator.Accepted()->Triggers[0].Instance;
	ASSERT_TRUE(trigger);
	base::Action action;
	io::UserConfig cfg{};
	cfg.Audio.NumChannelsIn = 2u;
	auto edge = [&](bool activate, bool down, std::uint64_t revision)
	{
		ASSERT_TRUE(trigger->QueueExternalControlAction(activate, down, action, revision).IsEaten);
		trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
		trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
		trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, revision);
	};
	auto record = [&](std::uint64_t revision)
	{
		edge(true, true, revision);
		edge(true, false, revision);
		trigger->OnTick(utils::Timer::GetTime(), 64u, cfg, std::nullopt, revision);
		edge(true, true, revision);
		edge(true, false, revision);
		stationA->CommitChanges();
		stationB->CommitChanges();
	};
	auto publishEdit = [&](io::RigFile candidate)
	{
		ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, coordinator.SubmitCandidate(candidate));
		const auto quiescing = coordinator.Quiescing();
		ASSERT_TRUE(quiescing);
		host.RequestRigTriggerQuiescence(quiescing->Revision, coordinator.Accepted(), quiescing);
		audio::RigAudioBoundaryTestAccess::PublishQuiescence(host);
		ASSERT_EQ(quiescing->Revision, host.QuiescedRigRevision());
		ASSERT_EQ(engine::RigCoordinator::EditResult::Pending,
			coordinator.CompleteQuiescence(quiescing->Revision, true,
				[](const io::RigFile&) { return true; }));
		host.ClearRigTriggerQuiescence();
		host.PublishPendingRigSnapshot(coordinator.Pending());
		audio::RigAudioBoundaryTestAccess::ApplyPending(host);
		ASSERT_TRUE(coordinator.ObserveAudioAcknowledgement(host.AppliedRigRevision()));
		ASSERT_TRUE(coordinator.AcknowledgeInput(host.AppliedRigRevision()));
		ASSERT_TRUE(coordinator.PromoteAcknowledged());
		EXPECT_EQ(trigger, coordinator.Accepted()->Triggers[0].Instance);
	};

	record(coordinator.Accepted()->Revision);
	ASSERT_EQ(1u, stationA->NumTakes());
	ASSERT_EQ(1u, stationA->GetLoopTakes()[0]->GetLoops().size());
	auto candidate = coordinator.Accepted()->Rig;
	candidate.Triggers[0].InputChannels = { 0u, 1u };
	publishEdit(candidate);
	record(coordinator.Accepted()->Revision);
	ASSERT_EQ(2u, stationA->NumTakes());
	ASSERT_EQ(2u, stationA->GetLoopTakes()[1]->GetLoops().size());
	candidate = coordinator.Accepted()->Rig;
	candidate.Triggers[0].InputChannels = { 0u };
	candidate.Triggers[0].StationTarget = "B";
	publishEdit(candidate);
	record(coordinator.Accepted()->Revision);
	ASSERT_EQ(1u, stationB->NumTakes());

	const auto history = trigger->GetTakes();
	ASSERT_EQ(3u, history.size());
	EXPECT_NE(history[0].TargetTakeId, history[1].TargetTakeId);
	EXPECT_NE(history[1].TargetTakeId, history[2].TargetTakeId);
	auto ditch = [&]()
	{
		const auto revision = coordinator.Accepted()->Revision;
		edge(false, true, revision);
		edge(false, false, revision);
		stationA->CommitChanges();
		stationB->CommitChanges();
	};
	ditch();
	EXPECT_EQ(0u, stationB->NumTakes());
	EXPECT_EQ(2u, stationA->NumTakes());
	ditch();
	EXPECT_EQ(1u, stationA->NumTakes());
	ditch();
	EXPECT_EQ(0u, stationA->NumTakes());
	EXPECT_TRUE(trigger->GetTakes().empty());
	ditch();
	EXPECT_EQ(0u, stationA->NumTakes());
	EXPECT_EQ(0u, stationB->NumTakes());
}
