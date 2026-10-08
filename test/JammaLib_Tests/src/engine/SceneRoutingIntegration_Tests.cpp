#include "gtest/gtest.h"
#include <algorithm>
#include "engine/Scene.h"

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
