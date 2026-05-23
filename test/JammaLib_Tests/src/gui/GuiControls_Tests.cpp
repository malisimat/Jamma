#include "gtest/gtest.h"
#include "gui/GuiButton.h"
#include "gui/GuiToggle.h"
#include "gui/GuiRadio.h"
#include <stdexcept>

using base::ActionReceiver;
using gui::GuiButton;
using gui::GuiButtonParams;
using gui::GuiToggle;
using gui::GuiToggleParams;
using gui::GuiRadio;
using gui::GuiRadioParams;
using actions::GuiAction;
using actions::TouchAction;

namespace
{
	class MockedGuiReceiver :
		public ActionReceiver
	{
	public:
		MockedGuiReceiver() :
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
		}

		int ActionCount() const { return _actionCount; }
		actions::GuiAction LastAction() const { return _lastAction; }

	private:
		int _actionCount;
		actions::GuiAction _lastAction;
	};

	class ThrowingGuiReceiver :
		public ActionReceiver
	{
	public:
		virtual actions::ActionResult OnAction(actions::GuiAction action) override
		{
			throw std::runtime_error("receiver failure");
		}
	};

	static TouchAction MakeTouchAction(TouchAction::TouchState state, utils::Position2d position)
	{
		TouchAction action;
		action.Touch = TouchAction::TOUCH_MOUSE;
		action.Position = position;
		action.Index = 0;
		action.State = state;
		return action;
	}

	static GuiButtonParams MakeButtonParams(unsigned int index = 0)
	{
		GuiButtonParams params;
		params.Index = index;
		params.Position = { 0, 0 };
		params.Size = { 20, 20 };
		params.MinSize = { 20, 20 };
		return params;
	}

	static GuiToggleParams MakeToggleParams(unsigned int index = 0, unsigned int toggleIndex = 0)
	{
		GuiToggleParams params;
		params.Index = index;
		params.ToggleIndex = toggleIndex;
		params.Position = { 0, 0 };
		params.Size = { 20, 20 };
		params.MinSize = { 20, 20 };
		return params;
	}
}

TEST(GuiButton, TouchInsideEatsDownAndUp) {
	auto button = std::make_shared<GuiButton>(MakeButtonParams());

	auto downRes = button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 }));
	auto upRes = button->OnAction(MakeTouchAction(TouchAction::TOUCH_UP, { 10, 10 }));

	ASSERT_TRUE(downRes.IsEaten);
	ASSERT_TRUE(upRes.IsEaten);
}

TEST(GuiButton, DisabledButtonIgnoresTouch) {
	auto button = std::make_shared<GuiButton>(MakeButtonParams());
	button->SetEnabled(false);

	auto res = button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 }));

	ASSERT_FALSE(res.IsEaten);
}

TEST(GuiButton, TouchOutsideBoundsIsIgnored) {
	auto button = std::make_shared<GuiButton>(MakeButtonParams());

	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { -1, 10 })).IsEaten);
	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 21, 10 })).IsEaten);
	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, -1 })).IsEaten);
	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 21 })).IsEaten);
}

TEST(GuiButton, ExactBoundaryTouchesAreIgnored) {
	auto button = std::make_shared<GuiButton>(MakeButtonParams());

	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 0, 10 })).IsEaten);
	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 0 })).IsEaten);
	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 20, 10 })).IsEaten);
	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 20 })).IsEaten);
}

TEST(GuiButton, NearBoundaryInteriorTouchesAreAccepted) {
	auto button = std::make_shared<GuiButton>(MakeButtonParams());

	ASSERT_TRUE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 1, 10 })).IsEaten);
	ASSERT_TRUE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 1 })).IsEaten);
	ASSERT_TRUE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 19, 10 })).IsEaten);
	ASSERT_TRUE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 19 })).IsEaten);
}

TEST(GuiButton, ZeroSizedButtonNeverEatsTouch) {
	auto params = MakeButtonParams();
	params.Size = { 0, 0 };
	params.MinSize = { 0, 0 };

	auto button = std::make_shared<GuiButton>(params);

	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 0, 0 })).IsEaten);
	ASSERT_FALSE(button->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 1, 1 })).IsEaten);
}

TEST(GuiToggle, TouchUpFlipsStateAndNotifiesReceiver) {
	auto toggle = std::make_shared<GuiToggle>(MakeToggleParams(5, 7));
	auto receiver = std::make_shared<MockedGuiReceiver>();
	toggle->SetReceiver(receiver);

	auto downRes = toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 }));
	auto upRes = toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_UP, { 10, 10 }));

	ASSERT_TRUE(downRes.IsEaten);
	ASSERT_TRUE(upRes.IsEaten);
	ASSERT_EQ(actions::ACTIONRESULT_TOGGLE, upRes.ResultType);
	ASSERT_EQ("5", upRes.SourceId);
	ASSERT_EQ(GuiToggleParams::TOGGLE_ON, toggle->GetToggleState());
	ASSERT_EQ(1, receiver->ActionCount());
	ASSERT_EQ(GuiAction::ACTIONELEMENT_TOGGLE, receiver->LastAction().ElementType);
	ASSERT_EQ(7u, receiver->LastAction().Index);
	ASSERT_EQ(GuiToggleParams::TOGGLE_ON, std::get<GuiAction::GuiInt>(receiver->LastAction().Data).Value);
}

TEST(GuiToggle, TouchUpOutsideAfterTouchDownDoesNotToggle) {
	auto toggle = std::make_shared<GuiToggle>(MakeToggleParams(5, 7));
	auto receiver = std::make_shared<MockedGuiReceiver>();
	toggle->SetReceiver(receiver);

	auto downRes = toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 }));
	auto upRes = toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_UP, { 30, 30 }));

	ASSERT_TRUE(downRes.IsEaten);
	ASSERT_FALSE(upRes.IsEaten);
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, toggle->GetToggleState());
	ASSERT_EQ(0, receiver->ActionCount());
}

TEST(GuiToggle, InterleavedTouchIndexesToggleDeterministically) {
	auto toggle = std::make_shared<GuiToggle>(MakeToggleParams(5, 7));
	auto receiver = std::make_shared<MockedGuiReceiver>();
	toggle->SetReceiver(receiver);

	TouchAction down0 = MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 });
	down0.Index = 0;
	TouchAction down1 = MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 });
	down1.Index = 1;
	TouchAction up0 = MakeTouchAction(TouchAction::TOUCH_UP, { 10, 10 });
	up0.Index = 0;
	TouchAction up1 = MakeTouchAction(TouchAction::TOUCH_UP, { 10, 10 });
	up1.Index = 1;

	ASSERT_TRUE(toggle->OnAction(down0).IsEaten);
	ASSERT_TRUE(toggle->OnAction(down1).IsEaten);
	ASSERT_TRUE(toggle->OnAction(up0).IsEaten);
	ASSERT_TRUE(toggle->OnAction(up1).IsEaten);

	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, toggle->GetToggleState());
	ASSERT_EQ(2, receiver->ActionCount());
}

TEST(GuiToggle, RapidTapSequenceMaintainsConsistentParity) {
	auto toggle = std::make_shared<GuiToggle>(MakeToggleParams(5, 7));
	auto receiver = std::make_shared<MockedGuiReceiver>();
	toggle->SetReceiver(receiver);

	for (int i = 0; i < 9; ++i)
	{
		auto downRes = toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 }));
		auto upRes = toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_UP, { 10, 10 }));
		ASSERT_TRUE(downRes.IsEaten);
		ASSERT_TRUE(upRes.IsEaten);
	}

	ASSERT_EQ(GuiToggleParams::TOGGLE_ON, toggle->GetToggleState());
	ASSERT_EQ(9, receiver->ActionCount());
}

TEST(GuiToggle, ReceiverExceptionPropagatesAndControlRecovers) {
	auto toggle = std::make_shared<GuiToggle>(MakeToggleParams(5, 7));
	toggle->SetReceiver(std::make_shared<ThrowingGuiReceiver>());

	ASSERT_TRUE(toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 })).IsEaten);
	EXPECT_THROW(toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_UP, { 10, 10 })), std::runtime_error);
	ASSERT_EQ(GuiToggleParams::TOGGLE_ON, toggle->GetToggleState());

	auto recoveryReceiver = std::make_shared<MockedGuiReceiver>();
	toggle->SetReceiver(recoveryReceiver);

	ASSERT_TRUE(toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 })).IsEaten);
	ASSERT_TRUE(toggle->OnAction(MakeTouchAction(TouchAction::TOUCH_UP, { 10, 10 })).IsEaten);
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, toggle->GetToggleState());
	ASSERT_EQ(1, recoveryReceiver->ActionCount());
}

TEST(GuiRadio, InitValueSetsSelectedChild) {
	GuiRadioParams params(MakeButtonParams(11));
	params.InitValue = 1;
	params.ToggleParams = {
		MakeToggleParams(0, 0),
		MakeToggleParams(1, 1),
		MakeToggleParams(2, 2)
	};

	auto radio = std::make_shared<GuiRadio>(params);
	radio->Init();

	auto first = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(0));
	auto second = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(1));
	auto third = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(2));

	ASSERT_EQ(1u, radio->CurrentValue());
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, first->GetToggleState());
	ASSERT_EQ(GuiToggleParams::TOGGLE_ON, second->GetToggleState());
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, third->GetToggleState());
}

TEST(GuiRadio, OutOfRangeInitValueKeepsAllChildrenOff) {
	GuiRadioParams params(MakeButtonParams(11));
	params.InitValue = 99;
	params.ToggleParams = {
		MakeToggleParams(0, 0),
		MakeToggleParams(1, 1),
		MakeToggleParams(2, 2)
	};

	auto radio = std::make_shared<GuiRadio>(params);
	radio->Init();

	auto first = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(0));
	auto second = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(1));
	auto third = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(2));

	ASSERT_EQ(99u, radio->CurrentValue());
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, first->GetToggleState());
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, second->GetToggleState());
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, third->GetToggleState());
}

TEST(GuiRadio, EmptyToggleParamsIgnoresChildGuiAction) {
	GuiRadioParams params(MakeButtonParams(11));
	params.InitValue = 7;

	auto radio = std::make_shared<GuiRadio>(params);
	radio->Init();

	GuiAction action;
	action.ElementType = GuiAction::ACTIONELEMENT_TOGGLE;
	action.Index = 0;
	action.Data = GuiAction::GuiInt(1);

	auto res = radio->OnAction(action);

	ASSERT_FALSE(res.IsEaten);
	ASSERT_EQ(7u, radio->CurrentValue());
}

TEST(GuiRadio, ChildToggleUpdatesSelectionAndNotifiesReceiver) {
	GuiRadioParams params(MakeButtonParams(11));
	params.ToggleParams = {
		MakeToggleParams(0, 0),
		MakeToggleParams(1, 1),
		MakeToggleParams(2, 2)
	};

	auto radio = std::make_shared<GuiRadio>(params);
	auto receiver = std::make_shared<MockedGuiReceiver>();
	radio->SetReceiver(receiver);
	radio->Init();

	auto third = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(2));
	ASSERT_NE(nullptr, third);

	third->OnAction(MakeTouchAction(TouchAction::TOUCH_DOWN, { 10, 10 }));
	auto upRes = third->OnAction(MakeTouchAction(TouchAction::TOUCH_UP, { 10, 10 }));

	auto first = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(0));
	auto second = std::dynamic_pointer_cast<GuiToggle>(radio->TryGetChild(1));

	ASSERT_TRUE(upRes.IsEaten);
	ASSERT_EQ(actions::ACTIONRESULT_TOGGLE, upRes.ResultType);
	ASSERT_EQ(2u, radio->CurrentValue());
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, first->GetToggleState());
	ASSERT_EQ(GuiToggleParams::TOGGLE_OFF, second->GetToggleState());
	ASSERT_EQ(GuiToggleParams::TOGGLE_ON, third->GetToggleState());
	ASSERT_EQ(1, receiver->ActionCount());
	ASSERT_EQ(GuiAction::ACTIONELEMENT_RADIO, receiver->LastAction().ElementType);
	ASSERT_EQ(11u, receiver->LastAction().Index);
	ASSERT_EQ(2, std::get<GuiAction::GuiInt>(receiver->LastAction().Data).Value);
}
