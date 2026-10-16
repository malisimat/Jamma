#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "gui/GuiSlider.h"
#include "gui/GuiRack.h"
#include "audio/AudioMixer.h"
#include <algorithm>
#include <cmath>
#include <vector>

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

TEST(GuiSlider, WheelDoesNotStartOrInterruptPointerDrag) {
	auto params = GuiSliderParams::PanelHorizontal("", 120u);
	params.InitValue = 0.25;
	auto slider = std::make_shared<GuiSlider>(params);
	auto wheel = TouchAction();
	wheel.Touch = TouchAction::TOUCH_MOUSE;
	wheel.State = TouchAction::TOUCH_DOWN;
	wheel.Index = 4;
	wheel.Position = { 20, 10 };
	EXPECT_FALSE(slider->OnAction(wheel).IsEaten);
	slider->SetValue(0.5, true);
	EXPECT_DOUBLE_EQ(slider->Value(), 0.5);

	auto press = wheel;
	press.Index = 0;
	ASSERT_TRUE(slider->OnAction(press).IsEaten);
	auto move = TouchMoveAction();
	move.Touch = TouchAction::TOUCH_MOUSE;
	move.Position = { 30, 10 };
	slider->OnAction(move);
	const auto draggedValue = slider->Value();
	EXPECT_GT(draggedValue, 0.5);
	EXPECT_FALSE(slider->OnAction(wheel).IsEaten);
	EXPECT_DOUBLE_EQ(slider->Value(), draggedValue);
	press.State = TouchAction::TOUCH_UP;
	ASSERT_TRUE(slider->OnAction(press).Undo);
	slider->SetValue(0.75, true);
	EXPECT_DOUBLE_EQ(slider->Value(), 0.75);
}

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
	EXPECT_EQ(base::GuiElement::STATE_OVER, slider->GetState());

	moveAction.Position = { 80, 10 };
	slider->OnAction(moveAction);
	EXPECT_FALSE(slider->DragHandleIsOverForTest());
	EXPECT_EQ(base::GuiElement::STATE_OVER, slider->GetState());

	moveAction.Position = { 10, 10 };
	slider->OnAction(moveAction);
	ASSERT_TRUE(slider->DragHandleIsOverForTest());
	slider->ApplyHoverState(false);
	EXPECT_FALSE(slider->DragHandleIsOverForTest());
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, slider->GetState());
}

TEST(GuiSlider, DragHandleOverhangResolvesAndKeepsTrackHovered) {
	auto sliderParams = GuiSliderParams();
	sliderParams.Size = { 100u, 30u };
	sliderParams.DragControlSize = { 20u, 20u };
	sliderParams.DragControlOffset = { -4, 5 };
	sliderParams.InitValue = 0.0;
	sliderParams.Orientation = GuiSliderParams::SLIDER_VERTICAL;
	auto slider = std::make_shared<GuiSlider>(sliderParams);
	slider->SetDragParams({ -4, 5 }, { 20u, 20u }, { 0u, 0u });

	// The thumb extends four pixels to the left of the slider's own rectangle.
	const utils::Position2d overhangPoint{ -2, 10 };
	EXPECT_FALSE(slider->HitTest(overhangPoint));
	EXPECT_TRUE(slider->RouteHitTest(overhangPoint));
	EXPECT_EQ(slider, slider->FindTopmostDescendant(overhangPoint));

	std::static_pointer_cast<base::GuiElement>(slider)->ApplyHoverPoint(overhangPoint);
	EXPECT_TRUE(slider->DragHandleIsOverForTest());
	EXPECT_EQ(base::GuiElement::STATE_OVER, slider->GetState());

	actions::TouchAction down;
	down.Touch = TouchAction::TOUCH_MOUSE;
	down.Position = overhangPoint;
	down.Index = 0;
	down.State = TouchAction::TOUCH_DOWN;
	EXPECT_TRUE(slider->OnAction(down).IsEaten);
	EXPECT_EQ(base::GuiElement::STATE_DOWN, slider->GetState());

	actions::TouchAction up = down;
	up.State = TouchAction::TOUCH_UP;
	EXPECT_TRUE(slider->OnAction(up).IsEaten);
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
	using GuiSlider::BuildScaleMarks;
	using GuiSlider::BuildScaleTrackBounds;
	static const GuiSlider::ScaleMark* FindScaleMark(const std::vector<GuiSlider::ScaleMark>& marks,
		GuiSlider::ScaleMarkKind kind)
	{
		for (const auto& mark : marks)
			if (mark.Kind == kind)
				return &mark;
		return nullptr;
	}
	static GuiSliderParams ParamsForTravel(unsigned int travel)
	{
		auto params = Params();
		params.ScaleMarksEnabled = true;
		params.Size.Height = travel + 36u;
		return params;
	}
	static GuiSliderParams Params()
	{
		GuiSliderParams params;
		params.Size = { 100u, 104u }; // 68px travel: one pixel per dB.
		params.DragControlSize = { 100u, 28u };
		params.DragControlOffset = { 0, 4 };
		params.DragGap = { 4u, 4u };
		params.Min = 0.0;
		params.Max = std::pow(10.0, GuiSliderParams::RackMaxDecibels / 20.0);
		params.Scale = GuiSliderParams::SliderScale::Decibels;
		params.MinDecibels = GuiSliderParams::RackMinDecibels;
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
	EXPECT_NEAR(48.0 / 68.0, params.ValueToFraction(1.0), 1e-12);
	EXPECT_DOUBLE_EQ(0.0, params.ValueToFraction(0.0));
	EXPECT_DOUBLE_EQ(0.0, params.FractionToValue(0.0));
	EXPECT_DOUBLE_EQ(params.Max, params.FractionToValue(1.0));
	EXPECT_EQ(4, DecibelSliderTestData::CalcDragPos(params, params.Size, 0.0).Y);
	EXPECT_EQ(52, DecibelSliderTestData::CalcDragPos(params, params.Size, 1.0).Y);
	EXPECT_EQ(72, DecibelSliderTestData::CalcDragPos(params, params.Size, params.Max).Y);
	EXPECT_EQ(106, DecibelSliderTestData::CalcDragPos(params, { 100u, 180u }, 1.0).Y);
	for (const auto db : { -42.0, -24.0, -6.0, 0.0, 6.0, 18.0, 20.0 }) {
		const auto gain = std::pow(10.0, db / 20.0);
		EXPECT_NEAR(gain, params.FractionToValue(params.ValueToFraction(gain)), 1e-12);
	}
}

struct FaderClickTestData
{
	static TouchAction Pointer(TouchAction::TouchState state, int milliseconds, utils::Position2d position = { 50, 50 })
	{
		TouchAction action;
		action.State = state; action.Position = position;
		action.SetActionTime(Time{} + std::chrono::milliseconds(milliseconds));
		return action;
	}
	static actions::ActionResult Click(const std::shared_ptr<GuiSlider>& slider, int milliseconds,
		utils::Position2d position = { 50, 50 })
	{
		slider->OnAction(Pointer(TouchAction::TOUCH_DOWN, milliseconds, position));
		return slider->OnAction(Pointer(TouchAction::TOUCH_UP, milliseconds + 10, position));
	}
	static std::shared_ptr<GuiSlider> Fader(double gain)
	{
		auto params = DecibelSliderTestData::Params();
		params.InitValue = gain; params.ResetToUnityOnDoubleClick = true;
		return std::make_shared<GuiSlider>(params);
	}
};

TEST(GuiSlider, DoubleClickGainResetUsesExactUnityAndPreservesUndo)
{
	for (const auto gain : { 0.0, 0.25, 4.0, 1.0 })
	{
		auto slider = FaderClickTestData::Fader(gain);
		auto receiver = std::make_shared<MockedSliderReceiver>(1.0); slider->SetReceiver(receiver);
		FaderClickTestData::Click(slider, 0);
		EXPECT_DOUBLE_EQ(gain, slider->Value());
		auto down = slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_DOWN, 100));
		EXPECT_TRUE(down.IsEaten); EXPECT_EQ(slider, down.ActiveElement.lock());
		EXPECT_DOUBLE_EQ(gain, slider->Value());
		auto reset = slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_UP, 110));
		ASSERT_TRUE(reset.IsEaten); ASSERT_TRUE(reset.Undo);
		EXPECT_DOUBLE_EQ(1.0, slider->Value()); EXPECT_DOUBLE_EQ(1.0, receiver->Value());
		auto move = DecibelSliderTestData::Move(100); slider->OnAction(move);
		EXPECT_DOUBLE_EQ(1.0, slider->Value());
		EXPECT_TRUE(reset.Undo->Undo());
		EXPECT_DOUBLE_EQ(gain, slider->Value()); EXPECT_DOUBLE_EQ(gain, receiver->Value());
	}
}

TEST(GuiSlider, DoubleClickRequiresNearbyTimedClicksAndGainOptIn)
{
	for (const auto scenario : { 0, 1, 2, 3, 4 })
	{
		auto params = DecibelSliderTestData::Params();
		params.InitValue = 0.25; params.ResetToUnityOnDoubleClick = scenario != 0;
		auto slider = std::make_shared<GuiSlider>(params);
		FaderClickTestData::Click(slider, 0);
		if (scenario == 3) { slider->SetVisible(false); slider->SetVisible(true); }
		if (scenario == 4) { slider->SetEnabled(false); slider->SetEnabled(true); }
		FaderClickTestData::Click(slider, scenario == 1 ? 10000 : 100,
			scenario == 2 ? utils::Position2d{80, 50} : utils::Position2d{50, 50});
		EXPECT_DOUBLE_EQ(0.25, slider->Value()) << scenario;
	}
}

TEST(GuiSlider, SecondPressDragAndCancellationKeepNormalGainBehavior)
{
	auto slider = FaderClickTestData::Fader(0.25);
	FaderClickTestData::Click(slider, 0);
	slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_DOWN, 100));
	slider->OnAction(DecibelSliderTestData::Move(60));
	const auto dragged = slider->Value(); EXPECT_GT(dragged, 0.25);
	auto release = slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_UP, 110, {50, 60}));
	EXPECT_DOUBLE_EQ(dragged, slider->Value());
	ASSERT_TRUE(release.Undo); release.Undo->Undo(); EXPECT_DOUBLE_EQ(0.25, slider->Value());
	// A completed drag must not become the first half of a reset.
	slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_DOWN, 200));
	slider->OnAction(DecibelSliderTestData::Move(60));
	slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_UP, 210, {50, 60}));
	const auto afterDrag = slider->Value();
	FaderClickTestData::Click(slider, 300, {50, 60}); EXPECT_DOUBLE_EQ(afterDrag, slider->Value());
	slider->ClearPointerState();
	FaderClickTestData::Click(slider, 400);
	slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_DOWN, 500));
	slider->ClearPointerState();
	slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_UP, 510));
	EXPECT_DOUBLE_EQ(afterDrag, slider->Value());
}

TEST(GuiSlider, RackMasterAndChannelFadersEnableUnityReset)
{
	gui::GuiRackParams params; params.Size = {200u, 200u}; params.NumInputChannels = 2u;
	auto rack = std::make_shared<gui::GuiRack>(params); rack->Init();
	rack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
	for (const auto slider : { rack->GetMasterSlider(), rack->GetChannelSlider(0), rack->GetChannelSlider(1) })
	{
		ASSERT_TRUE(slider); slider->SetValue(0.25, true);
		const auto point = utils::Position2d{10, 10};
		FaderClickTestData::Click(slider, 0, point);
		FaderClickTestData::Click(slider, 100, point);
		EXPECT_DOUBLE_EQ(1.0, slider->Value());
	}
}

TEST(GuiSlider, MeterRebindingUsesNewSourceAndDoesNotRetainRetiredMixer)
{
	auto slider = FaderClickTestData::Fader(1.0);
	auto first = std::make_shared<audio::AudioMixer>(audio::AudioMixerParams{});
	auto second = std::make_shared<audio::AudioMixer>(audio::AudioMixerParams{});
	first->UpdateVu(1.0f, 1u); second->UpdateVu(0.125f, 1u);
	slider->SetMixer(first); EXPECT_EQ(first, slider->Mixer());
	std::weak_ptr<audio::AudioMixer> retired = first;
	slider->SetMixer(second); first.reset();
	EXPECT_TRUE(retired.expired()); ASSERT_EQ(second, slider->Mixer());
	EXPECT_FLOAT_EQ(0.125f, slider->Mixer()->VuPeakLevel());
	slider->SetMixer(nullptr); EXPECT_FALSE(slider->Mixer());
}

TEST(GuiSlider, CapturedDoubleClickReleaseAfterRackHideCancelsReset)
{
	for (const bool moveWhileHidden : {false, true})
	{
		gui::GuiRackParams params; params.Size = {200u, 200u};
		auto rack = std::make_shared<gui::GuiRack>(params); rack->Init();
		const auto slider = rack->GetMasterSlider();
		slider->SetValue(0.25, true);
		const auto point = utils::Position2d{10, 10};
		FaderClickTestData::Click(slider, 0, point);
		const auto press = slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_DOWN, 100, point));
		const auto captured = press.ActiveElement.lock(); ASSERT_EQ(slider, captured);
		rack->SetVisible(false); ASSERT_TRUE(slider->IsVisible());
		EXPECT_FALSE(slider->RouteHitTest(point));
		if (moveWhileHidden)
		{
			auto move = DecibelSliderTestData::Move(60);
			EXPECT_TRUE(captured->OnAction(move).IsEaten);
			EXPECT_DOUBLE_EQ(0.25, slider->Value());
		}
		const auto release = captured->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_UP, 110, point));
		EXPECT_TRUE(release.IsEaten); EXPECT_FALSE(release.Undo);
		EXPECT_DOUBLE_EQ(0.25, slider->Value());
		rack->SetVisible(true);
		FaderClickTestData::Click(slider, 200, point); EXPECT_DOUBLE_EQ(0.25, slider->Value());
		FaderClickTestData::Click(slider, 300, point); EXPECT_DOUBLE_EQ(1.0, slider->Value());
	}
}

TEST(GuiSlider, CapturedDoubleClickReleaseAfterAncestorDisableCancelsReset)
{
	for (const bool moveWhileDisabled : {false, true})
	{
		auto ancestor = std::make_shared<base::GuiElement>(base::GuiElementParams{});
		gui::GuiRackParams params; params.Size = {200u, 200u};
		auto rack = std::make_shared<gui::GuiRack>(params); ancestor->AddChild(rack); ancestor->Init();
		const auto slider = rack->GetMasterSlider(); slider->SetValue(0.25, true);
		const auto point = utils::Position2d{10, 10};
		FaderClickTestData::Click(slider, 0, point);
		const auto press = slider->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_DOWN, 100, point));
		const auto captured = press.ActiveElement.lock(); ASSERT_EQ(slider, captured);
		ancestor->SetEnabled(false); ASSERT_TRUE(rack->IsEnabled()); ASSERT_TRUE(slider->IsEnabled());
		EXPECT_FALSE(slider->RouteHitTest(point));
		if (moveWhileDisabled)
		{
			auto move = DecibelSliderTestData::Move(60);
			EXPECT_TRUE(captured->OnAction(move).IsEaten);
			EXPECT_DOUBLE_EQ(0.25, slider->Value());
		}
		const auto release = captured->OnAction(FaderClickTestData::Pointer(TouchAction::TOUCH_UP, 110, point));
		EXPECT_TRUE(release.IsEaten); EXPECT_FALSE(release.Undo);
		EXPECT_DOUBLE_EQ(0.25, slider->Value());
		ancestor->SetEnabled(true);
		FaderClickTestData::Click(slider, 200, point); EXPECT_DOUBLE_EQ(0.25, slider->Value());
		FaderClickTestData::Click(slider, 300, point); EXPECT_DOUBLE_EQ(1.0, slider->Value());
	}
}

TEST(GuiSlider, DoubleClickPublishesUnityThroughExistingMixerSmoothing)
{
	audio::AudioMixerParams params;
	audio::WireMixBehaviourParams wire; wire.Channels = {0u}; params.Behaviour = wire;
	auto mixer = std::make_shared<audio::AudioMixer>(params);
	mixer->SetUnmutedLevel(0.25); mixer->Offset(4096u);
	auto slider = FaderClickTestData::Fader(0.25); slider->SetReceiver(mixer);
	const auto before = mixer->Level();
	FaderClickTestData::Click(slider, 0); FaderClickTestData::Click(slider, 100);
	EXPECT_DOUBLE_EQ(1.0, mixer->UnmutedLevel());
	EXPECT_DOUBLE_EQ(before, mixer->Level()); // GUI changes only the audio target.
	const float input[] = {1.f, 1.f, 1.f, 1.f};
	mixer->WriteBlock(nullptr, input, 4u);
	EXPECT_NEAR(1.0 + (before - 1.0) * std::pow(0.99, 4), mixer->Level(), 1e-12);
	EXPECT_GT(mixer->Level(), before); EXPECT_LT(mixer->Level(), 1.0);
}

TEST(GuiSlider, ScaleMarksAnchorToSilenceUnityAndMaximumHandleCentres) {
	const auto params = DecibelSliderTestData::ParamsForTravel(114u);
	const auto marks = DecibelSliderTestData::BuildScaleMarks(params, params.Size);
	ASSERT_EQ(12u, marks.size());

	const auto* silence = DecibelSliderTestData::FindScaleMark(marks, GuiSlider::ScaleMarkKind::Endpoint);
	ASSERT_NE(nullptr, silence);
	EXPECT_DOUBLE_EQ(0.0, silence->Gain);
	EXPECT_EQ(DecibelSliderTestData::CalcDragPos(params, params.Size, silence->Gain).Y + 14,
		silence->CentreY);

	const auto* unity = DecibelSliderTestData::FindScaleMark(marks, GuiSlider::ScaleMarkKind::Unity);
	ASSERT_NE(nullptr, unity);
	EXPECT_DOUBLE_EQ(1.0, unity->Gain);
	EXPECT_EQ(DecibelSliderTestData::CalcDragPos(params, params.Size, 1.0).Y + 14,
		unity->CentreY);
	EXPECT_EQ(18 + static_cast<int>(std::round(114.0 * 48.0 / 68.0)), unity->CentreY);

	const auto maxCentre = DecibelSliderTestData::CalcDragPos(params, params.Size, params.Max).Y + 14;
	const auto maxEndpoint = std::find_if(marks.begin(), marks.end(), [&params](const auto& mark) {
		return mark.Kind == GuiSlider::ScaleMarkKind::Endpoint && mark.Gain == params.Max;
	});
	ASSERT_NE(marks.end(), maxEndpoint);
	EXPECT_EQ(maxCentre, maxEndpoint->CentreY);
	EXPECT_DOUBLE_EQ(params.Max, maxEndpoint->Gain);
	EXPECT_EQ(marks.end(), std::find_if(marks.begin(), marks.end(), [](const auto& mark) {
		return mark.Kind == GuiSlider::ScaleMarkKind::Intermediate &&
			std::abs(20.0 * std::log10(mark.Gain) - 18.0) < 1e-9;
	}));
	EXPECT_EQ(0u, params.Steps);
}

TEST(GuiSlider, ScaleMarksUseUniformDecibelSubdivisionsAndKeepThePartialTopInterval)
{
	const auto params = DecibelSliderTestData::ParamsForTravel(355u);
	const auto marks = DecibelSliderTestData::BuildScaleMarks(params, params.Size);
	ASSERT_EQ(24u, marks.size());

	for (int index = 0; index <= 22; ++index)
	{
		const auto decibels = -48.0 + 3.0 * index;
		const auto expectedGain = index == 0 ? 0.0 : std::pow(10.0, decibels / 20.0);
		const auto& mark = marks[static_cast<size_t>(index)];
		EXPECT_NEAR(expectedGain, mark.Gain, 1e-12);
		EXPECT_EQ(DecibelSliderTestData::CalcDragPos(params, params.Size, expectedGain).Y + 14,
			mark.CentreY);
		if (index == 0)
			EXPECT_EQ(GuiSlider::ScaleMarkKind::Endpoint, mark.Kind);
		else if (index == 16)
			EXPECT_EQ(GuiSlider::ScaleMarkKind::Unity, mark.Kind);
		else
			EXPECT_EQ(GuiSlider::ScaleMarkKind::Intermediate, mark.Kind);
	}
	EXPECT_NEAR(std::pow(10.0, 18.0 / 20.0), marks[22].Gain, 1e-12);
	EXPECT_DOUBLE_EQ(params.Max, marks.back().Gain);
	EXPECT_EQ(GuiSlider::ScaleMarkKind::Endpoint, marks.back().Kind);
}

TEST(GuiSlider, ScaleMarkCountsFollowHeightThresholdsAndShortFadersKeepAnchors)
{
	const auto shortParams = DecibelSliderTestData::ParamsForTravel(101u);
	const auto regularParams = DecibelSliderTestData::ParamsForTravel(114u);
	const auto justKeepsEndpoint = DecibelSliderTestData::ParamsForTravel(204u);
	const auto tallParams = DecibelSliderTestData::ParamsForTravel(318u);
	const auto tallerParams = DecibelSliderTestData::ParamsForTravel(476u);
	const auto belowFourParams = DecibelSliderTestData::ParamsForTravel(635u);
	const auto tallestParams = DecibelSliderTestData::ParamsForTravel(816u);
	EXPECT_EQ(3u, DecibelSliderTestData::BuildScaleMarks(shortParams, shortParams.Size).size());
	EXPECT_EQ(12u, DecibelSliderTestData::BuildScaleMarks(regularParams, regularParams.Size).size());
	EXPECT_EQ(13u, DecibelSliderTestData::BuildScaleMarks(justKeepsEndpoint, justKeepsEndpoint.Size).size());
	EXPECT_EQ(24u, DecibelSliderTestData::BuildScaleMarks(tallParams, tallParams.Size).size());
	EXPECT_EQ(35u, DecibelSliderTestData::BuildScaleMarks(tallerParams, tallerParams.Size).size());
	EXPECT_EQ(46u, DecibelSliderTestData::BuildScaleMarks(belowFourParams, belowFourParams.Size).size());
	EXPECT_EQ(47u, DecibelSliderTestData::BuildScaleMarks(tallestParams, tallestParams.Size).size());

	const auto regularMarks = DecibelSliderTestData::BuildScaleMarks(regularParams, regularParams.Size);
	ASSERT_EQ(12u, regularMarks.size());
	EXPECT_EQ(GuiSlider::ScaleMarkKind::Endpoint, regularMarks.front().Kind);
	EXPECT_DOUBLE_EQ(0.0, regularMarks.front().Gain);
	EXPECT_EQ(GuiSlider::ScaleMarkKind::Unity, regularMarks[8].Kind);
	EXPECT_DOUBLE_EQ(1.0, regularMarks[8].Gain);
	EXPECT_EQ(GuiSlider::ScaleMarkKind::Endpoint, regularMarks.back().Kind);
	EXPECT_DOUBLE_EQ(regularParams.Max, regularMarks.back().Gain);
	EXPECT_EQ(GuiSlider::ScaleMarkKind::Intermediate, regularMarks[9].Kind);
	EXPECT_NEAR(std::pow(10.0, 6.0 / 20.0), regularMarks[9].Gain, 1e-12);
	const auto retainedEndpointGapMarks = DecibelSliderTestData::BuildScaleMarks(
		justKeepsEndpoint, justKeepsEndpoint.Size);
	EXPECT_NE(retainedEndpointGapMarks.end(), std::find_if(retainedEndpointGapMarks.begin(),
		retainedEndpointGapMarks.end(), [](const auto& mark) {
			return mark.Kind == GuiSlider::ScaleMarkKind::Intermediate &&
				std::abs(20.0 * std::log10(mark.Gain) - 18.0) < 1e-9;
		}));
}

TEST(GuiSlider, ScaleMarksAreOptInAndDegenerateLayoutsReturnNoMarks)
{
	auto params = DecibelSliderTestData::ParamsForTravel(76u);
	params.ScaleMarksEnabled = false;
	EXPECT_TRUE(DecibelSliderTestData::BuildScaleMarks(params, params.Size).empty());

	params.ScaleMarksEnabled = true;
	params.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
	EXPECT_TRUE(DecibelSliderTestData::BuildScaleMarks(params, params.Size).empty());
	params.Orientation = GuiSliderParams::SLIDER_VERTICAL;
	params.Size.Height = params.DragControlSize.Height + 2u * params.DragGap.Height;
	EXPECT_TRUE(DecibelSliderTestData::BuildScaleMarks(params, params.Size).empty());

	params = DecibelSliderTestData::ParamsForTravel(76u);
	params.Max = params.Min;
	EXPECT_TRUE(DecibelSliderTestData::BuildScaleMarks(params, params.Size).empty());
}

TEST(GuiSlider, ScaleMarkDeduplicationPrefersUnityOverCoincidentEndpoint)
{
	const auto params = DecibelSliderTestData::ParamsForTravel(1u);
	const auto marks = DecibelSliderTestData::BuildScaleMarks(params, params.Size);
	ASSERT_EQ(2u, marks.size());
	EXPECT_EQ(GuiSlider::ScaleMarkKind::Endpoint, marks.front().Kind);
	EXPECT_DOUBLE_EQ(0.0, marks.front().Gain);
	EXPECT_EQ(GuiSlider::ScaleMarkKind::Unity, marks.back().Kind);
	EXPECT_DOUBLE_EQ(1.0, marks.back().Gain);
	EXPECT_EQ(marks.back().CentreY,
		DecibelSliderTestData::CalcDragPos(params, params.Size, params.Max).Y + 14);
}

TEST(GuiSlider, ScaleTrackUsesTheHandleTravelAxisAndEndpointCentres)
{
	const auto params = DecibelSliderTestData::ParamsForTravel(114u);
	const auto track = DecibelSliderTestData::BuildScaleTrackBounds(params, params.Size);
	ASSERT_TRUE(track.Valid);
	EXPECT_EQ(46, track.Bounds.Left);
	EXPECT_EQ(54, track.Bounds.Right);
	EXPECT_EQ(18, track.Bounds.Bottom);
	EXPECT_EQ(132, track.Bounds.Top);
	EXPECT_EQ(8, track.Bounds.Right - track.Bounds.Left);

	auto offsetParams = DecibelSliderTestData::Params();
	offsetParams.ScaleMarksEnabled = true;
	offsetParams.DragControlSize = { 40u, 24u };
	offsetParams.DragControlOffset = { 7, 6 };
	offsetParams.DragGap = { 3u, 5u };
	offsetParams.Size = { 100u, 24u + 10u + 114u };
	const auto offsetTrack = DecibelSliderTestData::BuildScaleTrackBounds(offsetParams, offsetParams.Size);
	ASSERT_TRUE(offsetTrack.Valid);
	EXPECT_EQ(23, offsetTrack.Bounds.Left);
	EXPECT_EQ(31, offsetTrack.Bounds.Right);
	EXPECT_EQ(18, offsetTrack.Bounds.Bottom);
	EXPECT_EQ(132, offsetTrack.Bounds.Top);

	const auto tinyTravel = DecibelSliderTestData::ParamsForTravel(3u);
	EXPECT_FALSE(DecibelSliderTestData::BuildScaleTrackBounds(tinyTravel, tinyTravel.Size).Valid);
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

TEST(GuiSlider, VisibleScaleKeepsDraggingContinuousBetweenGridMarks)
{
	auto params = DecibelSliderTestData::ParamsForTravel(114u);
	params.InitValue = 1.0;
	auto slider = std::make_shared<GuiSlider>(params);
	ASSERT_TRUE(slider->OnAction(DecibelSliderTestData::Down(30)).IsEaten);
	slider->OnAction(DecibelSliderTestData::Move(31));
	EXPECT_NEAR(std::pow(10.0, (68.0 / 114.0) / 20.0), slider->Value(), 1e-12);
	EXPECT_EQ(0u, params.Steps);
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
	auto receiver = std::make_shared<MockedSliderReceiver>(0.0);
	slider->SetReceiver(receiver);
	ASSERT_TRUE(slider->OnAction(DecibelSliderTestData::Down(14)).IsEaten);
	slider->OnAction(DecibelSliderTestData::Move(24));
	EXPECT_DOUBLE_EQ(1.0, slider->Value());
	EXPECT_DOUBLE_EQ(0.0, receiver->Value()); // No audio action for degenerate travel.
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
