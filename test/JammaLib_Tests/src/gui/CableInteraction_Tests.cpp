#include "gtest/gtest.h"
#include "gui/CableInteraction.h"
#include <limits>
#include "gui/GuiHud.h"
#include "gui/GuiScrollPanel.h"
#include "gui/GuiPopupManager.h"
#include "engine/RigSnapshot.h"
#include "engine/Trigger.h"

using gui::CableInteraction;

TEST(CableGeometry, BoundaryResolutionKeepsVisiblePointsAndClampsEveryEdge)
{
	const utils::Rect2d bounds{ 10, 20, 100, 200 };
	const auto visible = CableInteraction::ResolveBoundary({ 40, 50 }, bounds);
	ASSERT_TRUE(visible);
	EXPECT_EQ((utils::Position2d{ 40, 50 }), visible->Position);
	EXPECT_FALSE(visible->Clipped);
	for (const auto& point : { glm::dvec2{ -1e100, 50 }, { 1e100, 50 }, { 40, -1e100 }, { 40, 1e100 },
		{ -1e100, -1e100 }, { -1e100, 1e100 }, { 1e100, -1e100 }, { 1e100, 1e100 } })
	{
		const auto endpoint = CableInteraction::ResolveBoundary(point, bounds);
		ASSERT_TRUE(endpoint);
		EXPECT_TRUE(endpoint->Clipped);
		EXPECT_TRUE(bounds.Contains(endpoint->Position));
	}
	EXPECT_EQ(10, CableInteraction::ResolveBoundary({ -1e100, 50 }, bounds)->Position.X);
	EXPECT_EQ(99, CableInteraction::ResolveBoundary({ 1e100, 50 }, bounds)->Position.X);
	EXPECT_EQ(20, CableInteraction::ResolveBoundary({ 40, -1e100 }, bounds)->Position.Y);
	EXPECT_EQ(199, CableInteraction::ResolveBoundary({ 40, 1e100 }, bounds)->Position.Y);
	EXPECT_FALSE(CableInteraction::ResolveBoundary({ 10, 50 }, bounds)->Clipped);
	EXPECT_EQ(10, CableInteraction::ResolveBoundary({ 9.99, 50 }, bounds)->Position.X);
	EXPECT_FALSE(CableInteraction::ResolveBoundary({ 50, 50 }, {}));
	EXPECT_FALSE(CableInteraction::ResolveBoundary({ std::numeric_limits<double>::infinity(), 0 }, bounds));
	EXPECT_FALSE(CableInteraction::ResolveBoundary({ 0, std::numeric_limits<double>::quiet_NaN() }, bounds));
}

TEST(CableGeometry, ProjectionAcceptsOffscreenAnchorsButRejectsInvalidDepth)
{
	const utils::Size2d window{ 100u, 200u };
	const auto center = CableInteraction::ProjectAnchor({ 0, 0, 0, 1 }, window);
	ASSERT_TRUE(center);
	EXPECT_DOUBLE_EQ(50.0, center->x);
	EXPECT_DOUBLE_EQ(100.0, center->y);
	const auto distant = CableInteraction::ProjectAnchor({ -1e30f, 1e30f, 0, 1 }, window);
	ASSERT_TRUE(distant);
	EXPECT_LT(distant->x, -1e30);
	EXPECT_GT(distant->y, 1e30);
	EXPECT_TRUE(CableInteraction::ResolveBoundary(*distant, { 0, 0, 100, 200 }));
	EXPECT_FALSE(CableInteraction::ProjectAnchor({ 0, 0, 0, -1 }, window));
	EXPECT_FALSE(CableInteraction::ProjectAnchor({ 0, 0, 0, 0 }, window));
	EXPECT_FALSE(CableInteraction::ProjectAnchor({ 0, 0, -1.01f, 1 }, window));
	EXPECT_FALSE(CableInteraction::ProjectAnchor({ 0, 0, 1.01f, 1 }, window));
	EXPECT_FALSE(CableInteraction::ProjectAnchor({ 0, 0, 0, 1 }, {}));
	const float invalid = std::numeric_limits<float>::quiet_NaN();
	for (const auto& clip : { glm::vec4{ invalid, 0, 0, 1 }, { 0, invalid, 0, 1 },
		{ 0, 0, invalid, 1 }, { 0, 0, 0, invalid } })
		EXPECT_FALSE(CableInteraction::ProjectAnchor(clip, window));
}

TEST(CableGeometry, ClippedFansSeparateAlongEdgesAndApproachVisibleSocketsContinuously)
{
	const utils::Rect2d bounds{ 0, 0, 100, 100 };
	const auto low = CableInteraction::ResolveBoundary(CableInteraction::FannedPoint({ -1000, 50 }, -7, true, bounds), bounds);
	const auto high = CableInteraction::ResolveBoundary(CableInteraction::FannedPoint({ -1000, 50 }, 7, true, bounds), bounds);
	ASSERT_TRUE(low);
	ASSERT_TRUE(high);
	EXPECT_EQ(0, low->Position.X);
	EXPECT_EQ(0, high->Position.X);
	EXPECT_EQ(43, low->Position.Y);
	EXPECT_EQ(57, high->Position.Y);
	const auto above = CableInteraction::ResolveBoundary(CableInteraction::FannedPoint({ 50, 1000 }, 7, false, bounds), bounds);
	EXPECT_EQ(57, above->Position.X);
	EXPECT_EQ(99, above->Position.Y);
	const auto entering = CableInteraction::FannedPoint({ -0.00001, 50 }, 7, true, bounds);
	const auto visible = CableInteraction::FannedPoint({ 0, 50 }, 7, true, bounds);
	EXPECT_NEAR(visible.x, entering.x, 0.0001);
	EXPECT_NEAR(visible.y, entering.y, 0.0001);
	for (const auto offset : { -7.0, 7.0 })
	{
		const auto nearRight = CableInteraction::ResolveBoundary(CableInteraction::FannedPoint({ 95, 50 }, offset, true, bounds), bounds);
		ASSERT_TRUE(nearRight);
		EXPECT_FALSE(nearRight->Clipped);
		EXPECT_GE(nearRight->Position.X, 91);
		EXPECT_LE(nearRight->Position.X, 99);
		const auto nearTop = CableInteraction::ResolveBoundary(CableInteraction::FannedPoint({ 50, 95 }, offset, false, bounds), bounds);
		ASSERT_TRUE(nearTop);
		EXPECT_FALSE(nearTop->Clipped);
	}
}

class HudCableGeometryTests : public ::testing::Test
{
protected:
	static void Layout(const std::shared_ptr<base::GuiElement>& element,
		std::vector<std::shared_ptr<gui::GuiScrollPanel>>& scrolls)
	{
		if (auto stack = std::dynamic_pointer_cast<gui::GuiStackPanel>(element))
			stack->ComputeLayout();
		if (auto scroll = std::dynamic_pointer_cast<gui::GuiScrollPanel>(element))
		{
			scrolls.push_back(scroll);
			if (scroll->Content()) Layout(scroll->Content(), scrolls);
		}
		for (unsigned int index = 0; index < 256; ++index)
		{
			const auto child = element->TryGetChild(static_cast<unsigned char>(index));
			if (!child) break;
			Layout(child, scrolls);
		}
	}
};

TEST_F(HudCableGeometryTests, BothScrolledEndsAndStationBoundaryRetainOriginalRouteIdentity)
{
	gui::GuiHudParams params;
	params.Size = { 1200u, 800u };
	auto hud = std::make_shared<gui::GuiHud>(params);
	engine::RigSnapshot snapshot;
	snapshot.Revision = 42u;
	for (size_t index = 0; index < 12; ++index)
	{
		io::RigFileRouting::TriggerResolution trigger;
		trigger.TriggerIndex = index;
		trigger.TriggerName = "Trigger " + std::to_string(index);
		if (index == 0) trigger.StationIndex = 0u;
		if (index == 11) trigger.Sources.push_back({ io::RigFileRouting::SourceKind::Adc, 19u, {}, true });
		snapshot.Graph.Triggers.push_back(trigger);
	}
	hud->SetRoutingConfig(20u, {}, snapshot);
	hud->SetStationAnchors({ { 0u, "A", glm::dvec2{ -1e30, 400 }, glm::vec4{ 1.0f } } });
	std::vector<std::shared_ptr<gui::GuiScrollPanel>> scrolls;
	Layout(hud, scrolls);
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	hud->BuildInteractionGeometry(endpoints, cables);
	ASSERT_EQ(2u, cables.size());
	const auto capture = std::find_if(cables.begin(), cables.end(), [](const auto& cable) { return cable.Route.Kind == CableInteraction::RouteKind::Capture; });
	ASSERT_NE(cables.end(), capture);
	EXPECT_TRUE(capture->Start.Continuation);
	EXPECT_TRUE(capture->Finish.Continuation);
	EXPECT_EQ(42u, capture->Route.Revision);
	EXPECT_EQ(11u, capture->Route.TriggerIndex);
	EXPECT_EQ(19u, capture->Start.Source->AdcChannel);
	EXPECT_TRUE(cables.front().Finish.Continuation);
	EXPECT_EQ(0, cables.front().Finish.Position.X);
	for (const auto& endpoint : endpoints) EXPECT_FALSE(endpoint.Continuation);
	for (const auto& scroll : scrolls) scroll->SetScrollOffset(scroll->MaxScrollOffset());
	hud->SetStationAnchors({ { 0u, "A", glm::dvec2{ 400, 400 }, glm::vec4{ 1.0f } } });
	hud->BuildInteractionGeometry(endpoints, cables);
	ASSERT_EQ(2u, cables.size());
	EXPECT_FALSE(cables.back().Start.Continuation);
	EXPECT_FALSE(cables.back().Finish.Continuation);
	EXPECT_EQ(42u, cables.back().Route.Revision);
	EXPECT_EQ(11u, cables.back().Route.TriggerIndex);
	hud->SetStationAnchors({ { 0u, "A", std::nullopt, glm::vec4{ 1.0f } } });
	hud->BuildInteractionGeometry(endpoints, cables);
	ASSERT_EQ(1u, cables.size());
	EXPECT_EQ(CableInteraction::RouteKind::Capture, cables.front().Route.Kind);
	EXPECT_EQ(2u, gui::GuiHud::BuildCableRoutes(snapshot.Graph).size());
	hud->SetSize({ 0u, 0u });
	hud->BuildInteractionGeometry(endpoints, cables);
	EXPECT_TRUE(cables.empty());
	EXPECT_TRUE(endpoints.empty());
	EXPECT_EQ(2u, gui::GuiHud::BuildCableRoutes(snapshot.Graph).size());
}

TEST_F(HudCableGeometryTests, PopulatedRoutesAndUnavailableInputsSurviveClippingWithBoundedFans)
{
	gui::GuiHudParams params;
	params.Size = { 1200u, 800u };
	auto hud = std::make_shared<gui::GuiHud>(params);
	engine::RigSnapshot snapshot;
	snapshot.Revision = 43u;
	for (size_t index = 0; index < 20; ++index)
	{
		io::RigFileRouting::TriggerResolution trigger;
		trigger.TriggerIndex = index;
		trigger.TriggerName = "Trigger " + std::to_string(index);
		trigger.StationIndex = 0u;
		trigger.Sources.push_back({ io::RigFileRouting::SourceKind::Adc, static_cast<unsigned int>(index), {}, true });
		if (index == 19) trigger.Sources.push_back({ io::RigFileRouting::SourceKind::Midi, 0u, "Missing", false });
		snapshot.Graph.Triggers.push_back(trigger);
	}
	hud->SetRoutingConfig(20u, {}, snapshot);
	hud->SetStationAnchors({ { 0u, "A", glm::dvec2{ -1e30, 400 }, glm::vec4{ 1.0f } } });
	std::vector<std::shared_ptr<gui::GuiScrollPanel>> scrolls;
	Layout(hud, scrolls);
	std::vector<CableInteraction::Endpoint> endpoints;
	std::vector<CableInteraction::Cable> cables;
	hud->BuildInteractionGeometry(endpoints, cables);
	ASSERT_EQ(41u, cables.size());
	int firstY = 1000;
	int lastY = 0;
	bool unavailable = false;
	for (const auto& cable : cables)
	{
		EXPECT_EQ(43u, cable.Route.Revision);
		if (cable.Route.Kind == CableInteraction::RouteKind::Station)
		{
			EXPECT_TRUE(cable.Finish.Continuation);
			EXPECT_EQ(0, cable.Finish.Position.X);
			EXPECT_GE(cable.Finish.Position.Y, 393);
			EXPECT_LE(cable.Finish.Position.Y, 407);
			firstY = std::min(firstY, cable.Finish.Position.Y);
			lastY = std::max(lastY, cable.Finish.Position.Y);
		}
		else if (cable.Start.Source->MidiDevice == "Missing")
		{
			unavailable = true;
			EXPECT_FALSE(cable.Start.Available);
			EXPECT_EQ(19u, cable.Route.TriggerIndex);
			EXPECT_EQ(1u, cable.Route.RouteIndex);
		}
	}
	EXPECT_LT(firstY, lastY);
	EXPECT_TRUE(unavailable);
}

class HudCableInteractionTests : public HudCableGeometryTests
{
protected:
	void SetUp() override
	{
		auto routing = std::make_shared<engine::RigSnapshot>(); routing->Revision = 42u;
		for (unsigned int index = 0; index < 12; ++index) {
			io::RigFile::Trigger trigger{}; trigger.Id = "trigger-" + std::to_string(index);
			trigger.Name = trigger.Id; if (index == 11) trigger.InputChannels = { 19u };
			routing->Rig.Triggers.push_back(trigger);
			engine::RigSnapshotTrigger runtime{}; runtime.Id = trigger.Id; runtime.RigTriggerIndex = index;
			runtime.Instance = std::make_shared<engine::Trigger>(engine::TriggerParams{});
			routing->Triggers.push_back(runtime);
			io::RigFileRouting::TriggerResolution resolved{};
			resolved.TriggerIndex = index; resolved.TriggerName = trigger.Name;
			if (index == 11) resolved.Sources.push_back({ io::RigFileRouting::SourceKind::Adc, 19u, {}, true });
			routing->Graph.Triggers.push_back(resolved);
		}
		Routing = routing;
		gui::GuiHudParams params; params.Size = { 1200u, 800u }; params.PopupManager = &Popups;
		params.SubmitRigEdit = [this](const io::RigFile& candidate) { Submissions.push_back(candidate); return true; };
		Hud = std::make_shared<gui::GuiHud>(params); Hud->SetRoutingConfig(20u, {}, *Routing);
		Hud->SetCableRevealHeld(true); Hud->Init();
		std::vector<std::shared_ptr<gui::GuiScrollPanel>> scrolls; Layout(Hud, scrolls);
		Hud->BuildInteractionGeometry(Endpoints, Cables); ASSERT_EQ(1u, Cables.size());
		ASSERT_TRUE(Cables.front().Start.Continuation); ASSERT_TRUE(Cables.front().Finish.Continuation);
		const auto target = std::find_if(Endpoints.begin(), Endpoints.end(), [](const auto& endpoint) {
			return endpoint.Source && endpoint.Source->AdcChannel == 1u;
		});
		ASSERT_NE(Endpoints.end(), target); ReplacementSocket = target->Position;
	}
	void BeginBodyDrag()
	{
		const auto curve = CableInteraction::CurveControls(Cables.front().Route.Kind,
			Cables.front().Start.Position, Cables.front().Finish.Position);
		const auto point = CableInteraction::EvaluateCurve(curve, 0.15f);
		actions::TouchAction down; down.Touch = actions::TouchAction::TOUCH_MOUSE;
		down.Index = 0; down.State = actions::TouchAction::TOUCH_DOWN;
		down.Position = { static_cast<int>(point.x), static_cast<int>(point.y) };
		ASSERT_TRUE(CableInteraction::HitCable(Cables, down.Position, 6.0f));
		ASSERT_TRUE(Hud->OnAction(down).IsEaten); ASSERT_TRUE(Hud->HasCableDrag());
	}
	void MoveToReplacement(unsigned int buttons)
	{
		actions::TouchMoveAction move; move.Touch = actions::TouchAction::TOUCH_MOUSE;
		move.MouseButtonsDown = buttons; move.Position = ReplacementSocket;
		EXPECT_TRUE(Hud->OnAction(move).IsEaten);
	}
	void Release()
	{
		actions::TouchAction up; up.Touch = actions::TouchAction::TOUCH_MOUSE;
		up.Index = 0; up.State = actions::TouchAction::TOUCH_UP; up.Position = ReplacementSocket;
		Hud->OnAction(up);
	}
	std::shared_ptr<gui::GuiScrollPanel> TriggerScroll() const
	{
		return std::dynamic_pointer_cast<gui::GuiScrollPanel>(Hud->TryGetChild(1)->TryGetChild(1));
	}
	void Click(const std::shared_ptr<base::GuiElement>& control, bool popup = false)
	{
		ASSERT_TRUE(control); ASSERT_TRUE(control->IsVisible());
		const auto pos = control->GlobalPosition(); const auto size = control->GetSize();
		actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 0;
		touch.Position = { pos.X + static_cast<int>(size.Width / 2), pos.Y + static_cast<int>(size.Height / 2) };
		for (const auto state : { actions::TouchAction::TOUCH_DOWN, actions::TouchAction::TOUCH_UP }) {
			touch.State = state;
			EXPECT_TRUE(popup ? Popups.OnAction(touch).IsEaten : Hud->OnAction(touch).IsEaten);
		}
	}
	void PublishRig(const io::RigFile& rig)
	{
		auto next = std::make_shared<engine::RigSnapshot>(); next->Revision = Routing->Revision + 1; next->Rig = rig;
		for (size_t index = 0; index < rig.Triggers.size(); ++index) {
			const auto& saved = rig.Triggers[index];
			engine::RigSnapshotTrigger runtime{}; runtime.Id = saved.Id; runtime.RigTriggerIndex = index;
			runtime.Instance = std::make_shared<engine::Trigger>(engine::TriggerParams{});
			next->Triggers.push_back(runtime);
			io::RigFileRouting::TriggerResolution resolved{}; resolved.TriggerIndex = index; resolved.TriggerName = saved.Name;
			for (const auto channel : saved.InputChannels)
				resolved.Sources.push_back({ io::RigFileRouting::SourceKind::Adc, channel, {}, true });
			next->Graph.Triggers.push_back(resolved);
		}
		Routing = next; Hud->SetRoutingConfig(20u, {}, *Routing);
	}
	std::shared_ptr<const engine::RigSnapshot> Routing;
	std::vector<io::RigFile> Submissions;
	gui::GuiPopupManager Popups;
	std::shared_ptr<gui::GuiHud> Hud;
	std::vector<CableInteraction::Endpoint> Endpoints;
	std::vector<CableInteraction::Cable> Cables;
	utils::Position2d ReplacementSocket{};
};

TEST_F(HudCableInteractionTests, PointerClearingCancelsClippedBodyDragWithoutSubmittingEdit)
{
	BeginBodyDrag(); ASSERT_TRUE(Hud->HasCableDrag());
	MoveToReplacement(1u); EXPECT_TRUE(Hud->HasCableDrag());
	Hud->ClearPointerState(); EXPECT_FALSE(Hud->HasCableDrag());
	Release(); EXPECT_TRUE(Submissions.empty());
}

TEST_F(HudCableInteractionTests, ZeroButtonMoveCancelsClippedBodyDragInEditorHudPath)
{
	Hud->SetLoopEditorMode(true);
	BeginBodyDrag(); ASSERT_TRUE(Hud->HasCableDrag());
	MoveToReplacement(0u); EXPECT_FALSE(Hud->HasCableDrag());
	Release(); EXPECT_TRUE(Submissions.empty());
}

TEST_F(HudCableInteractionTests, ClippedBodyReconnectPreservesOriginalTriggerAndStaleRevisionCancels)
{
	BeginBodyDrag(); ASSERT_TRUE(Hud->HasCableDrag()); MoveToReplacement(1u); Release();
	ASSERT_EQ(1u, Submissions.size());
	EXPECT_EQ((std::vector<unsigned int>{ 1u }), Submissions.front().Triggers[11].InputChannels);
	EXPECT_EQ(Routing->Rig.Triggers[11].Id, Submissions.front().Triggers[11].Id);
	for (size_t index = 0; index < 11; ++index) EXPECT_TRUE(Submissions.front().Triggers[index].InputChannels.empty());
	Submissions.clear(); BeginBodyDrag(); ASSERT_TRUE(Hud->HasCableDrag());
	auto replacement = *Routing; ++replacement.Revision;
	replacement.Rig.Triggers[11].Id = "replacement-trigger";
	Hud->SetRoutingConfig(20u, {}, replacement); EXPECT_FALSE(Hud->HasCableDrag());
	Release(); EXPECT_TRUE(Submissions.empty());
}

TEST_F(HudCableInteractionTests, AddTriggerRevealsPublishedCardAfterResizeAndSupportsEmptyList)
{
	Hud->SetSize({ 640, 320 }); auto scroll = TriggerScroll(); ASSERT_TRUE(scroll);
	scroll->SetScrollOffset(scroll->MaxScrollOffset());
	Click(Hud->TryGetChild(1)->TryGetChild(2));
	ASSERT_EQ(1u, Submissions.size()); ASSERT_EQ(13u, Submissions.back().Triggers.size());
	EXPECT_FALSE(Submissions.back().Triggers.back().Id.empty());
	const auto added = Submissions.back(); PublishRig(added); scroll = TriggerScroll();
	ASSERT_TRUE(scroll->Content()->TryGetChild(12)); EXPECT_FALSE(scroll->Content()->TryGetChild(13));
	EXPECT_EQ(100u, scroll->Content()->TryGetChild(12)->GetSize().Height);
	EXPECT_EQ(scroll->MaxScrollOffset(), scroll->ScrollOffset());
	const auto beforeExpand = scroll->ScrollOffset();
	Hud->SetSize({ 1200, 800 });
	EXPECT_EQ((std::min)(beforeExpand, scroll->MaxScrollOffset()), scroll->ScrollOffset());
	for (unsigned char index = 0; index < 13; ++index)
		EXPECT_EQ(100u, scroll->Content()->TryGetChild(index)->GetSize().Height);
	io::RigFile empty = added; empty.Triggers.clear(); PublishRig(empty); Submissions.clear();
	EXPECT_FALSE(TriggerScroll()->Content()->TryGetChild(0));
	Click(Hud->TryGetChild(1)->TryGetChild(2)); ASSERT_EQ(1u, Submissions.size());
	ASSERT_EQ(1u, Submissions.back().Triggers.size()); const auto first = Submissions.back(); PublishRig(first);
	EXPECT_EQ(100u, TriggerScroll()->Content()->TryGetChild(0)->GetSize().Height);
}

TEST_F(HudCableInteractionTests, DeleteConfirmationSubmitsOnlySelectedTriggerAndClampsPublishedList)
{
	Hud->SetSize({ 640, 320 }); auto scroll = TriggerScroll(); ASSERT_TRUE(scroll);
	scroll->SetScrollOffset(0);
	const auto close = scroll->Content()->TryGetChild(0)->TryGetChild(3);
	Click(close); ASSERT_TRUE(Popups.IsOpen()); EXPECT_EQ(Hud, Popups.OwnerOfTop());
	Click(Popups.Top()->TryGetChild(4), true); EXPECT_FALSE(Popups.IsOpen()); EXPECT_TRUE(Submissions.empty());
	Click(close); ASSERT_TRUE(Popups.IsOpen()); scroll->SetScrollOffset(scroll->MaxScrollOffset());
	Click(Popups.Top()->TryGetChild(5), true); EXPECT_FALSE(Popups.IsOpen());
	ASSERT_EQ(1u, Submissions.size()); ASSERT_EQ(11u, Submissions.back().Triggers.size());
	EXPECT_EQ(Routing->Rig.Triggers[1].Id, Submissions.back().Triggers.front().Id);
	EXPECT_EQ((std::vector<unsigned int>{ 19u }), Submissions.back().Triggers.back().InputChannels);
	const auto removed = Submissions.back(); PublishRig(removed); scroll = TriggerScroll();
	EXPECT_EQ(scroll->MaxScrollOffset(), scroll->ScrollOffset());
	EXPECT_FALSE(scroll->Content()->TryGetChild(11));
	const auto beforeShrink = scroll->ScrollOffset();
	Hud->SetSize({ 320, 180 }); EXPECT_EQ(beforeShrink, scroll->ScrollOffset());
	for (unsigned char index = 0; index < 11; ++index)
		EXPECT_EQ(100u, scroll->Content()->TryGetChild(index)->GetSize().Height);
}

class CableInteractionTests : public ::testing::Test
{
protected:
	static io::RigFile Rig()
	{
		io::RigFile rig;
		rig.Triggers.resize(2u);
		rig.Triggers[0].Id = "trigger-1-id";
		rig.Triggers[0].Name = "Trigger-1";
		rig.Triggers[0].InputChannels = { 1u };
		rig.Triggers[0].MidiInputs = io::RigFile::Trigger::MidiInputMode::Selected;
		rig.Triggers[0].MidiInputDevices = { "Keys" };
		rig.Triggers[0].StationTarget = "A";
		rig.Triggers[1].Name = "Trigger-2";
		rig.Triggers[1].Id = "trigger-2-id";
		rig.Triggers[1].MidiInputs = io::RigFile::Trigger::MidiInputMode::None;
		rig.Triggers[1].StationTarget = "";
		return rig;
	}

	static CableInteraction::Endpoint Adc(unsigned int channel, int x, int y, bool available = true)
	{
		return { CableInteraction::EndpointKind::AdcSource, { x, y }, {}, {}, {},
			io::RigFileRouting::Source{ io::RigFileRouting::SourceKind::Adc, channel, {}, available }, available };
	}

	static CableInteraction::Endpoint Input(size_t trigger, int x, int y)
	{
		return { CableInteraction::EndpointKind::TriggerInput, { x, y }, trigger };
	}

	static CableInteraction::Endpoint Midi(std::string device, int x, int y, bool available = true)
	{
		return { CableInteraction::EndpointKind::MidiSource, { x, y }, {}, {}, {},
			io::RigFileRouting::Source{ io::RigFileRouting::SourceKind::Midi, 0u, std::move(device), available }, available };
	}
};

TEST_F(CableInteractionTests, SpreadPlacesSingleAtCentreAndManyAtInclusiveEnds)
{
	EXPECT_TRUE(CableInteraction::Spread(10, 30, 0u).empty());
	EXPECT_EQ((std::vector<int>{ 20 }), CableInteraction::Spread(10, 30, 1u));
	EXPECT_EQ((std::vector<int>{ 10, 20, 30 }), CableInteraction::Spread(10, 30, 3u));
}

TEST_F(CableInteractionTests, EndpointHitTestingUsesLargerCircularTargetAndNearestWins)
{
	const std::vector<CableInteraction::Endpoint> endpoints{ Adc(0u, 0, 0), Adc(1u, 8, 0) };
	EXPECT_TRUE(CableInteraction::HitTest(endpoints[0], { 3, 4 }, 5.0f));
	EXPECT_FALSE(CableInteraction::HitTest(endpoints[0], { 6, 0 }, 5.0f));
	EXPECT_EQ(1u, CableInteraction::HitEndpoint(endpoints, { 7, 0 }, 10.0f).value());
}

TEST_F(CableInteractionTests, CableBodyHitAndClosestEndUseScreenSpace)
{
	CableInteraction::Cable cable{ {}, Adc(0u, 0, 0), Input(0u, 100, 0) };
	EXPECT_EQ(0u, CableInteraction::HitCable({ cable }, { 31, 19 }, 2.0f).value());
	EXPECT_FALSE(CableInteraction::HitCable({ cable }, { 50, 0 }, 5.0f).has_value());
	EXPECT_EQ(CableInteraction::End::Start, CableInteraction::ClosestEnd(cable, { 20, 0 }));
	EXPECT_EQ(CableInteraction::End::Finish, CableInteraction::ClosestEnd(cable, { 80, 0 }));
}

TEST_F(CableInteractionTests, ContinuationMarkersAreDecorativeAndCannotSnap)
{
	auto source = Adc(2u, 10, 10);
	source.Continuation = true;
	EXPECT_FALSE(CableInteraction::HitTest(source, { 10, 10 }, 14.0f));
	EXPECT_FALSE(CableInteraction::HitEndpoint({ source }, { 10, 10 }, 14.0f));
	const CableInteraction::Cable cable{ {}, source, Input(0u, 100, 100) };
	EXPECT_FALSE(CableInteraction::HitCableEnd({ cable }, { 10, 10 }, 14.0f));
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture },
		CableInteraction::End::Start, Input(0u, 100, 100), {}, { 10, 10 }, {} };
	EXPECT_FALSE(CableInteraction::Compatible(drag, source, Rig()));
	EXPECT_FALSE(CableInteraction::NearestViable(drag, { source }, Rig(), 14.0f));
}

TEST_F(CableInteractionTests, CurveControlsPreserveCaptureAndStationTangents)
{
	const auto capture = CableInteraction::CurveControls(CableInteraction::RouteKind::Capture, { 0, 0 }, { 100, 0 });
	EXPECT_FLOAT_EQ(0.0f, capture[1].x);
	EXPECT_FLOAT_EQ(50.0f, capture[1].y);
	EXPECT_FLOAT_EQ(50.0f, capture[2].x);
	EXPECT_FLOAT_EQ(0.0f, capture[2].y);
	const auto midpoint = CableInteraction::EvaluateCurve(capture, 0.5f);
	EXPECT_FLOAT_EQ(31.25f, midpoint.x);
	EXPECT_FLOAT_EQ(18.75f, midpoint.y);
	const auto station = CableInteraction::CurveControls(CableInteraction::RouteKind::Station, { 100, 0 }, { 0, 100 });
	EXPECT_FLOAT_EQ(50.0f, station[1].x);
	EXPECT_FLOAT_EQ(0.0f, station[1].y);
	EXPECT_FLOAT_EQ(0.0f, station[2].x);
	EXPECT_FLOAT_EQ(50.0f, station[2].y);
	CableInteraction::Cable cable{ { 0, 0, CableInteraction::RouteKind::Station },
		{ CableInteraction::EndpointKind::TriggerOutput, { 100, 0 } },
		{ CableInteraction::EndpointKind::Station, { 0, 100 } } };
	EXPECT_TRUE(CableInteraction::HitCable({ cable }, { 31, 31 }, 2.0f));
	EXPECT_FALSE(CableInteraction::HitCable({ cable }, { 50, 50 }, 5.0f));
}

TEST_F(CableInteractionTests, SocketHitAndSnapRadiusCannotEscapeContentClip)
{
	auto source = Adc(2u, 10, 10);
	source.HitBounds = utils::Rect2d{ 2, 8, 100, 100 };
	EXPECT_FALSE(CableInteraction::HitTest(source, { 10, 7 }, 14.0f));
	EXPECT_TRUE(CableInteraction::HitTest(source, { 10, 8 }, 14.0f));
	EXPECT_FALSE(CableInteraction::HitEndpoint({ source }, { 10, 7 }, 14.0f));
	const CableInteraction::Cable cable{ {}, source, Input(0u, 100, 100) };
	EXPECT_FALSE(CableInteraction::HitCableEnd({ cable }, { 10, 7 }, 14.0f));
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 100), {}, { 10, 7 }, {} };
	EXPECT_FALSE(CableInteraction::NearestViable(drag, { source }, Rig(), 14.0f));
	CableInteraction::Update(drag, { 10, 8 }, { source }, Rig(), 14.0f, 5.0f);
	ASSERT_TRUE(drag.Snap);
	CableInteraction::Update(drag, { 10, 7 }, { source }, Rig(), 14.0f, 5.0f);
	EXPECT_FALSE(drag.Snap);
	source.HitBounds = utils::Rect2d{};
	EXPECT_FALSE(CableInteraction::HitTest(source, source.Position, 14.0f));
}

TEST_F(CableInteractionTests, RelatedFindsOnlyCablesAttachedToTheHoveredEndpoint)
{
	CableInteraction::Cable capture{ {}, Adc(1u, 0, 0), Input(0u, 100, 0) };
	CableInteraction::Cable station{ { 0u, 0u, CableInteraction::RouteKind::Station, 0u },
		{ CableInteraction::EndpointKind::TriggerOutput, { 0, 0 }, 0u },
		{ CableInteraction::EndpointKind::Station, { 100, 0 }, {}, 1u, "B" } };

	EXPECT_TRUE(CableInteraction::Related(capture, Adc(1u, 0, 0)));
	EXPECT_FALSE(CableInteraction::Related(capture, Adc(2u, 0, 0)));
	EXPECT_TRUE(CableInteraction::Related(capture, Input(0u, 100, 0)));
	EXPECT_FALSE(CableInteraction::Related(capture, Input(1u, 100, 0)));
	EXPECT_TRUE(CableInteraction::Related(station,
		{ CableInteraction::EndpointKind::TriggerOutput, { 0, 0 }, 0u }));
	EXPECT_TRUE(CableInteraction::Related(station,
		{ CableInteraction::EndpointKind::Station, { 100, 0 }, {}, 1u, "B" }));
}

TEST_F(CableInteractionTests, CompatibilityIsDirectionalAvailableAndExcludesDuplicateCapture)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 0), {}, { 0, 0 }, {} };
	EXPECT_FALSE(CableInteraction::Compatible(drag, Adc(1u, 0, 0), rig));
	EXPECT_TRUE(CableInteraction::Compatible(drag, Adc(2u, 0, 0), rig));
	EXPECT_FALSE(CableInteraction::Compatible(drag, Adc(2u, 0, 0, false), rig));
	EXPECT_FALSE(CableInteraction::Compatible(drag, Input(0u, 0, 0), rig));
	drag.OriginalSource = Adc(1u, 0, 0).Source;
	EXPECT_TRUE(CableInteraction::Compatible(drag, Adc(1u, 0, 0), rig));
}

TEST_F(CableInteractionTests, NearestSnapChoosesOnlyViableEndpoint)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 0), {}, { 3, 0 }, {} };
	const std::vector<CableInteraction::Endpoint> endpoints{ Adc(1u, 2, 0), Adc(2u, 8, 0), Adc(3u, 12, 0) };
	EXPECT_EQ(1u, CableInteraction::NearestViable(drag, endpoints, rig, 20.0f).value());
}

TEST_F(CableInteractionTests, HysteresisRetainsSnapUntilOuterRadiusIsExceeded)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 0), {}, { 0, 0 }, {} };
	const std::vector<CableInteraction::Endpoint> endpoints{ Adc(2u, 10, 0), Adc(3u, 35, 0) };
	CableInteraction::Update(drag, { 10, 0 }, endpoints, rig, 10.0f, 5.0f);
	ASSERT_TRUE(drag.Snap.has_value());
	CableInteraction::Update(drag, { 24, 0 }, endpoints, rig, 10.0f, 5.0f);
	EXPECT_EQ(10, drag.Snap->Position.X);
	CableInteraction::Update(drag, { 35, 0 }, endpoints, rig, 10.0f, 5.0f);
	EXPECT_EQ(35, drag.Snap->Position.X);
}

TEST_F(CableInteractionTests, HysteresisUsesCurrentSocketGeometryAndDropsHiddenSockets)
{
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 100), {}, { 10, 10 }, {} };
	auto source = Adc(2u, 10, 10);
	CableInteraction::Update(drag, { 10, 10 }, { source }, Rig(), 14.0f, 5.0f);
	ASSERT_TRUE(drag.Snap);
	source.Position = { 20, 10 };
	CableInteraction::Update(drag, { 10, 10 }, { source }, Rig(), 14.0f, 5.0f);
	ASSERT_TRUE(drag.Snap);
	EXPECT_EQ(20, drag.Snap->Position.X);
	source.HitBounds = utils::Rect2d{ 15, 0, 100, 100 };
	CableInteraction::Update(drag, { 10, 10 }, { source }, Rig(), 14.0f, 5.0f);
	EXPECT_FALSE(drag.Snap);
	CableInteraction::Update(drag, { 20, 10 }, { source }, Rig(), 14.0f, 5.0f);
	ASSERT_TRUE(drag.Snap);
	CableInteraction::Update(drag, { 20, 10 }, {}, Rig(), 14.0f, 5.0f);
	EXPECT_FALSE(drag.Snap);
}

TEST_F(CableInteractionTests, PreviewLeavesOriginalCableUntouchedAndCancelClearsOnlyDrag)
{
	CableInteraction::Drag drag{ {}, CableInteraction::End::Start, Input(0u, 100, 20), {}, { 30, 40 }, {} };
	const auto preview = CableInteraction::Preview(drag);
	EXPECT_EQ(30, preview.first.X);
	EXPECT_EQ(100, preview.second.X);
	std::optional<CableInteraction::Drag> state = drag;
	CableInteraction::Cancel(state);
	EXPECT_FALSE(state.has_value());
}

TEST_F(CableInteractionTests, ReleaseEmptySpaceRemovesCaptureAndSnapReplacesIt)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 0), Adc(1u, 0, 0).Source, { 0, 0 }, {} };
	auto removed = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(removed.Candidate.has_value());
	EXPECT_TRUE(removed.Candidate->Triggers[0].InputChannels.empty());
	EXPECT_EQ("trigger-1-id", removed.Candidate->Triggers[0].Id);
	drag.Snap = Adc(2u, 0, 0);
	auto replaced = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(replaced.Candidate.has_value());
	EXPECT_EQ((std::vector<unsigned int>{ 2u }), replaced.Candidate->Triggers[0].InputChannels);
	EXPECT_EQ("trigger-1-id", replaced.Candidate->Triggers[0].Id);
}

TEST_F(CableInteractionTests, ReleaseSourceToTriggerCreatesCaptureRoute)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, static_cast<size_t>(-1), CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Finish, Adc(3u, 0, 0), {}, { 100, 0 }, Input(1u, 100, 0) };
	auto release = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(release.Candidate.has_value());
	EXPECT_EQ((std::vector<unsigned int>{ 3u }), release.Candidate->Triggers[1].InputChannels);
}

TEST_F(CableInteractionTests, ReleaseTriggerInputEndUnplugsOrMovesExistingCaptureRoute)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Finish, Adc(1u, 0, 0), Adc(1u, 0, 0).Source, { 100, 0 }, {} };
	const auto unplugged = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(unplugged.Candidate.has_value());
	EXPECT_TRUE(unplugged.Candidate->Triggers[0].InputChannels.empty());

	drag.Snap = Input(1u, 100, 0);
	const auto moved = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(moved.Candidate.has_value());
	EXPECT_TRUE(moved.Candidate->Triggers[0].InputChannels.empty());
	EXPECT_EQ((std::vector<unsigned int>{ 1u }), moved.Candidate->Triggers[1].InputChannels);
	EXPECT_EQ("trigger-1-id", moved.Candidate->Triggers[0].Id);
	EXPECT_EQ("trigger-2-id", moved.Candidate->Triggers[1].Id);
}

TEST_F(CableInteractionTests, UnavailableFixedSourceCannotCreateCaptureRoute)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, static_cast<size_t>(-1), CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Finish, Adc(3u, 0, 0, false), {}, { 100, 0 }, Input(1u, 100, 0) };

	EXPECT_FALSE(CableInteraction::Compatible(drag, Input(1u, 100, 0), rig));
	const auto release = CableInteraction::ReleaseToCandidate(drag, rig);
	EXPECT_FALSE(release.Candidate.has_value());
	EXPECT_FALSE(release.Changed);
}

TEST_F(CableInteractionTests, ExplicitMidiDeviceCanBeCreatedAndRemoved)
{
	auto rig = Rig();
	auto keys = Midi("Keys", 0, 0);
	CableInteraction::Drag create{ { 4u, static_cast<size_t>(-1), CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Finish, keys, {}, { 100, 0 }, Input(1u, 100, 0) };
	auto created = CableInteraction::ReleaseToCandidate(create, rig);
	ASSERT_TRUE(created.Candidate.has_value());
	EXPECT_EQ(io::RigFile::Trigger::MidiInputMode::Selected, created.Candidate->Triggers[1].MidiInputs);
	EXPECT_EQ((std::vector<std::string>{ "Keys" }), created.Candidate->Triggers[1].MidiInputDevices);

	CableInteraction::Drag remove{ { 5u, 1u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(1u, 100, 0), keys.Source, { 0, 0 }, {} };
	auto removed = CableInteraction::ReleaseToCandidate(remove, created.Candidate.value());
	ASSERT_TRUE(removed.Candidate.has_value());
	EXPECT_EQ(io::RigFile::Trigger::MidiInputMode::None, removed.Candidate->Triggers[1].MidiInputs);
}

TEST_F(CableInteractionTests, StationCompatibilityAcceptsOnlyCorrectDirection)
{
	auto rig = Rig();
	CableInteraction::Endpoint output{ CableInteraction::EndpointKind::TriggerOutput, { 0, 0 }, 0u };
	CableInteraction::Endpoint station{ CableInteraction::EndpointKind::Station, { 100, 0 }, {}, 1u, "B" };
	CableInteraction::Drag targetDrag{ { 4u, 0u, CableInteraction::RouteKind::Station, 0u },
		CableInteraction::End::Finish, output, {}, { 0, 0 }, {} };
	EXPECT_TRUE(CableInteraction::Compatible(targetDrag, station, rig));
	EXPECT_FALSE(CableInteraction::Compatible(targetDrag, Input(0u, 100, 0), rig));
	targetDrag.MovingEnd = CableInteraction::End::Start;
	EXPECT_TRUE(CableInteraction::Compatible(targetDrag, output, rig));
	EXPECT_FALSE(CableInteraction::Compatible(targetDrag, station, rig));
}

TEST_F(CableInteractionTests, StationReleaseMovesOrUnplugsSingleTarget)
{
	auto rig = Rig();
	CableInteraction::Endpoint output{ CableInteraction::EndpointKind::TriggerOutput, { 0, 0 }, 0u };
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Station, 0u },
		CableInteraction::End::Finish, output, {}, { 100, 0 }, {} };
	auto unplugged = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(unplugged.Candidate.has_value());
	EXPECT_EQ("", unplugged.Candidate->Triggers[0].StationTarget.value());
	drag.Snap = CableInteraction::Endpoint{ CableInteraction::EndpointKind::Station,
		{ 100, 0 }, {}, 1u, "B" };
	auto moved = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(moved.Candidate.has_value());
	EXPECT_EQ("B", moved.Candidate->Triggers[0].StationTarget.value());
	EXPECT_EQ("trigger-1-id", moved.Candidate->Triggers[0].Id);
}

TEST_F(CableInteractionTests, HiddenRoutesDoNotBecomeRevealedWithoutASocketOrExplicitReveal)
{
	CableInteraction::Cable cable;
	cable.Finish = Input(0u, 20, 20);
	EXPECT_FALSE(CableInteraction::Revealed(cable, false, false, std::nullopt));
	EXPECT_TRUE(CableInteraction::Revealed(cable, true, false, std::nullopt));
	EXPECT_TRUE(CableInteraction::Revealed(cable, false, true, std::nullopt));
	EXPECT_TRUE(CableInteraction::Revealed(cable, false, false, cable.Finish));
	EXPECT_FALSE(CableInteraction::Revealed(cable, false, false, Input(1u, 20, 20)));
	// Releasing the key and leaving the socket immediately revokes interaction.
	EXPECT_FALSE(CableInteraction::Revealed(cable, false, false, std::nullopt));
}

TEST_F(CableInteractionTests, EditorStationRoutesRequireExplicitRevealAtTheStationEnd)
{
	CableInteraction::Cable cable;
	cable.Route.Kind = CableInteraction::RouteKind::Station;
	EXPECT_TRUE(CableInteraction::CanGrabEnd(cable, CableInteraction::End::Start, true, false));
	EXPECT_FALSE(CableInteraction::CanGrabEnd(cable, CableInteraction::End::Finish, true, false));
	EXPECT_TRUE(CableInteraction::CanGrabEnd(cable, CableInteraction::End::Finish, true, true));
	EXPECT_TRUE(CableInteraction::CanGrabEnd(cable, CableInteraction::End::Finish, false, false));
	cable.Route.Kind = CableInteraction::RouteKind::Capture;
	EXPECT_TRUE(CableInteraction::CanGrabEnd(cable, CableInteraction::End::Finish, true, false));
}
