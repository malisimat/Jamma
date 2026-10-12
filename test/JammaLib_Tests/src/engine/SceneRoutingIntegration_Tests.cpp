#include "gtest/gtest.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "engine/Scene.h"
#include "gui/GuiScrollPanel.h"
#include "io/NativeMidiSidecar.h"
#include "io/IoSessionExporter.h"
#include "utils/StringUtils.h"

namespace engine
{
    class SourceLossIntegrationTestAccess
    {
    public:
        static void Confirm(Scene& scene, const midi::MidiConnectionResult& previous,
            const midi::MidiConnectionResult& refreshed, const std::vector<std::string>& names)
        {
            scene._ConfirmMidiConnectionChange(previous, refreshed, names);
        }
        static bool Export(Scene& scene, const std::wstring& directory)
        {
            return io::IoSessionExporter::ExportSessionToDirectory(scene.SnapshotStations(), scene.AcceptedRigSnapshot(),
                scene._quantisation, scene._globalMidiQuantState, scene._transportOffsetLoopFrac,
                scene._userConfig, scene._audioEngine->GetStreamParams(), nullptr, scene._sceneMutex, nullptr, directory);
        }
        static void Manage(Scene& scene, bool prepend, std::shared_ptr<Station> remove = {})
        {
			// Existing topology tests have no render pump; finish the previous
			// presentation interval before their next independent operation.
			FinishMotion(scene);
			scene._RequestStationControl({prepend, std::move(remove)});
		}
		static void FinishMotion(Scene& scene)
		{ std::scoped_lock lock(scene._sceneMutex); for (const auto& station : scene._stations) station->FinishPresentationMove(); }
		static bool ControlsAvailable(Scene& scene)
		{ std::scoped_lock lock(scene._sceneMutex); return scene._StationManagementAvailableLocked(); }
        static void ConfirmRemoval(Scene& scene) { scene._popupManager.Close(); scene._AcceptStationRemoval(true); }
        static bool Busy(Scene& scene) { std::scoped_lock lock(scene._sceneMutex); return scene._stationManagement.Busy(); }
        static void Recover(Scene& scene) { scene._PumpSourceLossRecovery(); }
		static std::shared_ptr<gui::GuiNumericInput> FocusSetting(Scene& scene, bool midiChannel, const std::string& text)
		{
			auto field = midiChannel ? scene._midiChannelOverrideInput : scene._transportOffsetInput;
			// Focus manager and text edit state belong to the window owner. Commit
			// remains deferred until actual Scene focus loss invokes FinalizeEdits.
			EXPECT_TRUE(scene._focusManager.RequestFocus(field));
			field->SetText(text, false);
			return field;
		}
		static unsigned int ForcedChannel(const Scene& scene) { return scene._inputSubsystem->ForcedChannelOverride(); }
		static void StaleProjection(Scene& scene) { scene._viewProj = glm::mat4(0.0f); }
		static bool HasCurrentProjection(const Scene& scene)
		{
			const auto aspect = static_cast<float>(scene._sizeParams.Size.Width) / scene._sizeParams.Size.Height;
			const auto current = scene._camera.Projection(aspect, Scene::_StationCentre(scene._stations)) * scene._camera.ViewMatrix();
			return scene._viewProj == current;
		}
		static std::shared_ptr<gui::GuiHud> PrepareHud(Scene& scene)
		{
			std::scoped_lock lock(scene._sceneMutex);
			const bool alreadyClosed = (scene._audioEngine->_callbackAdmission.load(std::memory_order_acquire)
				& audio::AudioHost::_CallbackAdmissionClosed) != 0u;
			scene._audioEngine->CloseCallbackAdmission();
			scene._audioEngine->PublishPendingRigSnapshot(scene._rigCoordinator.Accepted());
			scene._audioEngine->ApplyPendingRigWhileQuiescent();
			if (!alreadyClosed) scene._audioEngine->ReopenCallbackAdmission();
			EXPECT_EQ(scene._rigCoordinator.Accepted()->Revision, scene._audioEngine->AppliedRigRevision());
			// Supply only the liveness hint. Publication still runs through the real
			// producer gate, stopped-reader barrier and input acknowledgement.
			scene._audioEngine->_audioCallbackHeartbeat.store(1u);
			scene._lastAudioCallbackHeartbeatAt = std::chrono::steady_clock::now();
			scene._hudPanel->SetCableRevealHeld(true);
			scene._hudPanel->SetStationAnchors({ {0u, scene._stations[0]->Name(), glm::dvec2{600, 350}, glm::vec4{1.0f}},
				{1u, scene._stations[1]->Name(), glm::dvec2{800, 350}, glm::vec4{1.0f}} });
			return scene._hudPanel;
		}
		static void AudioBoundary(Scene& scene)
		{
			std::scoped_lock lock(scene._sceneMutex);
			const bool alreadyClosed = (scene._audioEngine->_callbackAdmission.load(std::memory_order_acquire)
				& audio::AudioHost::_CallbackAdmissionClosed) != 0u;
			scene._audioEngine->CloseCallbackAdmission();
			scene._audioEngine->ApplyPendingRigWhileQuiescent();
			if (!alreadyClosed) scene._audioEngine->ReopenCallbackAdmission();
		}
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
	static void LayoutHud(const std::shared_ptr<base::GuiElement>& element)
	{
		if (auto stack = std::dynamic_pointer_cast<gui::GuiStackPanel>(element)) stack->ComputeLayout();
		if (auto scroll = std::dynamic_pointer_cast<gui::GuiScrollPanel>(element))
			if (scroll->Content()) LayoutHud(scroll->Content());
		for (unsigned int i = 0; i < 256; ++i)
		{ auto child = element->TryGetChild(static_cast<unsigned char>(i)); if (!child) break; LayoutHud(child); }
	}
	static void Settle(const std::shared_ptr<engine::Scene>& scene)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while ((!scene->CaptureCommittedRig() || engine::SourceLossIntegrationTestAccess::Busy(*scene)) && std::chrono::steady_clock::now() < deadline)
		{ engine::SourceLossIntegrationTestAccess::AudioBoundary(*scene); scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
	}
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

TEST_F(SceneRoutingIntegrationTest, StoppedStreamMembershipPreservesSelectionAndRouteIdentity)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "B")});
 ASSERT_TRUE(scene);
 auto before = scene->SnapshotStations();
 auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
 before[1]->Select();
 before[0]->SetAllowedMidiChannels({9});
 engine::SourceLossIntegrationTestAccess::Manage(*scene, true);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (engine::SourceLossIntegrationTestAccess::Busy(*scene) && std::chrono::steady_clock::now() < deadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 auto added = scene->SnapshotStations();
 ASSERT_EQ(3u, added.size());
 EXPECT_EQ(before[0], added[1]); EXPECT_EQ(before[1], added[2]);
 EXPECT_TRUE(before[1]->IsSelected()); EXPECT_FALSE(added[0]->IsSelected());
 EXPECT_EQ(trigger, scene->AcceptedRigSnapshot()->Triggers.front().Instance);
 EXPECT_EQ(before[1], scene->AcceptedRigSnapshot()->Triggers.front().BoundReceiver);
 EXPECT_EQ((std::vector<int>{9}), before[0]->AllowedMidiChannels());
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false, added[0]);
 const auto deleteDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (engine::SourceLossIntegrationTestAccess::Busy(*scene) && std::chrono::steady_clock::now() < deleteDeadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 EXPECT_EQ(2u, scene->SnapshotStations().size());
 EXPECT_TRUE(before[1]->IsSelected());
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ActiveStationDeletionCancelsAndUnbindsWithoutSelectingNeighbour)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "A")});
 ASSERT_TRUE(scene);
 auto station = scene->SnapshotStations().front();
 auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
 io::UserConfig cfg{}; cfg.Audio.NumChannelsIn = 1u;
 engine::SourceLossIntegrationTestAccess::StartRecording(*scene, trigger, cfg);
 ASSERT_EQ(1u, station->NumTakes());
 station->Select();
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false, station);
 EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::Busy(*scene)); // Consent has not stopped recording.
 EXPECT_FALSE(station->MembershipRemoving());
 engine::SourceLossIntegrationTestAccess::ConfirmRemoval(*scene);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (engine::SourceLossIntegrationTestAccess::Busy(*scene) && std::chrono::steady_clock::now() < deadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 EXPECT_EQ(1u, scene->SnapshotStations().size());
 EXPECT_FALSE(scene->HasSelection());
 EXPECT_FALSE(scene->AcceptedRigSnapshot()->Triggers.front().BoundReceiver);
 EXPECT_EQ("", scene->AcceptedRigSnapshot()->Rig.Triggers.front().StationTarget);
 EXPECT_TRUE(trigger->GetTakes().empty());
 std::weak_ptr<engine::Station> retired = station;
 station.reset();
 const auto retireDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (!retired.expired() && std::chrono::steady_clock::now() < retireDeadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 EXPECT_TRUE(retired.expired()) << "Removal must retire without another edit";
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SavedJamAndExplicitRigReproduceAddedMembershipAndBindings)
{
 auto scene = FreshScene({Station("A")}, {Trigger("record", "A")});
 ASSERT_TRUE(scene);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, true);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (engine::SourceLossIntegrationTestAccess::Busy(*scene) && std::chrono::steady_clock::now() < deadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 ASSERT_EQ(2u, scene->SnapshotStations().size());
 const auto rig = scene->CaptureCommittedRig(); ASSERT_TRUE(rig);
 std::stringstream rigJson; ASSERT_TRUE(io::RigFile::ToJsonStream(*rig, rigJson));
 auto savedRig = io::RigFile::FromStream(std::move(rigJson)); ASSERT_TRUE(savedRig);
 const auto directory = std::filesystem::temp_directory_path() / ("jamma-station-save-" + std::to_string(utils::Timer::GetTime().time_since_epoch().count()));
 std::filesystem::create_directories(directory);
 ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::Export(*scene, directory.wstring()));
 std::ifstream file(directory / "session.jam"); std::stringstream jamJson; jamJson << file.rdbuf(); file.close();
 auto savedJam = io::JamFile::FromStream(std::move(jamJson)); ASSERT_TRUE(savedJam);
 EXPECT_EQ(2u, savedJam->Stations.size());
 EXPECT_EQ((std::vector<int>{1}), savedJam->Stations.front().AllowedMidiChannels);
 engine::SceneParams params({ "" }, {}, {640u, 480u});
 auto reloaded = engine::Scene::FromFile(params, *savedJam, *savedRig, directory.wstring()); ASSERT_TRUE(reloaded);
 EXPECT_EQ(scene->SnapshotStations()[0]->Name(), reloaded.value()->SnapshotStations()[0]->Name());
 EXPECT_EQ("A", reloaded.value()->AcceptedRigSnapshot()->Rig.Triggers.front().StationTarget); // frozen target lives in Rig
 reloaded.value()->Shutdown(); scene->Shutdown();
 std::filesystem::remove_all(directory);
}

TEST_F(SceneRoutingIntegrationTest, DeletedStationOrphanReconnectsThroughLockedSceneHudDispatch)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "A")});
 ASSERT_TRUE(scene);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false, scene->SnapshotStations()[0]);
 Settle(scene);
 ASSERT_EQ(1u, scene->SnapshotStations().size());
 ASSERT_FALSE(scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false);
 Settle(scene);
 ASSERT_EQ(2u, scene->SnapshotStations().size());
 const auto replacement = scene->SnapshotStations()[1];
 scene->SetSize({1200, 800});
 auto hud = engine::SourceLossIntegrationTestAccess::PrepareHud(*scene);
 LayoutHud(hud);
 std::vector<gui::CableInteraction::Endpoint> endpoints;
 std::vector<gui::CableInteraction::Cable> cables;
 hud->BuildInteractionGeometry(endpoints, cables);
 const auto source = std::find_if(endpoints.begin(), endpoints.end(), [](const auto& e) { return e.Kind == gui::CableInteraction::EndpointKind::TriggerOutput; });
 const auto target = std::find_if(endpoints.begin(), endpoints.end(), [](const auto& e) { return e.Kind == gui::CableInteraction::EndpointKind::Station && e.StationIndex == 1u; });
 ASSERT_NE(endpoints.end(), source); ASSERT_NE(endpoints.end(), target);
 actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 0;
 touch.Position = source->Position; touch.State = actions::TouchAction::TOUCH_DOWN;
 EXPECT_TRUE(scene->OnAction(touch).IsEaten);
 ASSERT_TRUE(hud->HasCableDrag());
 actions::TouchMoveAction move; move.Touch = actions::TouchAction::TOUCH_MOUSE;
 move.Position = target->Position; move.MouseButtonsDown = 1u;
 scene->OnAction(move);
 touch.Position = target->Position; touch.State = actions::TouchAction::TOUCH_UP;
 EXPECT_NO_THROW(scene->OnAction(touch));
 EXPECT_FALSE(hud->HasCableDrag());
 Settle(scene);
 EXPECT_EQ(replacement, scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 EXPECT_EQ(replacement->Name(), scene->AcceptedRigSnapshot()->Rig.Triggers[0].StationTarget);
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 EXPECT_FALSE(hud->HasCableDrag());
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, PublicRoutingSubmissionAndFocusLossRemainSafe)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "A")});
 ASSERT_TRUE(scene);
 auto hud = engine::SourceLossIntegrationTestAccess::PrepareHud(*scene);
 auto candidate = scene->AcceptedRigSnapshot()->Rig;
 candidate.Triggers[0].StationTarget = "B";
 EXPECT_EQ(engine::RigCoordinator::EditResult::Pending, scene->RequestRigEdit(candidate));
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 Settle(scene);
 EXPECT_EQ(scene->SnapshotStations()[1], scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FocusLossCommitsMidiChannelSettingOutsideSceneLock)
{
 auto scene = FreshScene({Station("A")}, {});
 ASSERT_TRUE(scene);
 auto field = engine::SourceLossIntegrationTestAccess::FocusSetting(*scene, true, "7");
 ASSERT_TRUE(field->HasFocus());
 EXPECT_EQ(0u, engine::SourceLossIntegrationTestAccess::ForcedChannel(*scene));
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 EXPECT_EQ(7u, engine::SourceLossIntegrationTestAccess::ForcedChannel(*scene));
 EXPECT_DOUBLE_EQ(7.0, field->Value());
 EXPECT_FALSE(field->HasFocus());
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FocusLossCommitsPhaseOffsetSettingOutsideSceneLock)
{
 auto scene = FreshScene({Station("A")}, {});
 ASSERT_TRUE(scene);
 auto field = engine::SourceLossIntegrationTestAccess::FocusSetting(*scene, false, "0.125");
 ASSERT_TRUE(field->HasFocus());
 EXPECT_DOUBLE_EQ(0.0, scene->SnapshotStations()[0]->TransportOffsetLoopFrac());
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 EXPECT_DOUBLE_EQ(0.125, scene->SnapshotStations()[0]->TransportOffsetLoopFrac());
 EXPECT_DOUBLE_EQ(0.125, field->Value());
 EXPECT_FALSE(field->HasFocus());
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, StationMotionPreservesFinalTopologyAndAppendLeavesSurvivorsStill)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "B")});
 ASSERT_TRUE(scene);
 const auto original = scene->SnapshotStations();
 engine::SourceLossIntegrationTestAccess::Manage(*scene, true);
 Settle(scene);
 auto prepended = scene->SnapshotStations(); ASSERT_EQ(3u, prepended.size());
 EXPECT_FLOAT_EQ(-50.0f, prepended[0]->ModelPosition().X);
 EXPECT_FLOAT_EQ(-650.0f, prepended[0]->PresentationModelPosition().X);
 EXPECT_FLOAT_EQ(-50.0f, original[0]->PresentationModelPosition().X);
 EXPECT_FLOAT_EQ(550.0f, original[0]->ModelPosition().X);
 EXPECT_EQ(original[1], scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ControlsAvailable(*scene));
 ASSERT_TRUE(scene->CaptureCommittedRig()); // Saved identities are final during motion.
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false);
 Settle(scene);
 auto appended = scene->SnapshotStations(); ASSERT_EQ(4u, appended.size());
 for (size_t i = 0; i < 3; ++i) EXPECT_FALSE(appended[i]->IsPresentationMoving());
 EXPECT_FLOAT_EQ(appended[3]->ModelPosition().X + 600.0f, appended[3]->PresentationModelPosition().X);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false, appended[0]);
 Settle(scene);
 auto deleted = scene->SnapshotStations(); ASSERT_EQ(3u, deleted.size());
 EXPECT_EQ(original[0], deleted[0]);
 EXPECT_FLOAT_EQ(-50.0f, deleted[0]->ModelPosition().X);
 EXPECT_FLOAT_EQ(550.0f, deleted[0]->PresentationModelPosition().X);
 EXPECT_EQ(original[1], scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 engine::SourceLossIntegrationTestAccess::FinishMotion(*scene);
 EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ControlsAvailable(*scene));
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, UiFrameRefreshesCameraProjectionWithoutGraphicsResources)
{
 auto scene = FreshScene({Station("A")}, {});
 ASSERT_TRUE(scene);
 scene->SetSize({1200, 800});
 // Represent the previous frame's projection before camera/frame preparation.
 engine::SourceLossIntegrationTestAccess::StaleProjection(*scene);
 EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::HasCurrentProjection(*scene));
 scene->AdvanceUiAnimations();
 EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::HasCurrentProjection(*scene));
 scene->Shutdown();
}
