#include "gtest/gtest.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "engine/Scene.h"
#include "gui/SceneSelector.h"
#include "graphics/Window.h"
#include "io/NativeMidiSidecar.h"
#include "utils/StringUtils.h"

namespace engine
{
    class PressPickWindow : public graphics::Window
    {
    public:
        PressPickWindow(Scene& scene, resources::ResourceLib& resources) : Window(scene, resources) {}
        std::vector<unsigned char> PressPath;
        utils::Position2d PickPosition{};
        unsigned int PickCount = 0;
    protected:
        std::optional<unsigned int> _PickPointerPress(utils::Position2d position) override
        {
            PickPosition = position;
            ++PickCount;
            if (PressPath.empty()) return 0u; // Cleared picker pixel.
            unsigned int objectId = 0;
            for (unsigned int segment = 0; segment < 3u; ++segment)
                objectId |= (segment < PressPath.size() ? PressPath[segment] : 0xffu) << (16u - 8u * segment);
            return objectId;
        }
    };

    class SourceLossIntegrationTestAccess
    {
    public:
        static void WhileSceneLocked(Scene& scene, const std::function<void()>& callback)
        {
            std::scoped_lock lock(scene._sceneMutex);
            callback();
        }
        static bool ControlHeld(const Scene& scene) { return scene._ctrlHeld; }
        static void SelectTakeDepth(Scene& scene) { scene._UpdateSelectDepth(Scene::VIEW_LOOPTAKE); }
        static bool ItemMutePressed(const Scene& scene) { return scene._selector->IsMutePressed(); }
        static bool ItemMutePainting(const Scene& scene) { return scene._selector->IsPaintingMute(); }
        static bool BackgroundPreviewActive(const Scene& scene) { return scene._BackgroundMutePreviewActive(); }
        static graphics::Camera::Pose CameraPose(const Scene& scene) { return scene._camera.CurrentPose(); }
        static bool BackgroundMuteOwned(const Scene& scene) { return scene._backgroundMuteGesture.has_value(); }
        static bool CameraBackgroundActive(const Scene& scene) { return scene._camera.IsBackgroundDragging(); }
        static bool CameraPointerHeld(const Scene& scene) { return scene._camera.IsBackgroundPointerHeld(); }
        static bool ApplyMuteBoundary(Scene& scene)
        {
            scene._audioEngine->CloseCallbackAdmission();
            return scene._audioEngine->ApplyPendingRigWhileQuiescent();
        }
        static void Confirm(Scene& scene, const midi::MidiConnectionResult& previous,
            const midi::MidiConnectionResult& refreshed, const std::vector<std::string>& names)
        {
            scene._ConfirmMidiConnectionChange(previous, refreshed, names);
        }
        static void Recover(Scene& scene) { scene._PumpSourceLossRecovery(); }
        static bool RecoveryGateIsClosed(const Scene& scene) { return scene._sourceLossRecoveryGateClosed; }
        static void CloseRecoveryGate(Scene& scene)
        {
            scene._audioEngine->CloseCallbackAdmission();
            scene._sourceLossRecoveryGateClosed = true;
        }
        static void StartRecording(Scene& scene, const std::shared_ptr<Trigger>& trigger,
            const io::UserConfig& cfg)
        {
            // The real job runner also consumes structural commands. Keep the
            // test's simulated audio/job handoff in the same critical section
            // as Scene's production structural pump.
            std::scoped_lock lock(scene._sceneMutex);
            trigger->AddInputChannel(0u);
            base::Action action;
            trigger->QueueExternalControlAction(true, true, action);
            trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
            trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
            trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
        }
    };
}

class SceneRoutingIntegrationTest : public testing::Test
{
protected:
	static io::JamFile::Station Station(std::string name)
	{
		io::JamFile::Station station{};
		station.Name = std::move(name);
		return station;
	}

	static io::RigFile::Trigger Trigger(std::string name, std::string target)
	{
		io::RigFile::Trigger trigger{};
		trigger.Name = std::move(name);
		trigger.StationTarget = std::move(target);
		trigger.MidiInputs = io::RigFile::Trigger::MidiInputMode::None;
		return trigger;
	}

	static std::shared_ptr<engine::Scene> FreshScene(std::vector<io::JamFile::Station> stations,
		std::vector<io::RigFile::Trigger> triggers,
		std::function<bool(const io::RigFile&)> saveRig = [](const io::RigFile&) { return true; },
		std::vector<io::JamFile::TriggerHistory> histories = {})
	{
		io::JamFile jam{};
		jam.Stations = std::move(stations);
		jam.TriggerHistories = std::move(histories);
		io::RigFile rig{};
		rig.Triggers = std::move(triggers);
		engine::SceneParams params({ "" }, {}, { 640u, 480u });
		auto scene = engine::Scene::FromFile(params, std::move(jam), std::move(rig), L"", std::move(saveRig));
		return scene.has_value() ? scene.value() : nullptr;
	}

	static std::vector<std::pair<std::string, std::size_t>> Memberships(const std::shared_ptr<engine::Scene>& scene)
	{
		std::vector<std::pair<std::string, std::size_t>> result;
		const auto stations = scene->SnapshotStations();
		const auto rig = scene->AcceptedRigSnapshot();
		for (std::size_t stationIndex = 0u; stationIndex < stations.size(); ++stationIndex)
		{
			const auto count = rig ? static_cast<std::size_t>(std::count_if(rig->Triggers.begin(),
				rig->Triggers.end(), [stationIndex](const engine::RigSnapshotTrigger& trigger)
				{
					return trigger.StationIndex == stationIndex;
				})) : 0u;
			result.emplace_back(stations[stationIndex]->Name(), count);
		}
		return result;
	}
};

TEST_F(SceneRoutingIntegrationTest, HaloSelectionIncludesMidiOnlyModelsAndClearsOnDeselect)
{
	auto scene = FreshScene({ Station("Keys") }, {});
	ASSERT_TRUE(scene);
	const auto stations = scene->SnapshotStations();
	ASSERT_EQ(1u, stations.size());
	auto station = stations.front();
	auto take = station->AddTake();
	take->Record({}, station->Name(), { 0u }, { "Keys" });
	scene->CommitChanges();
	ASSERT_EQ(1u, take->GetMidiLoops().size());
	auto model = take->GetMidiLoops().front()->Model();
	ASSERT_TRUE(model);
	station->DeSelect();
	take->DeSelect();
	EXPECT_FALSE(scene->HasSelection());

	// A MIDI model can be selected independently of its aggregate take state.
	model->Select();
	EXPECT_TRUE(scene->HasSelection());
	model->DeSelect();
	EXPECT_FALSE(scene->HasSelection());
	take->Select();
	EXPECT_TRUE(scene->HasSelection());
	take->DeSelect();
	EXPECT_FALSE(scene->HasSelection());
	station->Select();
	EXPECT_TRUE(scene->HasSelection());
	station->DeSelect();
	EXPECT_FALSE(scene->HasSelection());
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ConfiguredMissingMidiSourceCreatesEditableTakeThroughActivateControl)
{
    auto configured = Trigger("record", "Keys");
    configured.MidiInputs = io::RigFile::Trigger::MidiInputMode::Selected;
    configured.MidiInputDevices = { "missing" };
    auto scene = FreshScene({ Station("Keys") }, { configured });
    ASSERT_TRUE(scene);
    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    auto station = scene->SnapshotStations().front();
    station->SetAllowedMidiChannels({ 1 });
    // Confirm the real empty inventory, without manually adding a runtime source.
    engine::SourceLossIntegrationTestAccess::Confirm(*scene, {}, {}, {});
    io::UserConfig cfg;
    cfg.Trigger = { 0u, 0u };
    base::Action action;
    auto press = [&](bool down) {
        trigger->QueueExternalControlAction(true, down, action);
        trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
        trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
        trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
        scene->CommitChanges();
    };
    press(true); press(false);
    ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
    ASSERT_EQ(1u, station->GetLoopTakeSnapshot().size());
    auto take = station->GetLoopTakeSnapshot().front();
    EXPECT_TRUE(take->GetLoops().empty());
    ASSERT_EQ(1u, take->GetMidiLoops().size());
    EXPECT_EQ(std::vector<std::string>{ "missing" }, take->MidiLoopDevices());
    trigger->OnTick(utils::Timer::GetTime(), 256u, cfg, std::nullopt);
    press(true); press(false);
    EXPECT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState());
    EXPECT_GT(take->GetMidiLoops().front()->CompletedLengthForEditor(), 0u);
    EXPECT_EQ(0u, take->GetMidiLoops().front()->EventCount());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SuccessfulMidiConnectionLossCancelsCaptureThroughStoppedRecovery)
{
    auto configured = Trigger("record", "Keys");
    configured.InputChannels = { 0u };
    configured.MidiInputs = io::RigFile::Trigger::MidiInputMode::Selected;
    configured.MidiInputDevices = { "lost" };
    auto scene = FreshScene({ Station("Keys") }, { configured });
    ASSERT_TRUE(scene);
    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    ASSERT_TRUE(trigger);
    // Boundary route setup substitutes actual successfully opened capture names.
    trigger->AddMidiInputDevice("lost");
    trigger->AddInputChannel(0u);
    base::Action action;
    io::UserConfig cfg;
    trigger->QueueExternalControlAction(true, true, action);
    trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
    trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
    trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
    ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
    midi::MidiConnectionResult previous;
    previous.Connected.push_back({ 0u, "lost", "lost 0" });
    midi::MidiConnectionResult failed;
    failed.Inventory.Error = "transient inventory failure";
    engine::SourceLossIntegrationTestAccess::Confirm(*scene, previous, failed, {});
    EXPECT_FALSE(trigger->HasPendingSourceLoss());
    midi::MidiConnectionResult reopened;
    reopened.Connected.push_back({ 7u, "lost", "lost 7" });
    engine::SourceLossIntegrationTestAccess::Confirm(*scene, previous, reopened, { "lost" });
    EXPECT_FALSE(trigger->HasPendingSourceLoss());
    engine::SourceLossIntegrationTestAccess::Confirm(*scene, reopened, {}, {});
    ASSERT_TRUE(trigger->HasPendingSourceLoss());
    for (unsigned int step = 0u; step < 4u; ++step)
        engine::SourceLossIntegrationTestAccess::Recover(*scene);
    EXPECT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState());
    EXPECT_TRUE(trigger->GetTakes().empty());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ResetRetiresSixtyFourRestoredHistoryPinsAndAllowsRecording)
{
    struct SidecarCleanup
    {
        std::filesystem::path Path;
        ~SidecarCleanup() { std::error_code error; std::filesystem::remove(Path, error); }
    } sidecar{ std::filesystem::temp_directory_path() / ("jamma-source-loss-" + utils::GetGuid() + ".jammidi") };
    io::NativeMidiSidecar::Stream contents;
    contents.LogicalLength = 100u;
    contents.Events.push_back({ 0u, 0x90u, 60u, 100u });
    {
        std::ofstream output(sidecar.Path, std::ios::binary);
        ASSERT_TRUE(output);
        ASSERT_TRUE(io::NativeMidiSidecar::ToStream(contents, output));
    }
    auto descriptor = Station("restored-reset");
    auto configured = Trigger("restored-trigger", descriptor.Name);
    configured.Id = "restored-trigger-id";
    io::JamFile::TriggerHistory history;
    history.TriggerId = configured.Id;
    for (unsigned int index = 0u; index < 64u; ++index)
    {
        io::JamFile::LoopTake take{};
        take.Name = "restored-" + std::to_string(index);
        take.MidiPlayLength = 100u;
        take.MidiStreams.push_back({ sidecar.Path.string(), 0u, "healthy", 100u, 0u });
        descriptor.LoopTakes.push_back(take);
        history.Takes.push_back({ static_cast<unsigned int>(engine::TriggerTake::SOURCE_ADC), "", take.Name });
    }
    auto scene = FreshScene({ descriptor }, { configured }, {}, { history });
    ASSERT_TRUE(scene);
    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    auto station = scene->SnapshotStations().front();
    ASSERT_EQ(64u, trigger->GetTakes().size());
    ASSERT_EQ(64u, station->GetLoopTakeSnapshot().size());
    const auto externallyRetainedTarget = station->GetLoopTakeSnapshot().front();
    station->Reset();
    scene->CommitChanges();
    io::UserConfig cfg;
    for (unsigned int step = 0u; step < 130u; ++step)
        engine::SourceLossIntegrationTestAccess::Recover(*scene);
    EXPECT_TRUE(trigger->GetTakes().empty());
    EXPECT_FALSE(trigger->HasPendingSourceLoss());
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::RecoveryGateIsClosed(*scene));
    EXPECT_TRUE(externallyRetainedTarget->IsTriggerCaptureInactive());
    engine::SourceLossIntegrationTestAccess::StartRecording(*scene, trigger, cfg);
    EXPECT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ShutdownPreventsRecoveryFromReopeningAdmission)
{
    auto scene = FreshScene({ Station("shutdown") }, {});
    ASSERT_TRUE(scene);
    engine::SourceLossIntegrationTestAccess::CloseRecoveryGate(*scene);
    scene->Shutdown();
    engine::SourceLossIntegrationTestAccess::Recover(*scene);
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::RecoveryGateIsClosed(*scene));
}

TEST_F(SceneRoutingIntegrationTest, TakeClicksSelectAndToggleMuteWithoutChangingSibling)
{
	auto scene = FreshScene({ Station("Keys") }, {});
	ASSERT_TRUE(scene);
	const auto station = scene->SnapshotStations().front();
	const auto take = station->AddTake();
	const auto sibling = station->AddTake();
	scene->CommitChanges();
	actions::GuiAction view;
	view.ElementType = actions::GuiAction::ACTIONELEMENT_RADIO;
	view.Index = 100u;
	view.Data = actions::GuiAction::GuiInt{ engine::Scene::VIEW_LOOPTAKE };
	scene->OnAction(view);
	std::vector<unsigned char> pickPath;
	for (const auto index : take->GlobalId())
		pickPath.push_back(static_cast<unsigned char>(index + 1u));
	scene->SetHover3d(pickPath, base::Action::MODIFIER_NONE);

	actions::TouchAction click;
	click.Touch = actions::TouchAction::TOUCH_MOUSE;
	click.Position = { -100, -100 }; // Avoid the 2D controls; use the picked take.
	click.Modifiers = base::Action::MODIFIER_NONE;
	click.Index = 0;
	click.State = actions::TouchAction::TOUCH_DOWN;
	scene->OnAction(click);
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_TRUE(take->IsSelected());
	EXPECT_FALSE(sibling->IsSelected());

	click.Index = 1;
	for (const bool muted : { true, false })
	{
		click.State = actions::TouchAction::TOUCH_DOWN;
		scene->OnAction(click);
		click.State = actions::TouchAction::TOUCH_UP;
		scene->OnAction(click);
		EXPECT_EQ(muted, take->IsMuted());
		EXPECT_FALSE(sibling->IsMuted());
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ExpandedRackSuppressesOtherStationsAndRestoresOnLeavingView)
{
	auto scene = FreshScene({ Station("Keys"), Station("Bass") }, {});
	ASSERT_TRUE(scene);
	scene->SetSize({ 640, 480 });
	const auto stations = scene->SnapshotStations();
	const auto rack = stations[0]->GetGuiRack();
	const auto otherRack = stations[1]->GetGuiRack();
	rack->SetPosition({ 100, 100 });
	otherRack->SetPosition({ 300, 100 });
	rack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
	scene->AdvanceUiAnimations();
	ASSERT_EQ(gui::GuiRackParams::RACK_CHANNELS, rack->GetRackState());
	EXPECT_TRUE(rack->GetMasterSlider()->Parent()->IsVisible());
	EXPECT_FALSE(otherRack->GetMasterSlider()->Parent()->IsVisible());

	// Closing just the router still leaves an expanded channel rack.
	rack->SetRackState(gui::GuiRackParams::RACK_ROUTER, true);
	rack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
	scene->AdvanceUiAnimations();
	EXPECT_FALSE(otherRack->GetMasterSlider()->Parent()->IsVisible());
	rack->SetRackState(gui::GuiRackParams::RACK_MASTER, true);
	scene->AdvanceUiAnimations();
	EXPECT_TRUE(otherRack->GetMasterSlider()->Parent()->IsVisible());

	rack->SetRackState(gui::GuiRackParams::RACK_ROUTER, true);
	rack->SetPosition({ -10000, 100 });
	scene->AdvanceUiAnimations();
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	EXPECT_TRUE(otherRack->GetMasterSlider()->Parent()->IsVisible());
	rack->SetPosition({ 100, 100 });
	scene->AdvanceUiAnimations();
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SelectingAnotherTakeCollapsesRackButHoverAndRackInputPreserveIt)
{
	auto scene = FreshScene({ Station("Keys") }, {});
	ASSERT_TRUE(scene);
	scene->SetSize({ 1280, 900 });
	const auto station = scene->SnapshotStations().front();
	const auto take = station->AddTake();
	const auto sibling = station->AddTake();
	scene->CommitChanges();
	actions::GuiAction view;
	view.ElementType = actions::GuiAction::ACTIONELEMENT_RADIO;
	view.Index = 100u;
	view.Data = actions::GuiAction::GuiInt{ engine::Scene::VIEW_LOOPTAKE };
	scene->OnAction(view);
	const auto rack = take->GetGuiRack();
	rack->SetPosition({ 500, 400 });
	rack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
	scene->AdvanceUiAnimations();
	ASSERT_EQ(gui::GuiRackParams::RACK_CHANNELS, rack->GetRackState());

	std::vector<unsigned char> pickPath;
	for (const auto index : sibling->GlobalId())
		pickPath.push_back(static_cast<unsigned char>(index + 1u));
	scene->SetHover3d(pickPath, base::Action::MODIFIER_NONE);
	EXPECT_EQ(gui::GuiRackParams::RACK_CHANNELS, rack->GetRackState());

	// Exercise the scene's actual press/release capture path for the master.
	const auto slider = rack->GetMasterSlider();
	const auto sliderPosition = slider->GlobalPosition();
	actions::TouchAction click;
	click.Touch = actions::TouchAction::TOUCH_MOUSE;
	click.Position = { sliderPosition.X + 10, sliderPosition.Y + 10 };
	click.Index = 0;
	click.State = actions::TouchAction::TOUCH_DOWN;
	const auto rackPress = scene->OnAction(click);
	ASSERT_TRUE(rackPress.IsEaten);
	EXPECT_EQ(slider, rackPress.ActiveElement.lock());
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_EQ(gui::GuiRackParams::RACK_CHANNELS, rack->GetRackState());

	actions::TouchMoveAction move;
	move.Touch = actions::TouchAction::TOUCH_MOUSE;
	move.Position = { -100, -100 };
	scene->OnAction(move);
	scene->SetHover3d(pickPath, base::Action::MODIFIER_NONE);
	click.Position = { -100, -100 };
	click.State = actions::TouchAction::TOUCH_DOWN;
	scene->OnAction(click);
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_TRUE(sibling->IsSelected());
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	EXPECT_TRUE(sibling->GetGuiRack()->GetMasterSlider()->Parent()->IsVisible());

	// A background click clears selection and restores every master control.
	rack->SetRackState(gui::GuiRackParams::RACK_ROUTER, true);
	scene->AdvanceUiAnimations();
	scene->SetHover3d({}, base::Action::MODIFIER_NONE);
	click.State = actions::TouchAction::TOUCH_DOWN;
	scene->OnAction(click);
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_FALSE(scene->HasSelection());
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	EXPECT_TRUE(sibling->GetGuiRack()->GetMasterSlider()->Parent()->IsVisible());
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FreshScenesResolveReorderedAndAdditionalStationsByName)
{
	auto reordered = FreshScene({ Station("Bass"), Station("Drums") },
		{ Trigger("drums", "Drums"), Trigger("bass", "Bass") });
	ASSERT_TRUE(reordered);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Bass", 1u }, { "Drums", 1u } }),
		Memberships(reordered));
	reordered->Shutdown();

	auto additional = FreshScene({ Station("Drums"), Station("Unused"), Station("Bass") },
		{ Trigger("bass", "Bass") });
	ASSERT_TRUE(additional);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{
		{ "Drums", 0u }, { "Unused", 0u }, { "Bass", 1u } }), Memberships(additional));
	additional->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, RestoresDistinctHistoriesToTriggersWithSharedStation)
{
	auto bass = Station("Bass");
	auto drums = Station("Drums");
	auto drumTrigger = Trigger("drums", "Drums"); drumTrigger.Id = "drums-id";
	auto firstBassTrigger = Trigger("bass-first", "Bass"); firstBassTrigger.Id = "bass-first-id";
	auto secondBassTrigger = Trigger("bass-second", "Bass"); secondBassTrigger.Id = "bass-second-id";
	// Station order differs from trigger order, and two triggers share Bass.
	auto scene = FreshScene({ bass, drums }, { drumTrigger, firstBassTrigger, secondBassTrigger }, {}, {
		{ "drums-id", { { static_cast<unsigned int>(engine::TriggerTake::SOURCE_ADC), "drums-source", "drums-target" } } },
		{ "bass-first-id", { { static_cast<unsigned int>(engine::TriggerTake::SOURCE_ADC), "bass-source", "bass-target" } } },
		{ "bass-second-id", { { static_cast<unsigned int>(engine::TriggerTake::SOURCE_ADC), "other-source", "other-target" } } } });
	ASSERT_TRUE(scene);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Bass", 2u }, { "Drums", 1u } }),
		Memberships(scene));
	const auto rig = scene->AcceptedRigSnapshot();
	ASSERT_TRUE(rig);
	ASSERT_EQ(3u, rig->Triggers.size());
	ASSERT_TRUE(rig->Triggers[0].Instance);
	ASSERT_TRUE(rig->Triggers[1].Instance);
	ASSERT_TRUE(rig->Triggers[2].Instance);
	ASSERT_EQ(1u, rig->Triggers[0].Instance->GetTakes().size());
	EXPECT_EQ("drums-source", rig->Triggers[0].Instance->GetTakes()[0].SourceTakeId);
	ASSERT_EQ(1u, rig->Triggers[1].Instance->GetTakes().size());
	EXPECT_EQ("bass-source", rig->Triggers[1].Instance->GetTakes()[0].SourceTakeId);
	EXPECT_EQ("bass-target", rig->Triggers[1].Instance->GetTakes()[0].TargetTakeId);
	ASSERT_EQ(1u, rig->Triggers[2].Instance->GetTakes().size());
	EXPECT_EQ("other-source", rig->Triggers[2].Instance->GetTakes()[0].SourceTakeId);
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FreshScenesLeaveRenamedMissingAmbiguousAndFewerTargetsUnbound)
{
	auto renamed = FreshScene({ Station("Renamed") }, { Trigger("old", "Original") });
	ASSERT_TRUE(renamed);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Renamed", 0u } }), Memberships(renamed));
	renamed->Shutdown();

	auto missing = FreshScene({ Station("Present") }, { Trigger("missing", "Absent") });
	ASSERT_TRUE(missing);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Present", 0u } }), Memberships(missing));
	missing->Shutdown();

	auto ambiguous = FreshScene({ Station("Dup"), Station("Dup") }, { Trigger("ambiguous", "Dup") });
	ASSERT_TRUE(ambiguous);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Dup", 0u }, { "Dup", 0u } }),
		Memberships(ambiguous));
	ambiguous->Shutdown();

	auto fewer = FreshScene({ Station("Only") },
		{ Trigger("only", "Only"), Trigger("removed", "Removed") });
	ASSERT_TRUE(fewer);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Only", 1u } }), Memberships(fewer));
	fewer->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, InitialMigrationKeepsLegacyRuntimeRouteWithoutSaving)
{
	auto legacy = Trigger("legacy", "");
	legacy.StationTarget.reset();
	unsigned int saves = 0u;
	auto scene = FreshScene({ Station("Station") }, { legacy }, [&saves](const io::RigFile&)
	{
		++saves;
		return false;
	});
	ASSERT_TRUE(scene);
	EXPECT_EQ(0u, saves);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Station", 1u } }), Memberships(scene));
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, CloseAudioAndShutdownAreRepeatable)
{
	auto scene = FreshScene({ Station("Station") }, { Trigger("trigger", "Station") });
	ASSERT_TRUE(scene);
	EXPECT_NO_THROW(scene->CloseAudio());
	EXPECT_NO_THROW(scene->CloseAudio());
	EXPECT_NO_THROW(scene->Shutdown());
	EXPECT_NO_THROW(scene->Shutdown());
	EXPECT_EQ(engine::RigCoordinator::EditResult::EditsDisabled,
		scene->RequestRigEdit(io::RigFile{}));
}

TEST_F(SceneRoutingIntegrationTest, RoutingEditIsRejectedWhenAudioCallbackIsInactive)
{
	auto scene = FreshScene({ Station("Station") }, { Trigger("trigger", "Station") });
	ASSERT_TRUE(scene);
	EXPECT_EQ(engine::RigCoordinator::EditResult::AudioCallbackInactive,
		scene->RequestRigEdit(io::RigFile{}));
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Station", 1u } }),
		Memberships(scene));
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, PluginFocusLossDefersSceneEntryUntilStateCallUnlocks)
{
    auto scene = FreshScene({ Station("Keys") }, {});
    ASSERT_TRUE(scene);
    resources::ResourceLib resources;
    graphics::Window window(*scene, resources);
    const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(host, nullptr);
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    actions::KeyAction control;
    control.KeyChar = 17u;
    control.KeyActionType = actions::KeyAction::KEY_DOWN;
    window.OnAction(control);
    ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ControlHeld(*scene));
    actions::TouchAction press;
    press.Touch = actions::TouchAction::TOUCH_MOUSE;
    press.State = actions::TouchAction::TOUCH_DOWN;
    press.Index = 0;
    press.Position = { 320, 240 };
    window.OnAction(press);
    // Mirror Kontakt's synchronous native notification inside the Scene-locked
    // group state pump. Any Scene/capture/modifier dispatch here re-locks Scene.
    engine::SourceLossIntegrationTestAccess::WhileSceneLocked(*scene, [&] {
        graphics::Window::WindowProcedure(host, WM_KILLFOCUS, 0, 0);
        graphics::Window::WindowProcedure(host, WM_CAPTURECHANGED, 0, 0);
        graphics::Window::WindowProcedure(host, WM_KILLFOCUS, 0, 0);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ControlHeld(*scene));
        EXPECT_EQ(window.Modifiers(), base::Action::MODIFIER_CTRL);
    });
    EXPECT_TRUE(window.ProcessDeferredInputLoss());
    EXPECT_FALSE(window.CancelMouseCapture());
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ControlHeld(*scene));
    EXPECT_EQ(window.Modifiers(), base::Action::MODIFIER_NONE);
    EXPECT_FALSE(window.ProcessDeferredInputLoss());
    DestroyWindow(host);
}

TEST_F(SceneRoutingIntegrationTest, GroupFunctionKeyAndFocusLossDoNotReachConfiguredTrigger)
{
    auto triggerConfig = Trigger("Record", "Keys");
    triggerConfig.TriggerPairs.push_back({ 112u, 112u, 113u, 113u });
    auto scene = FreshScene({ Station("Keys") }, { triggerConfig });
    ASSERT_TRUE(scene);
    const auto rig = scene->AcceptedRigSnapshot();
    ASSERT_TRUE(rig && rig->Triggers.size() == 1u);
    actions::KeyAction key;
    key.KeyChar = 112u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    EXPECT_TRUE(scene->OnAction(key).IsEaten);
    EXPECT_TRUE(scene->OnAction(key).IsEaten); // Suppressed repeat.
    scene->OnInputFocusLost();
    key.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_TRUE(scene->OnAction(key).IsEaten);
    const auto trigger = rig->Triggers[0].Instance;
    ASSERT_TRUE(trigger);
    engine::SourceLossIntegrationTestAccess::WhileSceneLocked(*scene, [&] {
        trigger->OnTick(utils::Timer::GetTime(), 0u, io::UserConfig{},
            scene->GetAudioStreamParams(), rig->Revision);
    });
    EXPECT_EQ(trigger->GetState(), engine::TRIGSTATE_DEFAULT);
    EXPECT_TRUE(trigger->GetTakes().empty());
    EXPECT_TRUE(scene->SnapshotStations()[0]->GetLoopTakeSnapshot().empty());
}

TEST_F(SceneRoutingIntegrationTest, InWindowItemMiddlePaintUsesSelectionThresholdWithoutGlobalCapture)
{
    for (const bool initiallyMuted : { false, true })
    {
        auto scene = FreshScene({ Station("Local") }, {});
        ASSERT_TRUE(scene);
        scene->SetSize({ 800, 600 });
        auto station = scene->SnapshotStations().front();
        auto audioTake = station->AddTake();
        audioTake->AddLoop(0u, station->Id());
        auto midiTake = station->AddTake();
        midiTake->Record({}, station->Name(), { 0u }, { "Keys" });
        auto untouched = station->AddTake();
        untouched->AddLoop(0u, station->Id());
        scene->CommitChanges();
        engine::SourceLossIntegrationTestAccess::SelectTakeDepth(*scene);
        auto audioLoop = audioTake->GetLoops().front();
        const auto midiModel = midiTake->GetMidiLoops().front()->Model();
        ASSERT_TRUE(midiModel);
        audioTake->SetGroupMutedAudio(initiallyMuted);
        audioLoop->SetGroupMutedAudio(initiallyMuted);
        midiTake->SetGroupMutedAudio(initiallyMuted);
        untouched->SetGroupMutedAudio(initiallyMuted);
        const auto pickPath = [](const auto& item) {
            std::vector<unsigned char> path;
            for (const auto index : item->GlobalId()) path.push_back(static_cast<unsigned char>(index + 1u));
            return path;
        };
        const auto audioPath = pickPath(audioLoop);
        const auto midiPath = pickPath(midiModel);
        scene->SetHover3d(audioPath, base::Action::MODIFIER_NONE);
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = { 400, 300 }; // Valid client point; the picker reports this actual loop.
        click.Index = 1;
        click.MouseButtonsDown = 2u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        ASSERT_TRUE(scene->OnAction(click).IsEaten);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePressed(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ItemMutePainting(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        EXPECT_EQ(initiallyMuted, audioTake->IsMuted());
        EXPECT_EQ(initiallyMuted, midiTake->IsMuted());
        EXPECT_TRUE(audioTake->IsPicking3d());
        EXPECT_FALSE(midiTake->IsPicking3d());
        actions::TouchMoveAction move;
        move.Touch = actions::TouchAction::TOUCH_MOUSE;
        move.Position = click.Position;
        move.MouseButtonsDown = 2u;
        scene->OnAction(move);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePressed(*scene));
        move.Position.X += 5;
        scene->OnAction(move);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePainting(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ItemMutePressed(*scene));
        EXPECT_EQ(!initiallyMuted, audioTake->IsMuted());
        EXPECT_EQ(initiallyMuted, midiTake->IsMuted());
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        scene->SetHover3d(midiPath, base::Action::MODIFIER_NONE);
        EXPECT_EQ(!initiallyMuted, midiTake->IsMuted());
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        scene->SetHover3d({}, base::Action::MODIFIER_NONE); // Passing through background keeps item paint ownership.
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        scene->SetHover3d(audioPath, base::Action::MODIFIER_NONE);
        EXPECT_EQ(!initiallyMuted, audioTake->IsMuted()); // Revisit does not toggle direction.
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        click.Position = move.Position;
        click.State = actions::TouchAction::TOUCH_UP;
        click.MouseButtonsDown = 0u;
        scene->OnAction(click);
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ItemMutePainting(*scene));
        EXPECT_EQ(!initiallyMuted, audioTake->IsMuted());
        EXPECT_EQ(!initiallyMuted, midiTake->IsMuted());
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, RawStationPickBelowSelectionDepthDoesNotBecomeGlobalMiddleGesture)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto station = scene->SnapshotStations().front();
    auto take = station->AddTake();
    scene->CommitChanges();
        engine::SourceLossIntegrationTestAccess::SelectTakeDepth(*scene);
    take->SetGroupMutedAudio(true);
    std::vector<unsigned char> stationPath;
    for (const auto index : station->GlobalId()) stationPath.push_back(static_cast<unsigned char>(index + 1u));
    scene->SetHover3d(stationPath, base::Action::MODIFIER_NONE);
    actions::TouchAction click;
    click.Touch = actions::TouchAction::TOUCH_MOUSE;
    click.Position = { 400, 300 };
    click.Index = 1;
    click.MouseButtonsDown = 2u;
    click.State = actions::TouchAction::TOUCH_DOWN;
    scene->OnAction(click);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
    click.State = actions::TouchAction::TOUCH_UP;
    click.MouseButtonsDown = 0u;
    scene->OnAction(click);
    ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
    EXPECT_TRUE(take->IsMuted());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, WindowFreshPressPickWinsOverStaleBackgroundAndItemHover)
{
    for (const bool initiallyMuted : { false, true })
    {
        auto scene = FreshScene({ Station("Local") }, {});
        ASSERT_TRUE(scene);
        resources::ResourceLib resources;
        engine::PressPickWindow window(*scene, resources);
        window.Resize({ 800, 600 });
        auto station = scene->SnapshotStations().front();
        auto take = station->AddTake();
        take->AddLoop(0u, station->Id());
        auto untouched = station->AddTake();
        scene->CommitChanges();
        engine::SourceLossIntegrationTestAccess::SelectTakeDepth(*scene);
        take->SetGroupMutedAudio(initiallyMuted);
        take->GetLoops().front()->SetGroupMutedAudio(initiallyMuted);
        untouched->SetGroupMutedAudio(initiallyMuted);
        for (const auto index : take->GetLoops().front()->GlobalId())
            window.PressPath.push_back(static_cast<unsigned char>(index + 1u));
        scene->SetHover3d({}, base::Action::MODIFIER_NONE); // Previous rendered hover was background.
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = { 400, 300 };
        click.Index = 1;
        click.State = actions::TouchAction::TOUCH_DOWN;
        EXPECT_TRUE(window.OnAction(click).IsEaten);
        EXPECT_EQ(1u, window.PickCount);
        EXPECT_EQ(click.Position, window.PickPosition);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePressed(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        actions::TouchMoveAction move;
        move.Touch = actions::TouchAction::TOUCH_MOUSE;
        move.Position = { 405, 300 };
        move.MouseButtonsDown = 2u;
        window.OnAction(move);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePainting(*scene));
        EXPECT_EQ(!initiallyMuted, take->IsMuted());
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        click.State = actions::TouchAction::TOUCH_UP;
        click.Position = move.Position;
        window.OnAction(click);
        // The inverse stale-hover case must acquire a genuine background press.
        window.PressPath.clear();
        click.Position = { 420, 300 };
        click.State = actions::TouchAction::TOUCH_DOWN;
        window.OnAction(click);
        EXPECT_EQ(2u, window.PickCount);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        click.State = actions::TouchAction::TOUCH_UP;
        window.OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_FALSE(take->IsMuted());
        EXPECT_FALSE(untouched->IsMuted());
        scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, InWindowBackgroundMuteGesturesPreviewAndApplyOnlyStationaryRelease)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto station = scene->SnapshotStations().front();
    auto take = station->AddTake();
    take->AddLoop(0u, station->Id());
    take->CommitChanges();
    scene->CommitChanges();
    auto loop = take->GetLoops().front();
    for (const bool mute : { false, true })
    {
        for (int movement = 0; movement < 5; ++movement)
        {
            take->SetGroupMutedAudio(!mute);
            loop->SetGroupMutedAudio(!mute);
            scene->SetHover3d({}, base::Action::MODIFIER_NONE);
            const auto pose = engine::SourceLossIntegrationTestAccess::CameraPose(*scene);
            actions::TouchAction click;
            click.Touch = actions::TouchAction::TOUCH_MOUSE;
            click.Position = { 400, 300 }; // Empty space inside the pass-through HUD.
            click.Index = 1;
            click.Modifiers = mute ? base::Action::MODIFIER_ALT : base::Action::MODIFIER_NONE;
            click.MouseButtonsDown = 2u;
            click.State = actions::TouchAction::TOUCH_DOWN;
            ASSERT_TRUE(scene->OnAction(click).IsEaten);
            ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
            EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
            EXPECT_EQ(!mute, take->IsMuted());
            actions::TouchMoveAction move;
            move.Touch = actions::TouchAction::TOUCH_MOUSE;
            move.Position = click.Position;
            move.MouseButtonsDown = click.MouseButtonsDown;
            scene->OnAction(move); // A duplicate position is not movement.
            EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
            if (movement == 1 || movement == 2)
            {
                move.Position.X += 1;
                scene->OnAction(move);
                EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
                EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
                if (movement == 2) // Returning to the origin does not restore the click.
                {
                    move.Position = click.Position;
                    scene->OnAction(move);
                    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
                }
                click.Position = move.Position;
            }
            else if (movement == 4)
            {
                move.IsRelative = true;
                move.RelativeDelta = { 1, 0 };
                scene->OnAction(move);
                EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
            }
            else if (movement == 3) click.Position.X += 1; // Missing MOVE event.
            click.State = actions::TouchAction::TOUCH_UP;
            click.Modifiers = base::Action::MODIFIER_NONE; // Command was latched at press.
            click.MouseButtonsDown = 0u;
            EXPECT_TRUE(scene->OnAction(click).IsEaten);
            EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
            EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
            EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CameraBackgroundActive(*scene));
            const auto after = engine::SourceLossIntegrationTestAccess::CameraPose(*scene);
            EXPECT_EQ(pose.Eye, after.Eye);
            EXPECT_EQ(pose.Forward, after.Forward);
            EXPECT_EQ(pose.Up, after.Up);
            ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
            EXPECT_EQ(movement == 0 ? mute : !mute, take->IsMuted());
            EXPECT_EQ(movement == 0 ? mute : !mute, loop->IsMuted());
        }
    }
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, NativeBackgroundMiddleAndAltMiddleUseClickCommands)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    resources::ResourceLib resources;
    graphics::Window window(*scene, resources);
    window.Resize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(host, nullptr);
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    for (const bool mute : { false, true })
    {
        take->SetGroupMutedAudio(!mute);
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        if (mute) graphics::Window::WindowProcedure(host, WM_SYSKEYDOWN, VK_MENU, 1u << 29u);
        graphics::Window::WindowProcedure(host, WM_MBUTTONDOWN,
            MK_MBUTTON, MAKELPARAM(400, 300));
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
        if (mute) graphics::Window::WindowProcedure(host, WM_SYSKEYUP, VK_MENU, 1u << 29u);
        graphics::Window::WindowProcedure(host, WM_MBUTTONUP, 0, MAKELPARAM(400, 300));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_EQ(mute, take->IsMuted());
    }
    SetWindowLongPtrW(host, GWLP_USERDATA, 0);
    DestroyWindow(host);
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, AltLeftBackgroundRetainsCameraGestureAndDoesNotMuteAll)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    resources::ResourceLib resources;
    graphics::Window window(*scene, resources);
    window.Resize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    scene->SetHover3d({}, base::Action::MODIFIER_NONE);
    actions::KeyAction alt;
    alt.KeyChar = VK_MENU;
    alt.KeyActionType = actions::KeyAction::KEY_DOWN;
    window.OnAction(alt);
    actions::TouchAction click;
    click.Touch = actions::TouchAction::TOUCH_MOUSE;
    click.Position = { 400, 300 };
    click.Index = 0;
    click.Modifiers = base::Action::MODIFIER_ALT;
    click.State = actions::TouchAction::TOUCH_DOWN;
    window.OnAction(click);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
    click.State = actions::TouchAction::TOUCH_UP;
    window.OnAction(click);
    ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
    EXPECT_FALSE(take->IsMuted());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, BackgroundUnmuteCancelsOnCaptureLossFocusLossEscapeResetAndChord)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    for (int cancellation = 0; cancellation < 5; ++cancellation)
    {
        take->SetGroupMutedAudio(true);
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = { 400, 300 };
        click.Index = 1;
        click.MouseButtonsDown = 2u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        scene->OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        if (cancellation == 0)
        {
            actions::TouchMoveAction lost;
            lost.Touch = actions::TouchAction::TOUCH_MOUSE;
            lost.MouseButtonsDown = 0u;
            scene->OnAction(lost);
        }
        else if (cancellation == 1) scene->OnInputFocusLost();
        else if (cancellation == 2)
        {
            actions::KeyAction escape;
            escape.KeyChar = 27u;
            escape.KeyActionType = actions::KeyAction::KEY_DOWN;
            scene->OnAction(escape);
        }
        else if (cancellation == 3) scene->Reset();
        else
        {
            auto chord = click;
            chord.Index = 0;
            chord.MouseButtonsDown = 3u;
            scene->OnAction(chord);
        }
        click.State = actions::TouchAction::TOUCH_UP;
        click.MouseButtonsDown = 0u;
        scene->OnAction(click);
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_TRUE(take->IsMuted()) << cancellation;
        scene->AdvanceUiAnimations();
    }
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, MiddlePressOnItemAndHudDoesNotAcquireGlobalUnmute)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    auto sibling = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    sibling->SetGroupMutedAudio(true);
    std::vector<unsigned char> itemPath;
    for (auto index : take->GlobalId()) itemPath.push_back(static_cast<unsigned char>(index + 1u));
    scene->SetHover3d(itemPath, base::Action::MODIFIER_NONE);
    actions::TouchAction click;
    click.Touch = actions::TouchAction::TOUCH_MOUSE;
    click.Position = { 400, 300 };
    click.Index = 1;
    click.MouseButtonsDown = 2u;
    click.State = actions::TouchAction::TOUCH_DOWN;
    scene->OnAction(click);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
    click.State = actions::TouchAction::TOUCH_UP;
    click.MouseButtonsDown = 0u;
    scene->OnAction(click);
    ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
    EXPECT_TRUE(sibling->IsMuted());
    scene->SetHover3d({}, base::Action::MODIFIER_NONE);
    click.Position = { 10, 10 };
    click.State = actions::TouchAction::TOUCH_DOWN;
    click.MouseButtonsDown = 2u;
    scene->OnAction(click);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
    scene->OnInputFocusLost();
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, BackgroundMuteStopsUnownedCameraCoastButPreservesHeldCameraChord)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    for (const bool mute : { false, true })
    {
        take->SetGroupMutedAudio(!mute);
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        actions::TouchAction right;
        right.Touch = actions::TouchAction::TOUCH_MOUSE;
        right.Position = { 400, 300 };
        right.Index = 2;
        right.MouseButtonsDown = 4u;
        right.State = actions::TouchAction::TOUCH_DOWN;
        scene->OnAction(right);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
        actions::TouchMoveAction move;
        move.Touch = actions::TouchAction::TOUCH_MOUSE;
        move.Position = { 450, 320 };
        move.MouseButtonsDown = 4u;
        scene->OnAction(move);
        right.Position = move.Position;
        right.MouseButtonsDown = 0u;
        right.Value = 4;
        right.State = actions::TouchAction::TOUCH_UP;
        scene->OnAction(right);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::CameraBackgroundActive(*scene));
        ASSERT_FALSE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
        auto click = right;
        click.Index = 1;
        click.Modifiers = mute ? base::Action::MODIFIER_ALT : base::Action::MODIFIER_NONE;
        click.MouseButtonsDown = 2u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        scene->OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CameraBackgroundActive(*scene));
        click.MouseButtonsDown = 0u;
        click.State = actions::TouchAction::TOUCH_UP;
        scene->OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_EQ(mute, take->IsMuted());
    }
    actions::TouchAction right;
    right.Touch = actions::TouchAction::TOUCH_MOUSE;
    right.Position = { 400, 300 };
    right.Index = 2;
    right.MouseButtonsDown = 4u;
    right.State = actions::TouchAction::TOUCH_DOWN;
    scene->OnAction(right);
    auto chord = right;
    chord.Index = 1;
    chord.MouseButtonsDown = 6u;
    scene->OnAction(chord);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundMuteOwned(*scene));
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
    chord.State = actions::TouchAction::TOUCH_UP;
    chord.MouseButtonsDown = 4u;
    chord.Value = 2;
    scene->OnAction(chord);
    right.State = actions::TouchAction::TOUCH_UP;
    right.MouseButtonsDown = 0u;
    right.Value = 4;
    scene->OnAction(right);
    scene->Shutdown();
}
