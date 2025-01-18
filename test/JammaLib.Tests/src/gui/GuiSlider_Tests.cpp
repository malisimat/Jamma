
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "gui/GuiSlider.h"

using base::ActionReceiver;
using resources::ResourceLib;
using gui::GuiSlider;
using gui::GuiSliderParams;
using actions::DoubleAction;
using actions::TouchAction;
using actions::TouchMoveAction;

class MockedSliderReceiver :
	public ActionReceiver
{
public:
	MockedSliderReceiver(double expected) :
		ActionReceiver(),
		_expected(expected),
		_value(0.0)	{}
public:
	virtual actions::ActionResult OnAction(actions::DoubleAction action) override
	{
		_value = action.Value();
		return { true, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
	};
	
	bool IsExpected() { return _expected == _value; }

private:
	double _expected;
	double _value;
};

TEST(GuiSlider, DoesUndo) {
	auto dragLength = 100;
	auto dragSize = 20;
	auto dragGap = 2;

	auto sliderParams = GuiSliderParams();
	sliderParams.Position = { 0, 0 };
	sliderParams.Size = { (unsigned int)dragSize, (unsigned int)(dragLength + dragSize) };
	sliderParams.DragGap = { (unsigned int)dragGap, (unsigned int)dragGap };
	sliderParams.DragControlSize = { (unsigned int)dragSize, (unsigned int)dragSize };
	sliderParams.DragControlOffset = { 0, 0 };
	sliderParams.InitValue = 0.0;
	sliderParams.Orientation = GuiSliderParams::SLIDER_VERTICAL;

	auto slider = std::make_shared<GuiSlider>(sliderParams);
	ASSERT_EQ(0.0, slider->Value());

	auto downAction = TouchAction();
	downAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	downAction.Position = { dragSize / 2, dragSize / 2 };
	downAction.Index = 0;
	downAction.State = TouchAction::TOUCH_DOWN;
	slider->OnAction(downAction);

	auto moveAction = TouchMoveAction();
	moveAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	moveAction.Position = { dragSize / 2, dragLength + dragSize / 2 };
	moveAction.Index = 0;
	slider->OnAction(moveAction);

	ASSERT_EQ(1.0, slider->Value());

	auto upAction = TouchAction();
	upAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	upAction.Position = { dragSize / 2, dragLength + dragSize / 2 };
	upAction.Index = 0;
	upAction.State = TouchAction::TOUCH_UP;
	auto res = slider->OnAction(upAction);

	ASSERT_TRUE(res.IsEaten);

	res.Undo->Undo();

	ASSERT_EQ(0.0, slider->Value());
}

TEST(GuiSlider, ReceiverGetsValue) {
	auto expectedValue = 0.5;
	auto dragLength = 100;
	auto dragSize = 20;
	auto dragGap = 5;

	auto sliderParams = GuiSliderParams();
	sliderParams.Position = { 0, 0 };
	sliderParams.Size = { (unsigned int)(dragLength + dragSize + (2 * dragGap)), (unsigned int)dragSize };
	sliderParams.DragGap = { (unsigned int)dragGap, (unsigned int)dragGap };
	sliderParams.DragControlSize = { (unsigned int)dragSize, (unsigned int)dragSize };
	sliderParams.DragControlOffset = { 0, 0 };
	sliderParams.InitValue = 0.0;
	sliderParams.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;

	auto slider = std::make_shared<GuiSlider>(sliderParams);

	auto receiver = std::make_shared<MockedSliderReceiver>(expectedValue);
	ASSERT_FALSE(receiver->IsExpected());
	receiver->OnAction(DoubleAction(2.5));

	slider->SetReceiver(receiver);
	ASSERT_EQ(0.0, slider->Value());

	auto downAction = TouchAction();
	downAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	downAction.Position = { dragSize / 2, dragSize / 2 };
	downAction.Index = 0;
	downAction.State = TouchAction::TOUCH_DOWN;
	slider->OnAction(downAction);

	auto moveAction = TouchMoveAction();
	moveAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	moveAction.Position = { ((int)(dragLength * expectedValue)) + dragSize / 2, dragSize / 2,  };
	moveAction.Index = 0;
	slider->OnAction(moveAction);

	ASSERT_EQ(expectedValue, slider->Value());
	ASSERT_TRUE(receiver->IsExpected());
}