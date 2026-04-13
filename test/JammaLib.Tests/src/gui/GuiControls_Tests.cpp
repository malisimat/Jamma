#include "gtest/gtest.h"
#include "gui/GuiButton.h"
#include "gui/GuiToggle.h"
#include "gui/GuiRadio.h"

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
