#include "gtest/gtest.h"
#include "gui/GuiRack.h"

using base::ActionReceiver;
using gui::GuiRack;
using gui::GuiRackParams;
using actions::GuiAction;

// Mock receiver that records the last action forwarded to it
class MockedRackReceiver :
	public ActionReceiver
{
public:
	MockedRackReceiver() :
		ActionReceiver(),
		_actionCount(0),
		_lastAction()
	{}

public:
	virtual actions::ActionResult OnAction(actions::GuiAction action) override
	{
		_lastAction = action;
		_actionCount++;
		return { true, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
	};

	int ActionCount() const { return _actionCount; }
	actions::GuiAction LastAction() const { return _lastAction; }

private:
	int _actionCount;
	actions::GuiAction _lastAction;
};

// Helper to build default rack params with a given size
static GuiRackParams MakeRackParams(unsigned int width = 200, unsigned int height = 300)
{
	GuiRackParams params;
	params.Position = { 0, 0 };
	params.Size = { width, height };
	params.MinSize = { width, height };
	params.NumInputChannels = 0;
	params.NumOutputChannels = 0;
	params.InitLevel = 1.0;
	params.InitState = GuiRackParams::RACK_MASTER;
	return params;
}

// --- State transition tests ---

TEST(GuiRack, DefaultStateIsMaster) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	ASSERT_EQ(GuiRackParams::RACK_MASTER, rack->GetRackState());
}

TEST(GuiRack, InitStateChannels) {
	auto params = MakeRackParams();
	params.InitState = GuiRackParams::RACK_CHANNELS;
	auto rack = std::make_shared<GuiRack>(params);

	ASSERT_EQ(GuiRackParams::RACK_CHANNELS, rack->GetRackState());
}

TEST(GuiRack, InitStateRouter) {
	auto params = MakeRackParams();
	params.InitState = GuiRackParams::RACK_ROUTER;
	auto rack = std::make_shared<GuiRack>(params);

	ASSERT_EQ(GuiRackParams::RACK_ROUTER, rack->GetRackState());
}

TEST(GuiRack, SetRackStateMasterToChannels) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetRackState(GuiRackParams::RACK_CHANNELS, true);
	ASSERT_EQ(GuiRackParams::RACK_CHANNELS, rack->GetRackState());
}

TEST(GuiRack, SetRackStateMasterToRouter) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetRackState(GuiRackParams::RACK_ROUTER, true);
	ASSERT_EQ(GuiRackParams::RACK_ROUTER, rack->GetRackState());
}

TEST(GuiRack, SetRackStateRoundTrip) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetRackState(GuiRackParams::RACK_ROUTER, true);
	ASSERT_EQ(GuiRackParams::RACK_ROUTER, rack->GetRackState());

	rack->SetRackState(GuiRackParams::RACK_CHANNELS, true);
	ASSERT_EQ(GuiRackParams::RACK_CHANNELS, rack->GetRackState());

	rack->SetRackState(GuiRackParams::RACK_MASTER, true);
	ASSERT_EQ(GuiRackParams::RACK_MASTER, rack->GetRackState());
}

// --- Toggle action state transitions ---

TEST(GuiRack, ToggleChannelsOnFromMaster) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	// Toggle index 1 (RACK_CHANNELS) ON
	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_TOGGLE;
	action.Index = GuiRackParams::RACK_CHANNELS;
	action.Data = GuiAction::GuiInt{ 1 }; // toggle on
	rack->OnAction(action);

	ASSERT_EQ(GuiRackParams::RACK_CHANNELS, rack->GetRackState());
}

TEST(GuiRack, ToggleChannelsOffFromChannels) {
	auto params = MakeRackParams();
	params.InitState = GuiRackParams::RACK_CHANNELS;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	// Toggle index 1 (RACK_CHANNELS) OFF
	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_TOGGLE;
	action.Index = GuiRackParams::RACK_CHANNELS;
	action.Data = GuiAction::GuiInt{ 0 }; // toggle off
	rack->OnAction(action);

	ASSERT_EQ(GuiRackParams::RACK_MASTER, rack->GetRackState());
}

TEST(GuiRack, ToggleRouterOnFromChannels) {
	auto params = MakeRackParams();
	params.InitState = GuiRackParams::RACK_CHANNELS;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	// Toggle index 2 (RACK_ROUTER) ON
	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_TOGGLE;
	action.Index = GuiRackParams::RACK_ROUTER;
	action.Data = GuiAction::GuiInt{ 1 }; // toggle on
	rack->OnAction(action);

	ASSERT_EQ(GuiRackParams::RACK_ROUTER, rack->GetRackState());
}

TEST(GuiRack, ToggleRouterOffFromRouter) {
	auto params = MakeRackParams();
	params.InitState = GuiRackParams::RACK_ROUTER;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	// Toggle index 2 (RACK_ROUTER) OFF
	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_TOGGLE;
	action.Index = GuiRackParams::RACK_ROUTER;
	action.Data = GuiAction::GuiInt{ 0 }; // toggle off
	rack->OnAction(action);

	ASSERT_EQ(GuiRackParams::RACK_CHANNELS, rack->GetRackState());
}

TEST(GuiRack, ToggleChannelsOnPreservesRouterWhenVisible) {
	// When router panel is visible and channels toggle is turned on,
	// state should remain RACK_ROUTER
	auto params = MakeRackParams();
	params.InitState = GuiRackParams::RACK_ROUTER;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	// Toggle index 1 (RACK_CHANNELS) ON while in ROUTER state
	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_TOGGLE;
	action.Index = GuiRackParams::RACK_CHANNELS;
	action.Data = GuiAction::GuiInt{ 1 }; // toggle on
	rack->OnAction(action);

	// Since routerPanel is visible, toggling channels on preserves RACK_ROUTER
	ASSERT_EQ(GuiRackParams::RACK_ROUTER, rack->GetRackState());
}

// --- Channel count tests ---

TEST(GuiRack, InitialChannelCountsZero) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	ASSERT_EQ(0u, rack->NumInputChannels());
	ASSERT_EQ(0u, rack->NumOutputChannels());
}

TEST(GuiRack, SetNumInputChannelsCreatesSliders) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetNumInputChannels(3);
	ASSERT_EQ(3u, rack->NumInputChannels());
}

TEST(GuiRack, SetNumOutputChannels) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetNumOutputChannels(4);
	ASSERT_EQ(4u, rack->NumOutputChannels());
}

TEST(GuiRack, SetNumInputChannelsFromParams) {
	auto params = MakeRackParams();
	params.NumInputChannels = 2;
	params.NumOutputChannels = 3;
	auto rack = std::make_shared<GuiRack>(params);

	ASSERT_EQ(2u, rack->NumInputChannels());
	ASSERT_EQ(3u, rack->NumOutputChannels());
}

TEST(GuiRack, IncreaseInputChannels) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetNumInputChannels(2);
	ASSERT_EQ(2u, rack->NumInputChannels());

	rack->SetNumInputChannels(5);
	ASSERT_EQ(5u, rack->NumInputChannels());
}

TEST(GuiRack, DecreaseInputChannels) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetNumInputChannels(5);
	ASSERT_EQ(5u, rack->NumInputChannels());

	rack->SetNumInputChannels(2);
	ASSERT_EQ(2u, rack->NumInputChannels());
}

TEST(GuiRack, IncreaseOutputChannels) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetNumOutputChannels(2);
	ASSERT_EQ(2u, rack->NumOutputChannels());

	rack->SetNumOutputChannels(6);
	ASSERT_EQ(6u, rack->NumOutputChannels());
}

TEST(GuiRack, DecreaseOutputChannels) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	rack->SetNumOutputChannels(6);
	ASSERT_EQ(6u, rack->NumOutputChannels());

	rack->SetNumOutputChannels(1);
	ASSERT_EQ(1u, rack->NumOutputChannels());
}

// --- Route add/clear tests ---

TEST(GuiRack, AddRouteThenClear) {
	auto params = MakeRackParams();
	params.NumInputChannels = 2;
	params.NumOutputChannels = 2;
	auto rack = std::make_shared<GuiRack>(params);

	rack->AddRoute(0, 0);
	rack->AddRoute(1, 1);

	// Routes are delegated to the inner router; clearing should not throw
	rack->ClearRoutes();
}

TEST(GuiRack, AddRouteOutOfRange) {
	// Adding a route with channels that exceed current channel count
	// should not crash (GuiRouter just stores the pair)
	auto params = MakeRackParams();
	params.NumInputChannels = 1;
	params.NumOutputChannels = 1;
	auto rack = std::make_shared<GuiRack>(params);

	rack->AddRoute(99, 99);
	rack->ClearRoutes();
}

TEST(GuiRack, ClearRoutesOnEmptyRouter) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	// ClearRoutes on empty should not crash
	rack->ClearRoutes();
}

// --- Action dispatch tests ---

TEST(GuiRack, SliderActionForwardedAsRack) {
	auto params = MakeRackParams();
	params.NumInputChannels = 2;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	action.Index = 1; // Will be decremented to 0 by GuiRack
	action.Data = GuiAction::GuiDouble{ 0.75 };
	rack->OnAction(action);

	ASSERT_EQ(1, receiver->ActionCount());
	ASSERT_EQ(GuiAction::ACTIONELEMENT_RACK, receiver->LastAction().ElementType);
	ASSERT_EQ(0u, receiver->LastAction().Index);

	auto val = std::get<GuiAction::GuiDouble>(receiver->LastAction().Data);
	ASSERT_EQ(0.75, val.Value);
}

TEST(GuiRack, SliderActionIndexZeroNotDecremented) {
	auto params = MakeRackParams();
	params.NumInputChannels = 2;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	action.Index = 0; // GuiRack only decrements Index when > 0, so 0 passes through unchanged
	action.Data = GuiAction::GuiDouble{ 0.5 };
	rack->OnAction(action);

	ASSERT_EQ(1, receiver->ActionCount());
	ASSERT_EQ(0u, receiver->LastAction().Index);
}

TEST(GuiRack, RouterActionForwardedAsRack) {
	auto params = MakeRackParams();
	params.NumInputChannels = 2;
	params.NumOutputChannels = 2;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	std::vector<std::pair<unsigned int, unsigned int>> connections = { {0, 1}, {1, 0} };

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_ROUTER;
	action.Data = GuiAction::GuiConnections{ connections };
	rack->OnAction(action);

	ASSERT_EQ(1, receiver->ActionCount());
	ASSERT_EQ(GuiAction::ACTIONELEMENT_RACK, receiver->LastAction().ElementType);

	auto conns = std::get<GuiAction::GuiConnections>(receiver->LastAction().Data);
	ASSERT_EQ(2u, conns.Connections.size());
	ASSERT_EQ(0u, conns.Connections[0].first);
	ASSERT_EQ(1u, conns.Connections[0].second);
}

TEST(GuiRack, NoReceiverNoForward) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);
	// No receiver set

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	action.Index = 0;
	action.Data = GuiAction::GuiDouble{ 0.5 };

	// Should not crash when no receiver
	rack->OnAction(action);
}

TEST(GuiRack, DisabledRackDoesNotProcessActions) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	rack->SetEnabled(false);

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	action.Index = 1;
	action.Data = GuiAction::GuiDouble{ 0.5 };
	rack->OnAction(action);

	ASSERT_EQ(0, receiver->ActionCount());
}

TEST(GuiRack, HiddenRackDoesNotProcessActions) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	rack->SetVisible(false);

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	action.Index = 1;
	action.Data = GuiAction::GuiDouble{ 0.5 };
	rack->OnAction(action);

	ASSERT_EQ(0, receiver->ActionCount());
}
