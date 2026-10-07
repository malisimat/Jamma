#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "gui/GuiSlider.h"
#include <cmath>

using base::ActionReceiver;
using resources::ResourceLib;
using gui::GuiSlider;
using gui::GuiSliderParams;
using actions::GuiAction;
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
	virtual actions::ActionResult OnAction(actions::GuiAction action) override
	{
		_value = std::get<actions::GuiAction::GuiDouble>(action.Data).Value;
		return { true, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
	};
	
	bool IsExpected() { return _expected == _value; }
	double Value() const { return _value; }

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

	auto testAction = GuiAction();
	testAction.ElementType = GuiAction::ACTIONELEMENT_SLIDER;
	testAction.Data = GuiAction::GuiDouble{ 2.5 };
	receiver->OnAction(testAction);

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

TEST(GuiSlider, StepsQuantizeDraggedValue) {
	auto dragLength = 100;
	auto dragSize = 20;

	auto sliderParams = GuiSliderParams();
	sliderParams.Position = { 0, 0 };
	sliderParams.Size = { (unsigned int)(dragLength + dragSize), (unsigned int)dragSize };
	sliderParams.DragControlSize = { (unsigned int)dragSize, (unsigned int)dragSize };
	sliderParams.InitValue = 0.0;
	sliderParams.Min = 0.0;
	sliderParams.Max = 1.0;
	sliderParams.Steps = 4;
	sliderParams.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;

	auto slider = std::make_shared<GuiSlider>(sliderParams);

	auto downAction = TouchAction();
	downAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	downAction.Position = { dragSize / 2, dragSize / 2 };
	downAction.Index = 0;
	downAction.State = TouchAction::TOUCH_DOWN;
	slider->OnAction(downAction);

	auto moveAction = TouchMoveAction();
	moveAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	moveAction.Position = { 50, dragSize / 2 };
	moveAction.Index = 0;
	slider->OnAction(moveAction);

	ASSERT_EQ(0.5, slider->Value());
}

TEST(GuiSlider, DragBeyondRangeClampsValue) {
	auto dragLength = 100;
	auto dragSize = 20;

	auto sliderParams = GuiSliderParams();
	sliderParams.Position = { 0, 0 };
	sliderParams.Size = { (unsigned int)(dragLength + dragSize), (unsigned int)dragSize };
	sliderParams.DragControlSize = { (unsigned int)dragSize, (unsigned int)dragSize };
	sliderParams.InitValue = 0.0;
	sliderParams.Min = 0.0;
	sliderParams.Max = 1.0;
	sliderParams.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;

	auto slider = std::make_shared<GuiSlider>(sliderParams);

	auto downAction = TouchAction();
	downAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	downAction.Position = { dragSize / 2, dragSize / 2 };
	downAction.Index = 0;
	downAction.State = TouchAction::TOUCH_DOWN;
	slider->OnAction(downAction);

	auto moveAction = TouchMoveAction();
	moveAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	moveAction.Position = { dragLength + dragSize + 50, dragSize / 2 };
	moveAction.Index = 0;
	slider->OnAction(moveAction);

	ASSERT_EQ(1.0, slider->Value());
}

TEST(GuiSlider, DragCanStartFromTrackAndMovesRelatively) {
	auto dragLength = 100;
	auto dragSize = 20;

	auto sliderParams = GuiSliderParams();
	sliderParams.Position = { 0, 0 };
	sliderParams.Size = { (unsigned int)(dragLength + dragSize), (unsigned int)dragSize };
	sliderParams.DragControlSize = { (unsigned int)dragSize, (unsigned int)dragSize };
	sliderParams.InitValue = 0.0;
	sliderParams.Min = 0.0;
	sliderParams.Max = 1.0;
	sliderParams.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;

	auto slider = std::make_shared<GuiSlider>(sliderParams);

	auto downAction = TouchAction();
	downAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	downAction.Position = { 50, dragSize / 2 };
	downAction.Index = 0;
	downAction.State = TouchAction::TOUCH_DOWN;
	auto downRes = slider->OnAction(downAction);

	ASSERT_TRUE(downRes.IsEaten);

	auto moveAction = TouchMoveAction();
	moveAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	moveAction.Position = { 52, dragSize / 2 };
	moveAction.Index = 0;
	slider->OnAction(moveAction);

	ASSERT_DOUBLE_EQ(0.02, slider->Value());
}

TEST(GuiSlider, HitTestCoversFullSliderTrack) {
	auto sliderParams = GuiSliderParams();
	sliderParams.Position = { 0, 0 };
	sliderParams.Size = { 140u, 20u };
	sliderParams.DragControlSize = { 20u, 20u };
	sliderParams.InitValue = 0.0;
	sliderParams.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;

	auto slider = std::make_shared<GuiSlider>(sliderParams);

	ASSERT_TRUE(slider->HitTest({ 50, 10 }));
	ASSERT_TRUE(slider->HitTest({ 119, 10 }));
	ASSERT_FALSE(slider->HitTest({ 150, 10 }));
}

TEST(GuiSlider, DragHandleRespondsToHover) {
	auto sliderParams = GuiSliderParams();
	sliderParams.Position = { 0, 0 };
	sliderParams.Size = { 140u, 20u };
	sliderParams.DragControlSize = { 20u, 20u };
	sliderParams.InitValue = 0.0;
	sliderParams.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
	sliderParams.DragTexture = "drag";
	sliderParams.DragOverTexture = "drag_over";

	auto slider = std::make_shared<GuiSlider>(sliderParams);

	actions::TouchMoveAction moveAction;
	moveAction.Touch = actions::TouchAction::TOUCH_MOUSE;
	moveAction.Position = { 10, 10 };
	moveAction.Index = 0;
	slider->OnAction(moveAction);

	ASSERT_TRUE(slider->DragHandleIsOverForTest());
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, slider->GetState());

	moveAction.Position = { 80, 10 };
	slider->OnAction(moveAction);
	EXPECT_FALSE(slider->DragHandleIsOverForTest());
	EXPECT_EQ(base::GuiElement::STATE_OVER, slider->GetState());
}

TEST(GuiSlider, ClearingPointerCancelsDragAndDiscardsOffset) {
	GuiSliderParams params;
	params.Size = { 120u, 20u };
	params.DragControlSize = { 20u, 20u };
	params.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
	auto slider = std::make_shared<GuiSlider>(params);
	auto receiver = std::make_shared<MockedSliderReceiver>(0.0);
	slider->SetReceiver(receiver);
	auto down = TouchAction();
	down.Touch = TouchAction::TOUCH_MOUSE;
	down.Position = { 10, 10 };
	down.State = TouchAction::TOUCH_DOWN;
	ASSERT_TRUE(slider->OnAction(down).IsEaten);
	auto move = TouchMoveAction();
	move.Touch = TouchAction::TOUCH_MOUSE;
	move.Position = { 60, 10 };
	slider->OnAction(move);
	ASSERT_GT(slider->Value(), 0.0);
	slider->ClearPointerState();
	EXPECT_DOUBLE_EQ(0.0, slider->Value());
	EXPECT_TRUE(receiver->IsExpected());
	EXPECT_FALSE(slider->OnAction(move).IsEaten);
	EXPECT_DOUBLE_EQ(0.0, slider->Value());
}

class DecibelSliderTestData : public GuiSlider
{
public:
	using GuiSlider::CalcDragPos;
	static GuiSliderParams Params()
	{
		GuiSliderParams params;
		params.Size = { 100u, 112u }; // 76px travel: one pixel per dB.
		params.DragControlSize = { 100u, 28u };
		params.DragControlOffset = { 0, 4 };
		params.DragGap = { 4u, 4u };
		params.Min = 0.0;
		params.Max = std::pow(10.0, 16.0 / 20.0);
		params.Scale = GuiSliderParams::SliderScale::Decibels;
		params.MinDecibels = -60.0;
		params.InitValue = 1.0;
		return params;
	}
	static TouchAction Down(int y)
	{
		TouchAction action;
		action.Touch = TouchAction::TOUCH_MOUSE;
		action.State = TouchAction::TOUCH_DOWN;
		action.Position = { 50, y };
		return action;
	}
	static TouchMoveAction Move(int y)
	{
		TouchMoveAction action;
		action.Touch = TouchAction::TOUCH_MOUSE;
		action.Position = { 50, y };
		return action;
	}
};

TEST(GuiSlider, DecibelMappingPlacesUnityAndEndpointsAndSurvivesResize) {
	const auto params = DecibelSliderTestData::Params();
	EXPECT_NEAR(60.0 / 76.0, params.ValueToFraction(1.0), 1e-12);
	EXPECT_DOUBLE_EQ(0.0, params.ValueToFraction(0.0));
	EXPECT_DOUBLE_EQ(0.0, params.FractionToValue(0.0));
	EXPECT_DOUBLE_EQ(params.Max, params.FractionToValue(1.0));
	EXPECT_EQ(4, DecibelSliderTestData::CalcDragPos(params, params.Size, 0.0).Y);
	EXPECT_EQ(64, DecibelSliderTestData::CalcDragPos(params, params.Size, 1.0).Y);
	EXPECT_EQ(80, DecibelSliderTestData::CalcDragPos(params, params.Size, params.Max).Y);
	EXPECT_EQ(124, DecibelSliderTestData::CalcDragPos(params, { 100u, 188u }, 1.0).Y);
	for (const auto db : { -48.0, -24.0, -6.0, 0.0, 6.0, 16.0 }) {
		const auto gain = std::pow(10.0, db / 20.0);
		EXPECT_NEAR(gain, params.FractionToValue(params.ValueToFraction(gain)), 1e-12);
	}
}

TEST(GuiSlider, DecibelDraggingSendsLinearGainAndTrackGrabStaysRelative) {
	const auto params = DecibelSliderTestData::Params();
	auto slider = std::make_shared<GuiSlider>(params);
	auto receiver = std::make_shared<MockedSliderReceiver>(1.0);
	slider->SetReceiver(receiver);
	// Grab the track away from the handle; no gain jump on mouse down.
	ASSERT_TRUE(slider->OnAction(DecibelSliderTestData::Down(30)).IsEaten);
	EXPECT_DOUBLE_EQ(1.0, slider->Value());
	slider->OnAction(DecibelSliderTestData::Move(30));
	EXPECT_DOUBLE_EQ(1.0, slider->Value());
	slider->OnAction(DecibelSliderTestData::Move(36));
	const auto expected = std::pow(10.0, 6.0 / 20.0);
	EXPECT_NEAR(expected, slider->Value(), 1e-12);
	EXPECT_NEAR(expected, receiver->Value(), 1e-12);
}

TEST(GuiSlider, DecibelDraggingClampsToSilenceAndMaximumAndSupportsUndo) {
	const auto params = DecibelSliderTestData::Params();
	auto slider = std::make_shared<GuiSlider>(params);
	ASSERT_TRUE(slider->OnAction(DecibelSliderTestData::Down(78)).IsEaten);
	slider->OnAction(DecibelSliderTestData::Move(-100));
	EXPECT_DOUBLE_EQ(0.0, slider->Value());
	slider->OnAction(DecibelSliderTestData::Move(500));
	EXPECT_DOUBLE_EQ(params.Max, slider->Value());
	auto up = DecibelSliderTestData::Down(500);
	up.State = TouchAction::TOUCH_UP;
	const auto result = slider->OnAction(up);
	ASSERT_TRUE(result.Undo);
	result.Undo->Undo();
	EXPECT_DOUBLE_EQ(1.0, slider->Value());
}

TEST(GuiSlider, DecibelDragDoesNotChangeGainWhenHandleHasNoTravel) {
	auto params = DecibelSliderTestData::Params();
	params.Size.Height = 28u;
	auto slider = std::make_shared<GuiSlider>(params);
	ASSERT_TRUE(slider->OnAction(DecibelSliderTestData::Down(14)).IsEaten);
	slider->OnAction(DecibelSliderTestData::Move(24));
	EXPECT_DOUBLE_EQ(1.0, slider->Value());
}

TEST(GuiSlider, DecibelDraggingFromRoundedRestoredGainHasNoInitialJump) {
	auto params = DecibelSliderTestData::Params();
	params.InitValue = 0.71;
	auto slider = std::make_shared<GuiSlider>(params);
	ASSERT_TRUE(slider->OnAction(DecibelSliderTestData::Down(40)).IsEaten);
	slider->OnAction(DecibelSliderTestData::Move(40));
	EXPECT_DOUBLE_EQ(0.71, slider->Value());
	slider->OnAction(DecibelSliderTestData::Move(46));
	EXPECT_NEAR(0.71 * std::pow(10.0, 6.0 / 20.0), slider->Value(), 1e-12);
}
