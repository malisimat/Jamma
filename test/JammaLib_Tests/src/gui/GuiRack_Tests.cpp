#include "gtest/gtest.h"
#include "gui/GuiRack.h"
#include "gui/GuiVstRack.h"

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

TEST(GuiRack, CompactConstructionAndResizeKeepScaleAnchorsDistinct)
{
	GuiRack rack(MakeRackParams(200, 20));
	EXPECT_GE(rack.GetSize().Height, 55u);
	EXPECT_GE(rack.GetMasterSlider()->GetSize().Height, 47u);
	rack.SetNumInputChannels(1);
	rack.SetSize({ 200, 1 });
	EXPECT_GE(rack.GetMasterSlider()->GetSize().Height, 47u);
	EXPECT_GE(rack.GetChannelSlider(0)->GetSize().Height, 47u);
}


TEST(GuiRack, RestoredSlotsReplaceMissingCableRowsAndRetainFailedSlots)
{
	auto view = std::make_shared<gui::GuiVstRack>();
	view->Init();
	gui::GuiVstRack::Model model;
	model.Editable = true;
	model.ChannelRoutes[3].Explicit = true;
	model.ChannelRoutes[3].PluginIds = {"serum"};
	view->Update(model, {});
	ASSERT_TRUE(view->SlotButton(0u));
	EXPECT_EQ(view->SlotButton(0u)->Text(), "Missing");
	gui::GuiVstRack::Slot slot;
	slot.Value.Identity = "serum";
	slot.Value.Path = L"Serum_x64.dll";
	slot.Value.Failed = true;
	model.TotalSlots = 1u;
	model.Slots.push_back(slot);
	view->Update(model, {});
	ASSERT_TRUE(view->SlotButton(0u));
	EXPECT_EQ(view->SlotButton(0u)->Text(), "Retry Serum_x64");
	EXPECT_TRUE(view->SlotButton(0u)->IsEnabled());
	EXPECT_FALSE(view->SlotButton(1u));
}

TEST(GuiRack, SharedSlotViewPreservesCapturedIdentityScrollAndHiddenCableEndpoints)
{
	auto params = MakeRackParams(220, 300);
	int filterChanges = 0;
	std::vector<int> lastAllowedChannels;
	params.OnAllowedMidiChannelsChanged = [&](const auto& channels) { ++filterChanges; lastAllowedChannels = channels; };
	params.EnableVstControl = true; params.EnableVstRack = true; params.EnableVstRackMidiRouting = true;
	auto rack = std::make_shared<GuiRack>(params); rack->SetPosition({300, 400}); rack->Init();
	const auto view = rack->VstRack(); ASSERT_NE(nullptr, view);
	EXPECT_TRUE(view->ChannelsCollapsed());
	view->SetViewport({1200u, 1000u});
	gui::GuiVstRack::Model model; model.Editable = true; model.Revision = 4u; model.TotalSlots = 12u;
	for (auto i = 0u; i < 12u; ++i)
	{
		gui::GuiVstRack::Slot slot; slot.Value.Identity = "slot-" + std::to_string(i);
		slot.Value.Failed = true; model.Slots.push_back(std::move(slot));
	}
	model.ChannelRoutes[2] = {true, {"slot-0", "missing-slot"}};
	std::vector<gui::GuiVstRack::Command> commands;
	const auto submit = [&](auto command) { commands.push_back(std::move(command)); };
	view->Update(model, submit);
	auto expander = view->ChannelsExpander(); ASSERT_NE(nullptr, expander);
	ASSERT_TRUE(expander->RequestFocus());
	actions::KeyAction key; key.KeyChar = 13u; key.KeyActionType = actions::KeyAction::KEY_UP;
	EXPECT_TRUE(view->OnAction(key).IsEaten);
	EXPECT_FALSE(view->ChannelsCollapsed());
	EXPECT_EQ(gui::GuiToggleParams::TOGGLE_ON, expander->GetToggleState());
	key.KeyChar = 32u;
	EXPECT_TRUE(view->OnAction(key).IsEaten);
	EXPECT_TRUE(view->ChannelsCollapsed());
	EXPECT_EQ(gui::GuiToggleParams::TOGGLE_OFF, expander->GetToggleState());
	EXPECT_TRUE(view->ChannelShownActive(2u)); // Cable visibility is separate from live-input admission.
	EXPECT_FALSE(view->ChannelShownActive(3u));
	rack->SetAllowedMidiChannels({1}, true);
	EXPECT_TRUE(view->ChannelShownActive(0u));
	EXPECT_TRUE(view->ChannelShownActive(2u));
	rack->SetAllowedMidiChannels({}, true);
	EXPECT_FALSE(view->ChannelShownActive(0u));
	EXPECT_TRUE(view->ChannelShownActive(2u));
	EXPECT_EQ(0, filterChanges);
	EXPECT_TRUE(commands.empty());
	ASSERT_GT(view->SlotScroll()->MaxScrollOffset(), 0);
	view->SlotScroll()->SetScrollOffset(80);
	const auto slot = view->SlotButton(0u); ASSERT_NE(nullptr, slot);
	EXPECT_NE(nullptr, slot->Parent()); EXPECT_NE(nullptr, slot->Parent()->Parent());
	actions::TouchAction down; down.Index = 0; down.Touch = actions::TouchAction::TOUCH_MOUSE;
	down.State = actions::TouchAction::TOUCH_DOWN; down.Position = {20, 15}; down.Modifiers = base::Action::MODIFIER_ALT;
	slot->OnAction(down);
	model.Revision = 5u; view->Update(model, submit);
	EXPECT_EQ(80, view->SlotScroll()->ScrollOffset());
	auto up = down; up.State = actions::TouchAction::TOUCH_UP; up.Modifiers = base::Action::MODIFIER_NONE;
	slot->OnAction(up);
	ASSERT_EQ(1u, commands.size()); EXPECT_EQ("slot-0", commands[0].Identity);
	EXPECT_EQ(4u, commands[0].Revision); EXPECT_EQ(base::Action::MODIFIER_ALT, commands[0].Modifiers);
	std::vector<gui::CableInteraction::Endpoint> endpoints; std::vector<gui::CableInteraction::Cable> cables;
	view->BuildInteractionGeometry(endpoints, cables); ASSERT_EQ(2u, cables.size());
	EXPECT_TRUE(cables[0].Start.Continuation);
	EXPECT_EQ(2u, cables[0].Start.MidiChannel); EXPECT_EQ("slot-0", cables[0].Finish.PluginId);
	view->SetChannelsCollapsed(true); endpoints.clear(); cables.clear(); view->BuildInteractionGeometry(endpoints, cables);
	ASSERT_EQ(2u, cables.size()); EXPECT_TRUE(cables[0].Start.Continuation);
	EXPECT_EQ(2u, cables[0].Start.MidiChannel); EXPECT_EQ("missing-slot", cables[1].Finish.PluginId);
	EXPECT_FALSE(cables[1].Finish.Available);
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	// A presentation without Station channels still uses the same slot owner.
	auto plain = std::make_shared<gui::GuiVstRack>(); plain->Init(); plain->Update(model, submit);
	endpoints.clear(); cables.clear(); plain->BuildInteractionGeometry(endpoints, cables);
	EXPECT_TRUE(endpoints.empty()); EXPECT_TRUE(cables.empty());
	// The station owns route clearing when a channel is disabled.
	auto receiver = std::make_shared<MockedRackReceiver>(); rack->SetReceiver(receiver);
	GuiAction cabledClick; cabledClick.ElementType = GuiAction::ACTIONELEMENT_TOGGLE;
	cabledClick.Index = GuiRack::MidiChannelToggleBaseIndex + 2u;
	cabledClick.Data = GuiAction::GuiInt{0};
	rack->OnAction(cabledClick);
	EXPECT_EQ(1u, commands.size());
	EXPECT_EQ((std::vector<int>{3}), lastAllowedChannels);
	EXPECT_EQ(1, filterChanges);
	EXPECT_TRUE(view->ChannelShownActive(2u));
	rack->OnAction(cabledClick);
	EXPECT_EQ(1u, commands.size());
	EXPECT_EQ(2, filterChanges);
	EXPECT_TRUE(lastAllowedChannels.empty());
	EXPECT_TRUE(view->ChannelShownActive(2u)); // Disabling input does not erase the displayed cable.
	model.ChannelRoutes[2] = {};
	model.Revision = 6u; view->Update(model, submit);
	EXPECT_FALSE(view->ChannelShownActive(2u));
	rack->OnAction(cabledClick);
	EXPECT_EQ((std::vector<int>{3}), lastAllowedChannels);
	EXPECT_EQ(3, filterChanges);
	EXPECT_TRUE(view->ChannelShownActive(2u));
	EXPECT_EQ(1u, commands.size());
	model.TotalSlots = 1u; model.Slots.resize(1u); view->Update(model, submit);
	const auto append = view->AppendButton(); ASSERT_TRUE(append); EXPECT_EQ(view, append->Parent());
	unsigned int visibleFooterButtons = 0u;
	for (unsigned int index = 0u; const auto child = view->TryGetChild(static_cast<unsigned char>(index)); ++index)
		if (std::dynamic_pointer_cast<gui::GuiButton>(child) &&
			!std::dynamic_pointer_cast<gui::GuiToggle>(child) && child->IsVisible()) ++visibleFooterButtons;
	EXPECT_EQ(1u, visibleFooterButtons);
	down.Position = {static_cast<int>(append->GetSize().Width / 2u), static_cast<int>(append->GetSize().Height / 2u)};
	down.Modifiers = base::Action::MODIFIER_NONE;
	append->OnAction(down); up = down; up.State = actions::TouchAction::TOUCH_UP; append->OnAction(up);
	ASSERT_EQ(2u, commands.size()); EXPECT_EQ(gui::GuiVstRack::Command::Kind::Append, commands.back().Operation);
	EXPECT_EQ(model.Revision, commands.back().Revision);
	plain->DetachGuiOwnershipForRetirement(); rack->DetachGuiOwnershipForRetirement();
}

TEST(GuiRack, DraggingFromChannelButtonStartsCableAndKeepsItLit)
{
	auto params = MakeRackParams(220, 300);
	params.EnableVstControl = true; params.EnableVstRack = true; params.EnableVstRackMidiRouting = true;
	auto rack = std::make_shared<GuiRack>(params); rack->SetPosition({300, 400}); rack->Init();
	auto view = rack->VstRack(); ASSERT_NE(nullptr, view);
	gui::GuiVstRack::Model model; model.Editable = true; model.Revision = 1u;
	view->Update(model, [](auto) {});
	view->SetChannelsCollapsed(false);
	std::vector<gui::CableInteraction::Endpoint> endpoints;
	std::vector<gui::CableInteraction::Cable> cables;
	view->BuildInteractionGeometry(endpoints, cables); ASSERT_EQ(16u, endpoints.size());
	const auto point = endpoints[0].Position + utils::Position2d{-20, 0};
	auto toggle = std::dynamic_pointer_cast<gui::GuiToggle>(view->FindTopmostDescendant(point));
	ASSERT_NE(nullptr, toggle);
	actions::TouchAction down; down.Index = 0; down.Touch = actions::TouchAction::TOUCH_MOUSE;
	down.State = actions::TouchAction::TOUCH_DOWN; down.Position = point;
	const auto pressed = view->OnAction(down);
	EXPECT_EQ(view, pressed.ActiveElement.lock());
	EXPECT_FALSE(view->HasCableDrag());
	EXPECT_EQ(base::GuiElement::STATE_DOWN, toggle->GetState());
	actions::TouchMoveAction move; move.Index = 0; move.Touch = actions::TouchAction::TOUCH_MOUSE;
	move.MouseButtonsDown = 1u; move.Position = point + utils::Position2d{-6, 0};
	view->OnAction(move);
	EXPECT_TRUE(view->HasCableDrag());
	EXPECT_EQ(base::GuiElement::STATE_DOWN, toggle->GetState());
	auto up = down; up.State = actions::TouchAction::TOUCH_UP; up.Position = move.Position;
	view->OnAction(up);
	EXPECT_FALSE(view->HasCableDrag());
	EXPECT_EQ(gui::GuiToggleParams::TOGGLE_OFF, toggle->GetToggleState());
	view->OnAction(down);
	up.Position = point;
	view->OnAction(up);
	EXPECT_EQ(gui::GuiToggleParams::TOGGLE_ON, toggle->GetToggleState());
}

TEST(GuiRack, OptionalVstControlReservesOnlyMasterSliderSpace)
{
	auto params = MakeRackParams(200, 300);
	params.EnableVstControl = true;
	params.NumInputChannels = 1;
	auto rack = std::make_shared<GuiRack>(params);
	const auto button = rack->GetVstControl();
	ASSERT_NE(nullptr, button);
	EXPECT_EQ(gui::GuiButtonParams::DefaultHeight, button->GetSize().Height);
	EXPECT_EQ(rack->GetMasterSlider()->GetSize().Width, button->GetSize().Width);
	EXPECT_LE(button->Position().Y + static_cast<int>(button->GetSize().Height), rack->GetMasterSlider()->Position().Y);
	EXPECT_GT(rack->GetChannelSlider(0)->GetSize().Height, rack->GetMasterSlider()->GetSize().Height);
	rack->SetSize({ 1, 1 });
	EXPECT_GE(rack->GetMasterSlider()->GetSize().Height, 47u);
	EXPECT_GE(rack->GetMasterSlider()->GetSize().Width, 64u);
	EXPECT_LE(rack->GetMasterSlider()->Position().Y + static_cast<int>(rack->GetMasterSlider()->GetSize().Height), static_cast<int>(rack->GetSize().Height));

	params.EnableStationLabel = true;
	auto stationRack = std::make_shared<GuiRack>(params);
	stationRack->Init();
	const auto slider = stationRack->GetMasterSlider();
	const auto label = stationRack->StationLabel();
	ASSERT_NE(nullptr, label);
	EXPECT_EQ(gui::GuiStationLabel::LabelWidth + gui::GuiStationLabel::ButtonGap + gui::GuiStationLabel::ButtonWidth, label->GetSize().Width);
	EXPECT_NEAR(label->Position().X + gui::GuiStationLabel::LabelWidth / 2.0,
		slider->Position().X + slider->GetSize().Width / 2.0, 1.0);
	EXPECT_GT(label->Position().Y, slider->Position().Y + static_cast<int>(slider->GetSize().Height));
	EXPECT_LE(stationRack->GetVstControl()->Position().Y + static_cast<int>(stationRack->GetVstControl()->GetSize().Height), slider->Position().Y);
	const auto bin = label->BinPosition({100, 0});
	EXPECT_EQ(100, bin.X);
	EXPECT_GE(bin.Y, label->GlobalPosition().Y + 34);
}

TEST(GuiRack, VstControlIsAbsentUnlessRequested)
{
	GuiRack rack(MakeRackParams());
	EXPECT_EQ(nullptr, rack.GetVstControl());
}

TEST(GuiRack, VstCommandRetainsPressModifiersThroughTreeInitialization)
{
	auto params = MakeRackParams();
	params.EnableVstControl = true;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);
	rack->Init();
	const auto previousActions = receiver->ActionCount();
	auto button = rack->GetVstControl();
	actions::TouchAction down;
	down.Position = { 10, 10 };
	down.State = actions::TouchAction::TOUCH_DOWN;
	down.Modifiers = base::Action::MODIFIER_ALT;
	auto up = down;
	up.State = actions::TouchAction::TOUCH_UP;
	up.Modifiers = base::Action::MODIFIER_NONE;
	button->OnAction(down);
	button->OnAction(up);
	ASSERT_EQ(previousActions + 1, receiver->ActionCount());
	EXPECT_EQ(GuiAction::ACTIONELEMENT_RACK, receiver->LastAction().ElementType);
	EXPECT_EQ(GuiRack::VstControlActionIndex, receiver->LastAction().Index);
	EXPECT_EQ(base::Action::MODIFIER_ALT, receiver->LastAction().InputModifiers);
}

TEST(GuiRack, VstCallbackConsumesCommandAndLoadingOrReadOnlySuppressesIt)
{
	auto params = MakeRackParams();
	params.EnableVstControl = true;
	unsigned int count = 0;
	params.OnVstControl = [&count](base::Action::Modifiers) { ++count; };
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);
	GuiAction command;
	command.ElementType = GuiAction::ACTIONELEMENT_BUTTON;
	command.Index = GuiRack::VstControlActionIndex;
	rack->OnAction(command);
	EXPECT_EQ(1u, count);
	EXPECT_EQ(0, receiver->ActionCount());
	const auto empty = rack->GetVstControl();
	rack->SetVstControlState(GuiRack::VstControlState::Loading);
	EXPECT_FALSE(empty->IsVisible());
	EXPECT_FALSE(rack->GetVstControl()->IsEnabled());
	rack->OnAction(command);
	rack->SetVstControlState(GuiRack::VstControlState::Open, false);
	rack->OnAction(command);
	EXPECT_EQ(1u, count);
	rack->SetVstControlState(GuiRack::VstControlState::Failed);
	rack->OnAction(command);
	EXPECT_EQ(2u, count);
	EXPECT_TRUE(rack->GetVstControl()->IsEnabled());
}

// --- State transition tests ---

TEST(GuiRack, DefaultStateIsMaster) {
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);

	ASSERT_EQ(GuiRackParams::RACK_MASTER, rack->GetRackState());
}

TEST(GuiRack, SuppressedMasterControlsCannotReceivePointerInput)
{
	auto rack = std::make_shared<GuiRack>(MakeRackParams());
	const auto slider = rack->GetMasterSlider();
	const auto position = slider->GlobalPosition();
	const utils::Position2d pointer = { position.X + 10, position.Y + 10 };
	ASSERT_EQ(slider, rack->FindTopmostDescendant(pointer));

	rack->SetMasterControlsVisible(false);
	EXPECT_TRUE(rack->IsVisible());
	EXPECT_EQ(nullptr, rack->FindTopmostDescendant(pointer));
	rack->SetMasterControlsVisible(true);
	EXPECT_EQ(slider, rack->FindTopmostDescendant(pointer));
}

TEST(GuiRack, HidingExpandedRackResetsBothExpansionLevels)
{
	auto rack = std::make_shared<GuiRack>(MakeRackParams());
	for (const auto state : { GuiRackParams::RACK_CHANNELS, GuiRackParams::RACK_ROUTER })
	{
		rack->SetRackState(state, true);
		rack->SetVisible(false);
		EXPECT_EQ(GuiRackParams::RACK_MASTER, rack->GetRackState());
		rack->SetVisible(true);
		EXPECT_EQ(GuiRackParams::RACK_MASTER, rack->GetRackState());
	}
}

TEST(GuiRack, ViewportCheckIgnoresSuppressionAndExpandedOverhang)
{
	auto rack = std::make_shared<GuiRack>(MakeRackParams());
	base::GuiElementParams hostParams;
	auto host = std::make_shared<base::GuiElement>(hostParams);
	host->AddChild(rack);
	rack->SetMasterControlsVisible(false);
	EXPECT_TRUE(rack->IsInView({ 640, 480 }));
	rack->SetRackState(GuiRackParams::RACK_ROUTER, true);
	host->SetPosition({ -200, 0 });
	EXPECT_FALSE(rack->IsInView({ 640, 480 }));
	host->SetPosition({ -199, 0 });
	EXPECT_TRUE(rack->IsInView({ 640, 480 }));
	host->SetVisible(false);
	EXPECT_FALSE(rack->IsInView({ 640, 480 }));
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

	// Toggle index 1 (RACK_CHANNELS) ON from MASTER state
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

TEST(GuiRack, RuntimeAddedChannelSliderKeepsParentChainAndPointerRouting)
{
	auto params = MakeRackParams();
	auto rack = std::make_shared<GuiRack>(params);
	base::GuiElementParams hostParams;
	hostParams.Size = params.Size;
	auto host = std::make_shared<base::GuiElement>(hostParams);
	host->AddChild(rack);
	rack->SetRackState(GuiRackParams::RACK_CHANNELS, true);
	rack->SetNumInputChannels(1);

	auto slider = rack->GetChannelSlider(0);
	ASSERT_NE(nullptr, slider);
	ASSERT_NE(nullptr, slider->Parent());
	ASSERT_NE(nullptr, slider->Parent()->Parent());
	ASSERT_NE(nullptr, slider->Parent()->Parent()->Parent());
	EXPECT_EQ(rack, slider->Parent()->Parent()->Parent());
	EXPECT_EQ(host, rack->Parent());

	const auto sliderPosition = slider->GlobalPosition();
	const utils::Position2d pointerPosition = {
		sliderPosition.X + static_cast<int>(slider->GetSize().Width / 2),
		sliderPosition.Y + static_cast<int>(slider->GetSize().Height / 2)
	};
	EXPECT_EQ(slider, host->FindTopmostDescendant(pointerPosition));

	actions::TouchMoveAction move;
	move.Touch = actions::TouchAction::TOUCH_MOUSE;
	move.Position = pointerPosition;
	host->OnAction(move);
	EXPECT_EQ(base::GuiElement::STATE_OVER, slider->GetState());

	actions::TouchAction down;
	down.Touch = actions::TouchAction::TOUCH_MOUSE;
	down.State = actions::TouchAction::TOUCH_DOWN;
	down.Position = pointerPosition;
	EXPECT_TRUE(host->OnAction(down).IsEaten);

	actions::TouchAction up = down;
	up.State = actions::TouchAction::TOUCH_UP;
	EXPECT_TRUE(host->OnAction(up).IsEaten);

	host->ClearPointerState();
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, slider->GetState());
	EXPECT_FALSE(slider->DragHandleIsOverForTest());
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
	action.Index = 1;
	action.Data = GuiAction::GuiDouble{ 0.75 };
	rack->OnAction(action);

	ASSERT_EQ(1, receiver->ActionCount());
	ASSERT_EQ(GuiAction::ACTIONELEMENT_RACK, receiver->LastAction().ElementType);
	ASSERT_EQ(1u, receiver->LastAction().Index);

	auto val = std::get<GuiAction::GuiDouble>(receiver->LastAction().Data);
	ASSERT_EQ(0.75, val.Value);
}

TEST(GuiRack, MasterSliderActionKeepsIndexZero) {
	auto params = MakeRackParams();
	params.NumInputChannels = 2;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	action.Index = 0;
	action.Data = GuiAction::GuiDouble{ 0.5 };
	rack->OnAction(action);

	ASSERT_EQ(1, receiver->ActionCount());
	ASSERT_EQ(0u, receiver->LastAction().Index);
}

TEST(GuiRack, ChannelSliderActionDoesNotCollideWithMasterIndex) {
	auto params = MakeRackParams();
	params.NumInputChannels = 2;
	auto rack = std::make_shared<GuiRack>(params);
	auto receiver = std::make_shared<MockedRackReceiver>();
	rack->SetReceiver(receiver);

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	action.Index = 2;
	action.Data = GuiAction::GuiDouble{ 0.25 };
	rack->OnAction(action);

	ASSERT_EQ(1, receiver->ActionCount());
	ASSERT_EQ(2u, receiver->LastAction().Index);
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
