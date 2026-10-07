#include "gtest/gtest.h"
#include "gui/GuiButton.h"
#include "gui/GuiToggle.h"
#include "gui/GuiFocusManager.h"
#include "gui/GuiPopup.h"
#include "gui/GuiPopupManager.h"
#include "gui/GuiTextBox.h"
#include "gui/GuiNumericInput.h"
#include "gui/GuiDropDown.h"
#include "gui/GuiScrollBar.h"
#include "gui/GuiScrollPanel.h"
#include "gui/GuiMainPanel.h"
#include "engine/Scene.h"
#include "graphics/GlDrawContext.h"
#include <limits>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<graphics::GlDrawContext>);
static_assert(!std::is_copy_assignable_v<graphics::GlDrawContext>);
#include "actions/KeyAction.h"
#include "actions/TouchAction.h"
#include "actions/TouchMoveAction.h"
#include "base/Action.h"

using base::Action;
using gui::GuiButton;
using gui::GuiButtonParams;
using gui::GuiToggle;
using gui::GuiToggleParams;
using gui::GuiFocusManager;
using gui::GuiPopup;
using gui::GuiPopupButtonConfig;
using gui::GuiPopupManager;
using gui::GuiTextBox;
using gui::GuiTextBoxParams;
using gui::GuiNumericInput;
using gui::GuiNumericInputParams;
using gui::GuiDropDown;
using gui::GuiDropDownParams;
using gui::GuiScrollBar;
using gui::GuiScrollBarParams;
using gui::GuiScrollPanel;
using gui::GuiScrollPanelParams;
using gui::GuiScrollOrientation;
using actions::KeyAction;
using actions::TouchAction;
using actions::TouchMoveAction;
using actions::GuiAction;

class GuiPhase3RecordingGuiReceiver : public base::ActionReceiver
{
public:
	actions::ActionResult OnAction(actions::GuiAction action) override
	{
		LastAction = action;
		ActionCount++;
		return actions::ActionResult::NoAction();
	}

	int ActionCount = 0;
	std::optional<actions::GuiAction> LastAction;
};

static GuiButtonParams MakeSizedButton(utils::Position2d pos, utils::Size2d size)
{
	GuiButtonParams p;
	p.Position = pos;
	p.Size = size;
	p.MinSize = size;
	return p;
}

static KeyAction MakeKey(unsigned int vk,
	int type = KeyAction::KEY_DOWN,
	int modifiers = 0)
{
	KeyAction k;
	k.KeyChar = vk;
	k.KeyActionType = (decltype(k.KeyActionType))type;
	k.Modifiers = (Action::Modifiers)modifiers;
	return k;
}

static TouchAction MakeTouch(TouchAction::TouchState state, utils::Position2d pos)
{
	TouchAction a;
	a.Touch = TouchAction::TOUCH_MOUSE;
	a.Position = pos;
	a.Index = 0;
	a.State = state;
	return a;
}

static TouchMoveAction MakeTouchMove(utils::Position2d pos)
{
	TouchMoveAction a;
	a.Touch = TouchAction::TOUCH_MOUSE;
	a.Position = pos;
	a.Index = 0;
	return a;
}

static void TypeChars(const std::shared_ptr<GuiTextBox>& tb, const std::string& vkeys)
{
	for (char vk : vkeys)
		tb->OnAction(MakeKey((unsigned int)(unsigned char)vk));
}

// GuiFocusManager tests

TEST(GuiFocusManager, RequestFocusIsSingleOwner) {
	GuiFocusManager fm;
	auto a = std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 20, 20 }));
	auto b = std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 20, 20 }));

	ASSERT_TRUE(fm.RequestFocus(a));
	ASSERT_TRUE(a->HasFocus());

	ASSERT_TRUE(fm.RequestFocus(b));
	EXPECT_FALSE(a->HasFocus());
	EXPECT_TRUE(b->HasFocus());
	EXPECT_TRUE(fm.HasFocus(b));
}

TEST(GuiFocusManager, ClearFocusReleasesOwner) {
	GuiFocusManager fm;
	auto a = std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 20, 20 }));

	fm.RequestFocus(a);
	fm.ClearFocus();

	EXPECT_FALSE(a->HasFocus());
	EXPECT_EQ(nullptr, fm.CurrentFocus());
}

TEST(GuiFocusManager, IsEditingTextTracksFocusedTextBox) {
	GuiFocusManager fm;
	GuiTextBoxParams tp;
	tp.Size = { 80, 24 };
	tp.MinSize = { 80, 24 };
	auto tb = std::make_shared<GuiTextBox>(tp);

	EXPECT_FALSE(fm.IsEditingText());
	fm.RequestFocus(tb);
	EXPECT_TRUE(fm.IsEditingText());
	fm.ClearFocus();
	EXPECT_FALSE(fm.IsEditingText());
}

// GuiPopupManager tests

TEST(GuiPopupManager, OpenAndCloseTrackTopmost) {
	GuiPopupManager host;
	auto popup = std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 }));

	EXPECT_FALSE(host.IsOpen());
	host.Open(popup);
	EXPECT_TRUE(host.IsOpen());
	EXPECT_EQ(popup, host.Top());

	host.Close();
	EXPECT_FALSE(host.IsOpen());
}

TEST(GuiElement, ExclusiveHoverSelectsOnlyTheTopmostOverlap) {
	auto root = std::make_shared<base::GuiElement>(base::GuiElementParams{});
	root->SetSize({ 40u, 20u });
	auto underneath = std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 30, 20 }));
	auto topmost = std::make_shared<GuiButton>(MakeSizedButton({ 20, 0 }, { 20, 20 }));
	root->AddChild(underneath);
	root->AddChild(topmost);

	root->ApplyExclusiveHoverPoint({ 25, 10 });

	EXPECT_EQ(base::GuiElement::STATE_NORMAL, underneath->GetState());
	EXPECT_EQ(base::GuiElement::STATE_OVER, topmost->GetState());
}

TEST(GuiElement, ExclusiveHoverReachesScrollPanelContent) {
	GuiScrollPanelParams params;
	params.Size = { 100u, 50u };
	params.MinSize = params.Size;
	auto panel = std::make_shared<GuiScrollPanel>(params);
	auto button = std::make_shared<GuiButton>(MakeSizedButton({ 5, 75 }, { 40, 20 }));
	base::GuiElementParams contentParams;
	contentParams.Size = { 80u, 100u };
	contentParams.MinSize = contentParams.Size;
	auto content = std::make_shared<base::GuiElement>(contentParams);
	content->AddChild(button);
	panel->SetContent(content);

	panel->ApplyExclusiveHoverPoint({ 10, 30 });

	EXPECT_EQ(base::GuiElement::STATE_OVER, button->GetState());
}

TEST(GuiPopupManager, OutsidePressDismissesAndConsumes) {
	GuiPopupManager host;
	auto popup = std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 }));
	host.Open(popup);

	auto res = host.OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 0, 0 }));

	EXPECT_TRUE(res.IsEaten);
	EXPECT_FALSE(host.IsOpen());
}

TEST(GuiPopupManager, InsidePressRoutesToPopup) {
	GuiPopupManager host;
	auto popup = std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 }));
	host.Open(popup);

	auto res = host.OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 110, 110 }));

	EXPECT_TRUE(res.IsEaten);
	EXPECT_TRUE(host.IsOpen());
}

TEST(GuiPopupManager, EscapeDismissesTopmost) {
	GuiPopupManager host;
	auto popup = std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 }));
	host.Open(popup);

	auto res = host.OnAction(MakeKey(27, KeyAction::KEY_UP));

	EXPECT_TRUE(res.IsEaten);
	EXPECT_FALSE(host.IsOpen());
}

TEST(GuiPopupManager, OpenClearsOwnerPointerPresentation) {
	GuiPopupManager host;
	auto owner = std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 40, 20 }));
	auto sibling = std::make_shared<GuiButton>(MakeSizedButton({ 20, 0 }, { 20, 20 }));
	auto root = std::make_shared<base::GuiElement>(base::GuiElementParams{});
	root->SetSize({ 40u, 20u });
	root->AddChild(owner);
	root->AddChild(sibling);

	owner->ApplyHoverState(true);
	sibling->ApplyHoverState(true);
	host.Open(std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 })), owner);

	EXPECT_EQ(base::GuiElement::STATE_NORMAL, owner->GetState());
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, sibling->GetState());
}

TEST(GuiPopup, ConfirmationButtonsArePackedAtTheLowerRightAndDispatch) {
	auto popup = std::make_shared<GuiPopup>();
	GuiPopupButtonConfig config;
	config.Actions = { { "Cancel", 2u }, { "Delete", 1u } };
	popup->ConfigureButtons(config);

	auto cancelButton = std::dynamic_pointer_cast<GuiButton>(popup->TryGetChild(4u));
	auto deleteButton = std::dynamic_pointer_cast<GuiButton>(popup->TryGetChild(5u));
	ASSERT_NE(nullptr, cancelButton);
	ASSERT_NE(nullptr, deleteButton);
	EXPECT_EQ(nullptr, popup->TryGetChild(6u));
	EXPECT_EQ(144, cancelButton->Position().X);
	EXPECT_EQ(24, cancelButton->Position().Y);
	EXPECT_EQ(284, deleteButton->Position().X);
	EXPECT_EQ(24, deleteButton->Position().Y);

	auto receiver = std::make_shared<GuiPhase3RecordingGuiReceiver>();
	popup->SetButtonReceiver(receiver);
	popup->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 209, 25 }));
	popup->OnAction(MakeTouch(TouchAction::TOUCH_UP, { 209, 25 }));
	ASSERT_EQ(1, receiver->ActionCount);
	ASSERT_TRUE(receiver->LastAction.has_value());
	EXPECT_EQ(GuiAction::ACTIONELEMENT_BUTTON, receiver->LastAction->ElementType);
	EXPECT_EQ(2u, receiver->LastAction->Index);

	popup->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 317, 25 }));
	popup->OnAction(MakeTouch(TouchAction::TOUCH_UP, { 317, 25 }));
	ASSERT_EQ(2, receiver->ActionCount);
	ASSERT_TRUE(receiver->LastAction.has_value());
	EXPECT_EQ(1u, receiver->LastAction->Index);
}

TEST(GuiPopup, FitsViewportAndKeepsContentAndActionsInsideAfterResize)
{
	auto popup = std::make_shared<GuiPopup>();
	popup->SetTitle("Current server tempo");
	popup->SetBodyLines({ "Tempo: 120 BPM", "Remote master interval", "Apply locally?" });
	popup->ConfigureButtons({ { { "Cancel", 2u }, { "Follow server", 1u } } });
	auto receiver = std::make_shared<GuiPhase3RecordingGuiReceiver>(); popup->SetButtonReceiver(receiver);
	for (const auto viewport : { utils::Size2d{ 320, 180 }, utils::Size2d{ 184, 161 },
		utils::Size2d{ 10, 10 }, utils::Size2d{ 0, 0 }, utils::Size2d{ 800, 600 } }) {
		popup->FitToViewport(viewport);
		const auto size = popup->GetSize(); const auto position = popup->Position();
		EXPECT_GE(position.X, 0); EXPECT_GE(position.Y, 0);
		EXPECT_LE(position.X + static_cast<int>(size.Width), static_cast<int>(viewport.Width));
		EXPECT_LE(position.Y + static_cast<int>(size.Height), static_cast<int>(viewport.Height));
		for (unsigned char index = 0; index < 6; ++index) {
			const auto child = popup->TryGetChild(index); ASSERT_TRUE(child);
			const auto pos = child->Position(); const auto frame = child->GetSize();
			EXPECT_GE(pos.X, 0); EXPECT_GE(pos.Y, 0);
			EXPECT_LE(pos.X + static_cast<int>(frame.Width), static_cast<int>(size.Width));
			EXPECT_LE(pos.Y + static_cast<int>(frame.Height), static_cast<int>(size.Height));
		}
		if (viewport.Width >= 184 && viewport.Height >= 161) {
			const auto button = popup->TryGetChild(5); const auto pos = button->Position(); const auto frame = button->GetSize();
			const utils::Position2d point{ pos.X + static_cast<int>(frame.Width / 2), pos.Y + static_cast<int>(frame.Height / 2) };
			const auto before = receiver->ActionCount;
			EXPECT_TRUE(popup->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, point)).IsEaten);
			EXPECT_TRUE(popup->OnAction(MakeTouch(TouchAction::TOUCH_UP, point)).IsEaten);
			EXPECT_EQ(before + 1, receiver->ActionCount); EXPECT_EQ(1u, receiver->LastAction->Index);
		}
	}
	EXPECT_EQ(460u, popup->GetSize().Width); EXPECT_EQ(210u, popup->GetSize().Height);
}

// GuiToggle keyboard tests

TEST(GuiToggle, KeyboardActivatesWhenFocused) {
	GuiToggleParams p;
	p.Size = { 20, 20 };
	p.MinSize = { 20, 20 };
	auto toggle = std::make_shared<GuiToggle>(p);
	ASSERT_TRUE(toggle->RequestFocus());

	auto res = toggle->OnAction(MakeKey(13, KeyAction::KEY_UP));

	EXPECT_TRUE(res.IsEaten);
	EXPECT_EQ(actions::ACTIONRESULT_TOGGLE, res.ResultType);
}

TEST(GuiToggle, KeyboardIgnoredWithoutFocus) {
	GuiToggleParams p;
	p.Size = { 20, 20 };
	p.MinSize = { 20, 20 };
	auto toggle = std::make_shared<GuiToggle>(p);

	auto res = toggle->OnAction(MakeKey(13, KeyAction::KEY_UP));

	EXPECT_FALSE(res.IsEaten);
}

// GuiScrollBar range tests

TEST(GuiScrollBar, ThumbFractionClampsToOneWhenContentFits) {
	EXPECT_DOUBLE_EQ(1.0, GuiScrollBar::ThumbFraction(100, 100));
	EXPECT_DOUBLE_EQ(1.0, GuiScrollBar::ThumbFraction(120, 100));
	EXPECT_DOUBLE_EQ(0.5, GuiScrollBar::ThumbFraction(50, 100));
}

TEST(GuiScrollBar, ThumbLengthHonoursMinimum) {
	EXPECT_EQ(50u, GuiScrollBar::ThumbLength(100, 50, 100, 16));
	EXPECT_EQ(16u, GuiScrollBar::ThumbLength(200, 10, 1000, 16));
}

TEST(GuiScrollBar, OffsetAndValueRoundTrip) {
	EXPECT_EQ(0, GuiScrollBar::ThumbOffset(100, 50, 0.0));
	EXPECT_EQ(50, GuiScrollBar::ThumbOffset(100, 50, 1.0));

	const int offset = GuiScrollBar::ThumbOffset(100, 50, 0.5);
	EXPECT_DOUBLE_EQ(0.5, GuiScrollBar::ValueFromOffset(100, 50, offset));
}

TEST(GuiScrollBar, IsHiddenWhenContentFitsAndVisibleWhenItOverflows) {
	GuiScrollBarParams p;
	p.Size = { 18u, 100u };
	p.MinSize = p.Size;
	auto scrollBar = std::make_shared<GuiScrollBar>(p);

	scrollBar->SetMetrics(100.0, 100.0);
	EXPECT_FALSE(scrollBar->IsVisible());

	scrollBar->SetMetrics(100.0, 101.0);
	EXPECT_TRUE(scrollBar->IsVisible());
}

TEST(GuiScrollBar, ClearingPointerCancelsDrag) {
	GuiScrollBarParams p;
	p.Size = { 18u, 100u };
	auto scrollBar = std::make_shared<GuiScrollBar>(p);
	scrollBar->SetMetrics(50.0, 200.0);
	ASSERT_TRUE(scrollBar->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 9, 80 })).IsEaten);
	scrollBar->ClearPointerState();
	EXPECT_FALSE(scrollBar->OnAction(MakeTouchMove({ 9, 30 })).IsEaten);
	EXPECT_DOUBLE_EQ(0.0, scrollBar->Value());
}

TEST(GuiScrollBar, HorizontalDragMovesThumbToTheRight) {
	GuiScrollBarParams p;
	p.Orientation = GuiScrollOrientation::Horizontal;
	p.Size = { 100u, 18u };
	auto scrollBar = std::make_shared<GuiScrollBar>(p);
	scrollBar->SetMetrics(100.0, 200.0);
	ASSERT_TRUE(scrollBar->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 5, 9 })).IsEaten);
	EXPECT_TRUE(scrollBar->OnAction(MakeTouchMove({ 55, 9 })).IsEaten);
	EXPECT_DOUBLE_EQ(1.0, scrollBar->Value());
	EXPECT_TRUE(scrollBar->OnAction(MakeTouch(TouchAction::TOUCH_UP, { 55, 9 })).IsEaten);
}

// GuiScrollPanel offset tests

TEST(GuiScrollPanel, OffsetClampsToContentRange) {
	GuiScrollPanelParams p;
	p.Size = { 100, 50 };
	p.MinSize = { 100, 50 };
	p.ScrollBarWidth = 12u;
	auto panel = std::make_shared<GuiScrollPanel>(p);
	panel->SetContent(std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 80, 200 })));

	EXPECT_EQ(150, panel->MaxScrollOffset());

	panel->SetScrollOffset(1000);
	EXPECT_EQ(150, panel->ScrollOffset());

	panel->SetScrollOffset(-10);
	EXPECT_EQ(0, panel->ScrollOffset());
}

TEST(GuiScrollPanel, GlobalAndLocalCoordinatesIncludeNonzeroScrollOffset) {
	GuiScrollPanelParams p;
	p.Position = { 30, 40 };
	p.Size = { 100, 50 };
	p.MinSize = { 100, 50 };
	auto panel = std::make_shared<GuiScrollPanel>(p);

	base::GuiElementParams contentParams;
	contentParams.Size = { 80, 200 };
	contentParams.MinSize = contentParams.Size;
	auto content = std::make_shared<base::GuiElement>(contentParams);
	auto button = std::make_shared<GuiButton>(MakeSizedButton({ 5, 120 }, { 40, 20 }));
	content->AddChild(button);

	panel->SetContent(content);
	panel->SetScrollOffset(60);

	const auto globalPoint = button->GlobalPosition() + utils::Position2d{ 7, 8 };
	const auto localPoint = button->GlobalToLocal(globalPoint);

	EXPECT_EQ(7, localPoint.X);
	EXPECT_EQ(8, localPoint.Y);
	EXPECT_EQ(globalPoint.X, button->GlobalPosition().X + localPoint.X);
	EXPECT_EQ(globalPoint.Y, button->GlobalPosition().Y + localPoint.Y);
	ASSERT_NE(nullptr, content->Parent());
	EXPECT_EQ(panel.get(), content->Parent()->Parent().get());
}

TEST(GuiScrollPanel, CapturedButtonReceivesReleaseInScrolledLocalCoordinates) {
	GuiScrollPanelParams p;
	p.Position = { 30, 40 };
	p.Size = { 100, 50 };
	p.MinSize = { 100, 50 };
	auto panel = std::make_shared<GuiScrollPanel>(p);

	base::GuiElementParams contentParams;
	contentParams.Size = { 80, 200 };
	contentParams.MinSize = contentParams.Size;
	auto content = std::make_shared<base::GuiElement>(contentParams);
	auto button = std::make_shared<GuiButton>(MakeSizedButton({ 5, 120 }, { 40, 20 }));
	content->AddChild(button);
	panel->SetContent(content);
	panel->SetScrollOffset(60);

	const auto globalPoint = button->GlobalPosition() + utils::Position2d{ 7, 8 };
	const auto panelPoint = globalPoint - panel->GlobalPosition();
	auto down = panel->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, panelPoint));

	ASSERT_TRUE(down.IsEaten);
	auto active = down.ActiveElement.lock();
	ASSERT_EQ(button.get(), active.get());

	auto release = MakeTouch(TouchAction::TOUCH_UP, globalPoint);
	const auto localRelease = active->GlobalToLocal(release);
	EXPECT_EQ(7, localRelease.Position.X);
	EXPECT_EQ(8, localRelease.Position.Y);

	const auto up = active->OnAction(localRelease);
	EXPECT_TRUE(up.IsEaten);
	EXPECT_EQ(base::GuiElement::STATE_OVER, button->GetState());
}

TEST(GuiScrollPanel, ScrollFractionMapsToOffset) {
	GuiScrollPanelParams p;
	p.Size = { 100, 50 };
	p.MinSize = { 100, 50 };
	auto panel = std::make_shared<GuiScrollPanel>(p);
	panel->SetContent(std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 80, 200 })));

	panel->SetScrollFraction(0.5);
	EXPECT_EQ(75, panel->ScrollOffset());
}

TEST(GuiScrollPanel, ViewportExcludesScrollBar) {
	GuiScrollPanelParams p;
	p.Size = { 100, 50 };
	p.MinSize = { 100, 50 };
	p.ScrollBarWidth = 18u;
	auto panel = std::make_shared<GuiScrollPanel>(p);
	panel->SetContent(std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 80, 200 })));

	EXPECT_TRUE(panel->IsScrollBarVisible());
	EXPECT_EQ(82u, panel->ViewportWidth());
	EXPECT_EQ(50u, panel->ViewportHeight());
}

TEST(GuiScrollPanel, ClearingPointerRestoresContentMoveRouting) {
	GuiScrollPanelParams p;
	p.Size = { 100u, 50u };
	p.ScrollBarWidth = 12u;
	auto panel = std::make_shared<GuiScrollPanel>(p);
	panel->SetContent(std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 80, 200 })));
	ASSERT_TRUE(panel->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 94, 30 })).IsEaten);
	panel->ClearPointerState();
	EXPECT_FALSE(panel->OnAction(MakeTouchMove({ 94, 0 })).IsEaten);
	EXPECT_EQ(0, panel->ScrollOffset());
}

TEST(GuiScrollPanel, HidesScrollBarAndUsesFullWidthWhenContentFits) {
	GuiScrollPanelParams p;
	p.Size = { 100, 50 };
	p.MinSize = { 100, 50 };
	auto panel = std::make_shared<GuiScrollPanel>(p);
	panel->SetContent(std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 80, 50 })));

	EXPECT_FALSE(panel->IsScrollBarVisible());
	EXPECT_EQ(100u, panel->ViewportWidth());
}

TEST(GuiScrollPanel, ContentIsTopAlignedAtTheStartOfTheScrollRange) {
	GuiScrollPanelParams p;
	p.Size = { 100, 100 };
	p.MinSize = { 100, 100 };
	auto panel = std::make_shared<GuiScrollPanel>(p);
	auto content = std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 80, 30 }));
	panel->SetContent(content);

	EXPECT_EQ(70, content->GlobalPosition().Y);

	panel->SetSize({ 100, 20 });
	EXPECT_TRUE(panel->IsScrollBarVisible());
	EXPECT_EQ(-10, content->GlobalPosition().Y);

	panel->SetScrollOffset(10);
	EXPECT_EQ(0, content->GlobalPosition().Y);
}

TEST(GuiScrollPanel, HorizontalViewportAndOffsetFollowContentWidth) {
	GuiScrollPanelParams p;
	p.Orientation = GuiScrollOrientation::Horizontal;
	p.Size = { 100u, 60u };
	p.ScrollBarWidth = 18u;
	auto panel = std::make_shared<GuiScrollPanel>(p);
	auto content = std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 250u, 42u }));
	panel->SetContent(content);

	EXPECT_TRUE(panel->IsScrollBarVisible());
	EXPECT_EQ(100u, panel->ViewportWidth());
	EXPECT_EQ(42u, panel->ViewportHeight());
	EXPECT_EQ(150, panel->MaxScrollOffset());
	EXPECT_EQ(18, content->GlobalPosition().Y);

	panel->SetScrollOffset(1000);
	EXPECT_EQ(150, panel->ScrollOffset());
	EXPECT_EQ(-150, content->GlobalPosition().X);
	panel->SetScrollOffset(-1);
	EXPECT_EQ(0, panel->ScrollOffset());
}

TEST(GuiScrollPanel, HorizontalWheelAndBottomBarDragScrollContent) {
	GuiScrollPanelParams p;
	p.Orientation = GuiScrollOrientation::Horizontal;
	p.Size = { 100u, 60u };
	p.ScrollBarWidth = 18u;
	p.WheelStep = 24u;
	auto panel = std::make_shared<GuiScrollPanel>(p);
	panel->SetContent(std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 200u, 42u })));

	auto wheel = MakeTouch(TouchAction::TOUCH_DOWN, { 50, 40 });
	wheel.Index = 4;
	wheel.Value = -1;
	EXPECT_TRUE(panel->OnAction(wheel).IsEaten);
	EXPECT_EQ(24, panel->ScrollOffset());

	panel->SetScrollOffset(0);
	ASSERT_TRUE(panel->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 5, 9 })).IsEaten);
	EXPECT_TRUE(panel->OnAction(MakeTouchMove({ 55, 9 })).IsEaten);
	EXPECT_EQ(100, panel->ScrollOffset());
	EXPECT_TRUE(panel->OnAction(MakeTouch(TouchAction::TOUCH_UP, { 55, 9 })).IsEaten);
}

TEST(GuiScrollPanel, HorizontalBarHidesWhenContentFits) {
	GuiScrollPanelParams p;
	p.Orientation = GuiScrollOrientation::Horizontal;
	p.Size = { 100u, 60u };
	p.ScrollBarWidth = 18u;
	auto panel = std::make_shared<GuiScrollPanel>(p);
	panel->SetContent(std::make_shared<GuiButton>(MakeSizedButton({ 0, 0 }, { 80u, 60u })));

	EXPECT_FALSE(panel->IsScrollBarVisible());
	EXPECT_EQ(100u, panel->ViewportWidth());
	EXPECT_EQ(60u, panel->ViewportHeight());
	EXPECT_EQ(0, panel->MaxScrollOffset());
}

// GuiTextBox editing tests

TEST(GuiTextBox, VkToCharMapping) {
	EXPECT_EQ('a', GuiTextBox::VkToChar('A', false).value());
	EXPECT_EQ('A', GuiTextBox::VkToChar('A', true).value());
	EXPECT_EQ('5', GuiTextBox::VkToChar('5', false).value());
	EXPECT_EQ('%', GuiTextBox::VkToChar('5', true).value());
	EXPECT_EQ(' ', GuiTextBox::VkToChar(0x20, false).value());
	EXPECT_EQ('.', GuiTextBox::VkToChar(0xBE, false).value());
	EXPECT_FALSE(GuiTextBox::VkToChar(0x70, false).has_value()); // VK_F1
}

TEST(GuiTextBox, TypingInsertsCharacters) {
	GuiTextBoxParams tp;
	tp.Size = { 80, 24 };
	tp.MinSize = { 80, 24 };
	auto tb = std::make_shared<GuiTextBox>(tp);
	ASSERT_TRUE(tb->RequestFocus());

	TypeChars(tb, "ABC");

	EXPECT_EQ("abc", tb->Text());
	EXPECT_EQ(3u, tb->CaretIndex());
}

TEST(GuiTextBox, BackspaceAndDeleteEditText) {
	GuiTextBoxParams tp;
	tp.Text = "abc";
	tp.Size = { 80, 24 };
	tp.MinSize = { 80, 24 };
	auto tb = std::make_shared<GuiTextBox>(tp);
	ASSERT_TRUE(tb->RequestFocus());

	tb->OnAction(MakeKey(0x08)); // backspace at end -> "ab"
	EXPECT_EQ("ab", tb->Text());

	tb->OnAction(MakeKey(0x24)); // Home -> caret 0
	tb->OnAction(MakeKey(0x2E)); // delete -> "b"
	EXPECT_EQ("b", tb->Text());
}

TEST(GuiTextBox, ShiftSelectionThenBackspaceDeletesRange) {
	GuiTextBoxParams tp;
	tp.Text = "abcd";
	tp.Size = { 80, 24 };
	tp.MinSize = { 80, 24 };
	auto tb = std::make_shared<GuiTextBox>(tp);
	ASSERT_TRUE(tb->RequestFocus());

	// Caret starts at end (4). Shift+Home selects the whole string.
	tb->OnAction(MakeKey(0x24, KeyAction::KEY_DOWN, Action::MODIFIER_SHIFT));
	EXPECT_TRUE(tb->HasSelection());
	EXPECT_EQ(4u, tb->SelectionLength());

	tb->OnAction(MakeKey(0x08)); // backspace removes selection
	EXPECT_EQ("", tb->Text());
}

TEST(GuiTextBox, UnfocusedIgnoresKeys) {
	GuiTextBoxParams tp;
	tp.Size = { 80, 24 };
	tp.MinSize = { 80, 24 };
	auto tb = std::make_shared<GuiTextBox>(tp);

	auto res = tb->OnAction(MakeKey('A'));

	EXPECT_FALSE(res.IsEaten);
	EXPECT_EQ("", tb->Text());
}

TEST(GuiTextBox, LateBoundReceiverGetsNotifications) {
	GuiTextBoxParams tp;
	tp.Size = { 80, 24 };
	tp.MinSize = { 80, 24 };
	tp.Index = 17u;
	auto tb = std::make_shared<GuiTextBox>(tp);
	auto receiver = std::make_shared<GuiPhase3RecordingGuiReceiver>();
	tb->SetReceiver(receiver);

	tb->SetText("23", true);

	ASSERT_EQ(1, receiver->ActionCount);
	ASSERT_TRUE(receiver->LastAction.has_value());
	EXPECT_EQ(17u, receiver->LastAction->Index);
	EXPECT_EQ(GuiAction::ACTIONELEMENT_RACK, receiver->LastAction->ElementType);
	EXPECT_EQ("23", std::get<GuiAction::GuiString>(receiver->LastAction->Data).Value);
}

// GuiNumericInput tests

TEST(GuiNumericInput, SeedsFormattedInitialValue) {
	GuiNumericInputParams np;
	np.Min = 0.0; np.Max = 100.0; np.Step = 1.0;
	np.InitValue = 42.0; np.Decimals = 1;
	np.Size = { 100, 24 }; np.MinSize = { 100, 24 };
	auto ni = std::make_shared<GuiNumericInput>(np);

	EXPECT_DOUBLE_EQ(42.0, ni->Value());
	EXPECT_EQ("42.0", ni->Text());
}

TEST(GuiNumericInput, CommitParsesAndClamps) {
	GuiNumericInputParams np;
	np.Min = 0.0; np.Max = 100.0; np.Step = 1.0;
	np.InitValue = 50.0; np.Decimals = 0;
	np.Size = { 100, 24 }; np.MinSize = { 100, 24 };
	auto ni = std::make_shared<GuiNumericInput>(np);
	ASSERT_TRUE(ni->RequestFocus());

	// Text is "50"; append "9" -> "509", then Enter to commit (clamps to 100).
	ni->OnAction(MakeKey('9'));
	ni->OnAction(MakeKey(0x0D));

	EXPECT_DOUBLE_EQ(100.0, ni->Value());
	EXPECT_EQ("100", ni->Text());
}

TEST(GuiNumericInput, RejectsNonNumericCharacters) {
	GuiNumericInputParams np;
	np.Min = 0.0; np.Max = 100.0; np.Step = 1.0;
	np.InitValue = 1.0; np.Decimals = 0;
	np.Size = { 100, 24 }; np.MinSize = { 100, 24 };
	auto ni = std::make_shared<GuiNumericInput>(np);
	ASSERT_TRUE(ni->RequestFocus());

	ni->OnAction(MakeKey('A')); // letter rejected
	EXPECT_EQ("1", ni->Text());
}

TEST(GuiNumericInput, VerticalDragAdjustsValue) {
	GuiNumericInputParams np;
	np.Min = 0.0; np.Max = 100.0; np.Step = 1.0;
	np.InitValue = 50.0; np.Decimals = 0;
	np.Size = { 100, 24 }; np.MinSize = { 100, 24 };
	auto ni = std::make_shared<GuiNumericInput>(np);

	ni->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 10, 10 }));
	ni->OnAction(MakeTouchMove({ 10, 20 })); // dy = +10 -> +10 * step

	EXPECT_DOUBLE_EQ(60.0, ni->Value());
}

// GuiDropDown selection tests

TEST(GuiDropDown, ClickThenOpenSelectsItem) {
	GuiPopupManager host;
	GuiDropDownParams dp;
	dp.Items = { "Sine", "Square", "Saw" };
	dp.InitIndex = 0u;
	dp.RowHeight = 20u;
	dp.Size = { 120, 24 };
	dp.MinSize = { 120, 24 };
	auto dd = std::make_shared<GuiDropDown>(dp);
	dd->SetPopupManager(&host);

	EXPECT_EQ(0, dd->SelectedIndex());

	// Press + release opens the list popup.
	dd->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 10, 10 }));
	dd->OnAction(MakeTouch(TouchAction::TOUCH_UP, { 10, 10 }));
	ASSERT_TRUE(dd->IsOpen());
	ASSERT_TRUE(host.IsOpen());

	// Click the second row inside the popup (rows are RowHeight tall).
	auto top = host.Top();
	ASSERT_NE(nullptr, top);
	auto rowLocal = top->ParentToLocal(MakeTouch(TouchAction::TOUCH_UP, { top->Position().X + 5, top->Position().Y + 25 }));
	top->OnAction(rowLocal);

	EXPECT_EQ(1, dd->SelectedIndex());
	EXPECT_EQ("Square", dd->SelectedText());
	EXPECT_FALSE(dd->IsOpen());
}

TEST(GuiDropDown, SetSelectedIndexClamps) {
	GuiDropDownParams dp;
	dp.Items = { "A", "B", "C" };
	dp.Size = { 120, 24 };
	dp.MinSize = { 120, 24 };
	auto dd = std::make_shared<GuiDropDown>(dp);

	dd->SetSelectedIndex(99);
	EXPECT_EQ(2, dd->SelectedIndex());

	dd->SetSelectedIndex(-5);
	EXPECT_EQ(0, dd->SelectedIndex());
}

TEST(GuiNumericInput, FinalizeValidatesEntireFiniteTextThroughScrollContent) {
	GuiNumericInputParams params;
	params.Min = -1.0; params.Max = 1.0; params.InitValue = 0.25; params.Decimals = 3;
	params.Size = { 100, 40 };
	auto input = std::make_shared<GuiNumericInput>(params);
	auto scroll = std::make_shared<GuiScrollPanel>(GuiScrollPanelParams{});
	scroll->SetContent(input);
	for (const std::string invalid : { "nan", "inf", "0.5garbage", "-" }) {
		input->SetText(invalid, false);
		ASSERT_TRUE(input->RequestFocus());
		scroll->FinalizeEdits();
		EXPECT_EQ("0.250", input->Text());
		EXPECT_FALSE(input->HasFocus());
	}
	input->SetText("5", false);
	input->RequestFocus();
	scroll->FinalizeEdits();
	EXPECT_DOUBLE_EQ(1.0, input->Value());
	EXPECT_EQ("1.000", input->Text());
}

TEST(GuiNumericInput, CancelStopsDragAndHiddenInputCannotRestartIt) {
	GuiNumericInputParams params;
	params.Min = 0; params.Max = 100; params.InitValue = 50; params.Step = 1;
	params.Size = { 100, 40 };
	auto input = std::make_shared<GuiNumericInput>(params);
	input->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 10, 10 }));
	input->ClearPointerState();
	input->OnAction(MakeTouchMove({ 10, 30 }));
	EXPECT_DOUBLE_EQ(50, input->Value());
	input->SetVisible(false);
	input->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 10, 10 }));
	input->SetVisible(true);
	input->OnAction(MakeTouchMove({ 10, 30 }));
	EXPECT_DOUBLE_EQ(50, input->Value());
}

TEST(GuiTextBox, ReplacingReceiverSupersedesCachedReceiver) {
	auto oldOwner = std::make_shared<GuiPhase3RecordingGuiReceiver>();
	auto newOwner = std::make_shared<GuiPhase3RecordingGuiReceiver>();
	GuiTextBoxParams params;
	params.Receiver = oldOwner;
	auto input = std::make_shared<GuiTextBox>(params);
	input->SetText("first", true);
	const auto previousCount = oldOwner->ActionCount;
	input->SetReceiver(newOwner);
	input->SetText("second", true);
	EXPECT_EQ(previousCount, oldOwner->ActionCount);
	EXPECT_EQ(1, newOwner->ActionCount);
}

TEST(GuiMainPanel, RetainedPagesKeepCommandIdentityAfterTreeInitialization) {
	auto owner = std::make_shared<GuiPhase3RecordingGuiReceiver>();
	GuiNumericInputParams numericParams;
	numericParams.Size = { 96, 44 };
	auto numeric = std::make_shared<GuiNumericInput>(numericParams);
	gui::GuiRadioParams radioParams;
	radioParams.Size = { 228, 40 };
	radioParams.ToggleParams.resize(3);
	auto radio = std::make_shared<gui::GuiRadio>(radioParams);
	auto click = std::make_shared<GuiToggle>(GuiToggleParams::PanelPrimary());
	gui::GuiMainPanelParams params;
	params.Size = { 800, 600 };
	params.Settings = {
		{ gui::SettingsPage::Midi, "Quantisation", radio, 101u },
		{ gui::SettingsPage::Timing, "Offset", numeric, 7002u },
		{ gui::SettingsPage::Timing, "", click, 7003u }
	};
	auto panel = std::make_shared<gui::GuiMainPanel>(params);
	panel->Init();
	panel->SetCommandOwner(owner);
	panel->Init();
	EXPECT_EQ(gui::SettingsPage::Timing, panel->Page());
	numeric->SetValue(0.5, true);
	ASSERT_TRUE(owner->LastAction);
	EXPECT_EQ(7002u, owner->LastAction->Index);
	click->SetToggleState(GuiToggleParams::TOGGLE_ON, false);
	EXPECT_EQ(7003u, owner->LastAction->Index);
	panel->SetPage(gui::SettingsPage::Midi);
	radio->SetCurrentValue(2u, false);
	EXPECT_EQ(101u, owner->LastAction->Index);
	EXPECT_EQ(2, std::get<GuiAction::GuiInt>(owner->LastAction->Data).Value);
	panel->SetPage(gui::SettingsPage::Timing);
	EXPECT_EQ(numeric, numeric->Parent()->TryGetChild(1));
	owner.reset();
	EXPECT_NO_THROW(numeric->SetValue(0.6, true));
}

TEST(GuiMainPanel, SwitchAndCollapseFinalizeEditsAndOnlyCloseOwnedPopup) {
	GuiNumericInputParams numericParams;
	numericParams.Min = -1; numericParams.Max = 1; numericParams.InitValue = 0;
	numericParams.Size = { 96, 44 };
	auto numeric = std::make_shared<GuiNumericInput>(numericParams);
	GuiPopupManager popups;
	int hideCalls = 0;
	gui::GuiMainPanelParams params;
	params.Size = { 800, 600 }; params.PopupManager = &popups;
	params.BeforeHide = [&hideCalls](const auto&) { ++hideCalls; };
	params.Settings = { { gui::SettingsPage::Timing, "Offset", numeric, 7002u } };
	auto panel = std::make_shared<gui::GuiMainPanel>(params);
	panel->Init();
	auto unrelated = std::make_shared<base::GuiElement>(base::GuiElementParams{});
	auto owned = std::make_shared<base::GuiElement>(base::GuiElementParams{});
	popups.Open(unrelated);
	popups.Open(owned, numeric);
	numeric->SetText("9", false); numeric->RequestFocus();
	panel->SetPage(gui::SettingsPage::Midi);
	EXPECT_DOUBLE_EQ(1, numeric->Value());
	EXPECT_FALSE(numeric->HasFocus());
	EXPECT_EQ(unrelated, popups.Top());
	EXPECT_EQ(1, hideCalls);
	panel->SetExpanded(false);
	EXPECT_EQ(2, hideCalls);
	EXPECT_EQ(unrelated, popups.Top());
	EXPECT_TRUE(panel->RouteHitTest({ 25, 10 }));
	EXPECT_TRUE(panel->RouteHitTest({ 25, 100 })); // Sliding body still blocks the scene.
	for (int frame = 0; frame < 5; ++frame) panel->AdvanceAnimation(0.05f);
	EXPECT_FALSE(panel->RouteHitTest({ 25, 100 }));
}

TEST(GuiMainPanel, ControlsRecoverTheirWidthsAfterTinyAndZeroViewport) {
	GuiNumericInputParams numericParams;
	numericParams.Size = { 96, 44 };
	auto numeric = std::make_shared<GuiNumericInput>(numericParams);
	gui::GuiMainPanelParams params;
	params.Size = { 800, 600 };
	params.Settings = { { gui::SettingsPage::Timing, "Offset", numeric, 7002u } };
	auto panel = std::make_shared<gui::GuiMainPanel>(params);
	panel->Init();
	panel->SetViewportSize({ 40, 40 });
	panel->SetViewportSize({ 0, 0 });
	EXPECT_FALSE(panel->RouteHitTest({ 0, 0 }));
	panel->SetViewportSize({ 800, 600 });
	EXPECT_EQ(96u, numeric->GetSize().Width);
}

TEST(GuiFocusManager, MovingOrClearingFocusFinalizesNumericEdits) {
	GuiNumericInputParams params;
	params.Min = 0; params.Max = 16; params.InitValue = 3; params.Decimals = 0;
	auto input = std::make_shared<GuiNumericInput>(params);
	auto other = std::make_shared<GuiNumericInput>(params);
	GuiFocusManager focus;
	focus.RequestFocus(input);
	input->SetText("50", false);
	focus.RequestFocus(other);
	EXPECT_DOUBLE_EQ(16, input->Value());
	EXPECT_EQ("16", input->Text());
	other->SetText("junk", false);
	focus.ClearFocus();
	EXPECT_EQ("3", other->Text());
	EXPECT_EQ(nullptr, focus.CurrentFocus());
}

TEST(GuiNumericInput, OwnerValueSynchronizationPreservesPendingEditUntilFinalization) {
	GuiNumericInputParams params;
	params.Min = -1; params.Max = 1; params.InitValue = 0; params.Decimals = 3;
	auto input = std::make_shared<GuiNumericInput>(params);
	input->RequestFocus();
	input->SetText("-0.", false);
	input->SynchronizeValueFromOwner(0.25);
	EXPECT_EQ("-0.", input->Text());
	EXPECT_DOUBLE_EQ(0.25, input->Value());
	input->SetText("-", false);
	input->FinalizeEdits();
	EXPECT_EQ("0.250", input->Text());
	input->SynchronizeValueFromOwner(0.5);
	EXPECT_EQ("0.500", input->Text());
}

TEST(GuiMainPanel, HandlesAndTabsUseStableInternalCommands) {
	gui::GuiMainPanelParams params;
	params.Size = { 800, 600 };
	auto panel = std::make_shared<gui::GuiMainPanel>(params);
	panel->Init();
	auto frame = panel->TryGetChild(0);
	auto tabs = std::dynamic_pointer_cast<GuiScrollPanel>(frame->TryGetChild(2));
	ASSERT_NE(nullptr, tabs);
	auto radio = std::dynamic_pointer_cast<gui::GuiRadio>(tabs->Content());
	ASSERT_NE(nullptr, radio);
	radio->SetCurrentValue(0u, false);
	EXPECT_EQ(gui::SettingsPage::Midi, panel->Page());
	auto handle = std::dynamic_pointer_cast<GuiToggle>(panel->TryGetChild(1));
	ASSERT_NE(nullptr, handle);
	handle->SetToggleState(GuiToggleParams::TOGGLE_OFF, false);
	EXPECT_FALSE(panel->IsExpanded());
	handle->SetToggleState(GuiToggleParams::TOGGLE_ON, false);
	EXPECT_TRUE(panel->IsExpanded());
	EXPECT_EQ(gui::SettingsPage::Midi, panel->Page());
}

TEST(GuiMainPanel, MotionReversalStartsAtCurrentValueAndIdleDoesNoWork) {
	gui::GuiMainPanelParams params;
	params.Size = { 800, 600 };
	auto panel = std::make_shared<gui::GuiMainPanel>(params);
	panel->Init();
	EXPECT_FALSE(panel->AdvanceAnimation(0.016f));
	panel->SetExpanded(false);
	EXPECT_FLOAT_EQ(1.0f, panel->TransitionValue());
	EXPECT_TRUE(panel->AdvanceAnimation(0.05f));
	const auto value = panel->TransitionValue();
	const auto position = panel->TryGetChild(0)->Position();
	const auto opacity = panel->PresentedOpacity();
	EXPECT_GT(value, 0.0f); EXPECT_LT(value, 1.0f);
	EXPECT_GT(opacity, 0.0f); EXPECT_LT(opacity, 1.0f);
	panel->SetExpanded(true);
	EXPECT_FLOAT_EQ(value, panel->TransitionValue());
	EXPECT_EQ(position.X, panel->TryGetChild(0)->Position().X);
	EXPECT_FLOAT_EQ(opacity, panel->PresentedOpacity());
	EXPECT_TRUE(panel->AdvanceAnimation(0.05f));
	EXPECT_FLOAT_EQ(1.0f, panel->TransitionValue());
	EXPECT_FALSE(panel->AdvanceAnimation(0.05f));
}

TEST(GuiMainPanel, MotionClampsResumeIntervalAndResizePreservesProgress) {
	gui::GuiMainPanelParams params;
	params.Size = { 800, 600 }; params.SelectionOnly = true;
	auto panel = std::make_shared<gui::GuiMainPanel>(params);
	panel->Init(); panel->SetExpanded(false);
	panel->AdvanceAnimation(100.0f);
	const auto value = panel->TransitionValue();
	EXPECT_NEAR(1.0f - 0.05f / gui::GuiStyle::PanelTransitionSeconds, value, 0.0001f);
	const auto oldY = panel->TryGetChild(0)->Position().Y;
	panel->SetViewportSize({ 800, 900 });
	EXPECT_FLOAT_EQ(value, panel->TransitionValue());
	EXPECT_EQ(oldY + 300, panel->TryGetChild(0)->Position().Y);
	EXPECT_TRUE(panel->RouteHitTest({ 25, 890 }));
	panel->SetViewportSize({ 10, 10 });
	EXPECT_FLOAT_EQ(value, panel->TransitionValue());
	const auto handle = panel->TryGetChild(1);
	EXPECT_GE(handle->Position().Y, 0);
	EXPECT_LE(handle->Position().Y + static_cast<int>(handle->GetSize().Height), 10);
	panel->SetViewportSize({ 0, 0 });
	EXPECT_FALSE(panel->RouteHitTest({ 0, 0 }));
	EXPECT_FALSE(panel->RouteHitTest({ -1, -1 }));
}

TEST(GuiMainPanel, ClosingImmediatelyGatesControlsWhileRetainingBodyBlocker) {
	GuiNumericInputParams numericParams;
	numericParams.Size = { 96, 44 }; numericParams.Min = -1; numericParams.Max = 1;
	auto input = std::make_shared<GuiNumericInput>(numericParams);
	gui::GuiMainPanelParams params;
	params.Size = { 800, 600 };
	params.Settings = { { gui::SettingsPage::Timing, "Offset", input, 7002u } };
	auto panel = std::make_shared<gui::GuiMainPanel>(params);
	panel->Init();
	const auto position = input->GlobalPosition() + utils::Position2d{ 10, 10 };
	ASSERT_EQ(input, panel->FindTopmostDescendant(position));
	panel->SetExpanded(false);
	EXPECT_TRUE(panel->RouteHitTest(position));
	EXPECT_NE(input, panel->FindTopmostDescendant(position));
	EXPECT_TRUE(panel->OnAction(MakeTouch(TouchAction::TOUCH_DOWN, position)).IsEaten);
	panel->OnAction(MakeTouchMove(position + utils::Position2d{ 0, 10 }));
	EXPECT_DOUBLE_EQ(0, input->Value());
}

TEST(GuiOpacity, NestedScopesMultiplyAndRestoreOnEarlyExit) {
	base::DrawContext context({ 100, 100 }, base::DrawContext::SCREEN);
	EXPECT_FLOAT_EQ(1.0f, context.Opacity());
	{
		auto parent = context.WithOpacity(0.5f);
		EXPECT_FLOAT_EQ(0.5f, context.Opacity());
		{
			auto child = context.WithOpacity(0.4f);
			EXPECT_FLOAT_EQ(0.2f, context.Opacity());
		}
		EXPECT_FLOAT_EQ(0.5f, context.Opacity());
		try {
			auto child = context.WithOpacity(0.25f);
			throw 1;
		} catch (int) {}
		EXPECT_FLOAT_EQ(0.5f, context.Opacity());
	}
	EXPECT_FLOAT_EQ(1.0f, context.Opacity());
	{
		auto clamped = context.WithOpacity(100.0f);
		EXPECT_FLOAT_EQ(1.0f, context.Opacity());
	}
	{
		auto clamped = context.WithOpacity(-1.0f);
		EXPECT_FLOAT_EQ(0.0f, context.Opacity());
	}
	EXPECT_FLOAT_EQ(1.0f, context.Opacity());
}

TEST(GuiOpacity, GlUniformAlwaysHasEffectiveValueIncludingOrdinaryDraws) {
	graphics::GlDrawContext context({ 100, 100 }, base::DrawContext::SCREEN);
	ASSERT_TRUE(context.GetUniform("Opacity"));
	EXPECT_FLOAT_EQ(1.0f, std::any_cast<float>(*context.GetUniform("Opacity")));
	{
		auto scope = context.WithOpacity(0.25f);
		EXPECT_FLOAT_EQ(0.25f, std::any_cast<float>(*context.GetUniform("Opacity")));
	}
	EXPECT_FLOAT_EQ(1.0f, std::any_cast<float>(*context.GetUniform("Opacity")));
}

TEST(GuiTextGeometry, FontMetricsPlaceBaselineAndDescendersInsideStableFrame) {
	const graphics::Font::VerticalMetrics metrics{ 13.0f, -4.0f, 2.0f };
	EXPECT_FLOAT_EQ(17.0f, metrics.Height());
	EXPECT_FLOAT_EQ(19.0f, metrics.LineStep());
	const auto centered = gui::GuiLabel::ResolveLineFrame(24.0f, metrics, gui::GuiTextVerticalAlign::Center);
	EXPECT_FLOAT_EQ(7.5f, centered.BaselineY);
	EXPECT_FLOAT_EQ(3.5f, centered.Bottom);
	EXPECT_FLOAT_EQ(20.5f, centered.Top);
	EXPECT_FLOAT_EQ(12.0f, (centered.Bottom + centered.Top) * 0.5f);
	const auto top = gui::GuiLabel::ResolveLineFrame(24.0f, metrics, gui::GuiTextVerticalAlign::Top);
	EXPECT_FLOAT_EQ(24.0f, top.Top);
	EXPECT_FLOAT_EQ(7.0f, top.Bottom);
	const auto bottom = gui::GuiLabel::ResolveLineFrame(24.0f, metrics, gui::GuiTextVerticalAlign::Bottom);
	EXPECT_FLOAT_EQ(0.0f, bottom.Bottom);
	EXPECT_FLOAT_EQ(17.0f, bottom.Top);
	const auto baseline = gui::GuiLabel::ResolveLineFrame(24.0f, metrics, gui::GuiTextVerticalAlign::Baseline);
	EXPECT_FLOAT_EQ(0.0f, baseline.BaselineY);
	EXPECT_FLOAT_EQ(-4.0f, baseline.Bottom);
}

TEST(GuiTextGeometry, CompactRowsReserveAvailableFontHeightAndZeroFramesStayEmpty) {
	const auto row = gui::GuiLabelParams::ResolveTextFrame(100u, 22u, 10u, 10u, true);
	EXPECT_EQ(3u, row.PaddingY);
	EXPECT_EQ(16u, row.TextHeight);
	EXPECT_EQ(3, row.OffsetY);
	const auto odd = gui::GuiLabelParams::ResolveTextFrame(101u, 37u, 8u, 8u, true);
	EXPECT_EQ(21u, odd.TextHeight);
	EXPECT_EQ(8, odd.OffsetY);
	const auto empty = gui::GuiLabelParams::ResolveTextFrame(0u, 0u, 10u, 10u, true);
	EXPECT_EQ(0u, empty.ContentWidth);
	EXPECT_EQ(0u, empty.TextHeight);
	EXPECT_EQ(0, empty.OffsetY);
	const auto tiny = gui::GuiLabelParams::ResolveTextFrame(3u, 1u, 10u, 10u, true);
	EXPECT_EQ(1u, tiny.PaddingX);
	EXPECT_EQ(1u, tiny.ContentWidth);
	EXPECT_EQ(1u, tiny.TextHeight);
	EXPECT_EQ(0u, tiny.PaddingY);
}

TEST(GuiTextGeometry, CaretAndSelectionBandsUseFontLineAndClampedLabelFrame) {
	const utils::Rect2d frame{ 8, 8, 108, 28 };
	const auto line = gui::GuiLabel::ResolveLineFrame(20.0f, { 13.0f, -4.0f, 0.0f }, gui::GuiTextVerticalAlign::Center);
	const auto caret = GuiTextBox::ResolveTextBand(frame, line, 12.0f, 14.0f);
	EXPECT_EQ(20, caret.Left); EXPECT_EQ(22, caret.Right);
	EXPECT_EQ(9, caret.Bottom); EXPECT_EQ(27, caret.Top);
	const auto selection = GuiTextBox::ResolveTextBand(frame, line, 5.0f, 20.0f);
	EXPECT_EQ(caret.Bottom, selection.Bottom);
	EXPECT_EQ(caret.Top, selection.Top);
	const auto clipped = GuiTextBox::ResolveTextBand(frame, line, -500.0f, 500.0f);
	EXPECT_EQ(frame.Left, clipped.Left); EXPECT_EQ(frame.Right, clipped.Right);
	const auto tall = gui::GuiLabel::ResolveLineFrame(1.0f, { 13.0f, -4.0f, 0.0f }, gui::GuiTextVerticalAlign::Center);
	const auto tiny = GuiTextBox::ResolveTextBand({ 1, 0, 2, 1 }, tall, 0.0f, 2.0f);
	EXPECT_EQ(1, tiny.Left); EXPECT_EQ(2, tiny.Right);
	EXPECT_EQ(0, tiny.Bottom); EXPECT_EQ(1, tiny.Top);
	EXPECT_TRUE(GuiTextBox::ResolveTextBand({}, line, 0.0f, 2.0f).IsEmpty());
	EXPECT_TRUE(GuiTextBox::ResolveTextBand(frame, line, std::numeric_limits<float>::infinity(), 2.0f).IsEmpty());
}

TEST(GuiTextGeometry, RelatedControlsSharePreferredHeightAndHeadersKeepExplicitAlignment) {
	EXPECT_EQ(gui::GuiStyle::ControlHeight, gui::GuiButtonParams::PanelButton().Size.Height);
	EXPECT_EQ(gui::GuiStyle::ControlHeight, GuiToggleParams::PanelPrimary().Size.Height);
	EXPECT_EQ(gui::GuiStyle::ControlHeight, GuiTextBoxParams::PanelInput(100u).Size.Height);
	EXPECT_EQ(gui::GuiStyle::ControlHeight, GuiNumericInputParams::PanelInput(100u).Size.Height);
	EXPECT_EQ(gui::GuiStyle::ControlHeight, GuiDropDownParams::PanelInput(100u).Size.Height);
	EXPECT_EQ(gui::GuiTextVerticalAlign::Center, gui::GuiLabelParams::PanelHeader("Agjpq 0123", 100u).VerticalAlign);
	EXPECT_EQ(gui::GuiTextVerticalAlign::Center, gui::GuiLabelParams::PanelScrollRow("Agjpq 0123").VerticalAlign);
	EXPECT_EQ(gui::GuiTextVerticalAlign::Baseline, gui::GuiLabelParams{}.VerticalAlign);
}

TEST(GuiTextGeometry, ButtonAndRadioLabelsRetainCenteredContentFramesAcrossResize) {
	auto params = gui::GuiButtonParams::PanelButton();
	params.Text = "Agjpq 0123";
	auto button = std::make_shared<gui::GuiButton>(params);
	button->Init();
	auto label = std::dynamic_pointer_cast<gui::GuiLabel>(button->TryGetChild(0));
	ASSERT_NE(nullptr, label);
	for (const auto size : { utils::Size2d{ 100, 36 }, utils::Size2d{ 3, 1 }, utils::Size2d{ 0, 0 }, utils::Size2d{ 101, 37 } }) {
		button->SetSize(size);
		const auto frame = gui::GuiLabelParams::ResolveTextFrame(size.Width, size.Height, params.TextPadding, params.TextPadding, true);
		EXPECT_EQ(static_cast<int>(frame.PaddingX), label->Position().X);
		EXPECT_EQ(frame.OffsetY, label->Position().Y);
		EXPECT_EQ(frame.ContentWidth, label->GetSize().Width);
		EXPECT_EQ(frame.TextHeight, label->GetSize().Height);
	}
	gui::GuiRadioParams radioParams;
	auto toggle = GuiToggleParams::PanelPrimary(); toggle.Text = "Mixed";
	radioParams.ToggleParams = { toggle };
	auto radio = std::make_shared<gui::GuiRadio>(radioParams);
	radio->Init();
	auto toggleLabel = std::dynamic_pointer_cast<gui::GuiLabel>(radio->TryGetChild(0)->TryGetChild(0));
	ASSERT_NE(nullptr, toggleLabel);
	EXPECT_EQ(label->GetSize().Height - 1u, toggleLabel->GetSize().Height);
}

TEST(GuiTextGeometry, LegacyWidthTableFontHasExplicitFallbackLineMetrics) {
	graphics::Font font({ 1, 1, 1, 19.0f, 0, 0, graphics::FontOptions::FONT_LARGE },
		std::vector<float>(graphics::Font::MaxChars, 8.0f), {});
	EXPECT_FLOAT_EQ(19.0f, font.Metrics().Ascent);
	EXPECT_FLOAT_EQ(0.0f, font.Metrics().Descent);
	EXPECT_FLOAT_EQ(19.0f, font.Metrics().Height());
}

class GuiSettingsOwnerScene final : public engine::Scene
{
public:
	GuiSettingsOwnerScene() : Scene(engine::SceneParams({ "" }, {}, { 800u, 600u }), io::UserConfig{}) {}
	std::shared_ptr<gui::GuiMainPanel> Settings() const { return _mainPanel; }
	std::shared_ptr<gui::GuiMainPanel> Selection() const { return _selectionPanel; }
	std::shared_ptr<gui::GuiNumericInput> Channel() const { return _midiChannelOverrideInput; }
	std::shared_ptr<gui::GuiNumericInput> Phase() const { return _transportOffsetInput; }
	std::shared_ptr<gui::GuiToggle> Click() const { return _ninjamMetronomeToggle; }
	std::shared_ptr<gui::GuiRadio> Quantisation() const { return _globalMidiQuantRadio; }
	std::shared_ptr<gui::GuiRadio> Depth() const { return _modeRadio; }
	unsigned int ForcedChannel() const { return _inputSubsystem->ForcedChannelOverride(); }
	double PhaseFraction() const { return _transportOffsetLoopFrac; }
	bool ClickEnabled() const { return _audioEngine->NinjamMetronomeEnabled(); }
	io::JamFile::GlobalMidiQuantState QuantisationState() const { return _globalMidiQuantState; }
	unsigned int SelectionDepth() const { return static_cast<unsigned int>(_viewMode); }
	std::shared_ptr<engine::Station> AddStation()
	{
		engine::StationParams params; params.Name = "Settings owner station"; params.Size = { 100, 100 };
		audio::MergeMixBehaviourParams merge;
		auto station = std::make_shared<engine::Station>(params, engine::Station::GetMixerParams(params.Size, merge));
		_AddStation(station); return station;
	}
};

class GuiSceneSettingsTests : public ::testing::Test
{
protected:
	void SetUp() override
	{
		Scene = std::make_shared<GuiSettingsOwnerScene>(); Scene->InitReceivers();
		Station = Scene->AddStation(); ASSERT_TRUE(Station);
		// Production receivers must survive subsequent Init/tree renumbering.
		Scene->Settings()->AddChild(std::make_shared<base::GuiElement>(base::GuiElementParams{}));
		Scene->Selection()->Init();
	}
	void TearDown() override { Scene->Shutdown(); }
	std::shared_ptr<GuiSettingsOwnerScene> Scene;
	std::shared_ptr<engine::Station> Station;
};

TEST_F(GuiSceneSettingsTests, ChannelLimitsAndShortcutFeedbackReachRouterAfterTreeChanges)
{
	Scene->Settings()->SetPage(gui::SettingsPage::Midi);
	for (const auto value : { -1.0, 0.0, 1.0, 16.0, 17.0 }) {
		Scene->Channel()->SetValue(value, true);
		const auto expected = static_cast<unsigned int>(std::clamp(value, 0.0, 16.0));
		EXPECT_EQ(expected, Scene->ForcedChannel()); EXPECT_DOUBLE_EQ(expected, Scene->Channel()->Value());
	}
	Scene->Channel()->SetValue(0.0, true);
	KeyAction key; key.KeyChar = 0x21u; key.KeyActionType = KeyAction::KEY_DOWN;
	EXPECT_TRUE(Scene->OnAction(key).IsEaten);
	EXPECT_EQ(1u, Scene->ForcedChannel()); EXPECT_DOUBLE_EQ(1.0, Scene->Channel()->Value());
	key.KeyActionType = KeyAction::KEY_UP; EXPECT_TRUE(Scene->OnAction(key).IsEaten);
	key.KeyChar = 0x22u; key.KeyActionType = KeyAction::KEY_DOWN; EXPECT_TRUE(Scene->OnAction(key).IsEaten);
	EXPECT_EQ(0u, Scene->ForcedChannel()); EXPECT_DOUBLE_EQ(0.0, Scene->Channel()->Value());
	key.KeyActionType = KeyAction::KEY_UP; Scene->OnAction(key);
}

TEST_F(GuiSceneSettingsTests, PhaseOffsetUsesLoopFractionsAndPropagatesToStation)
{
	for (const auto value : { -2.0, -1.0, 0.5, 1.0, 2.0 }) {
		Scene->Phase()->SetValue(value, true);
		const auto expected = std::clamp(value, -1.0, 1.0);
		EXPECT_DOUBLE_EQ(expected, Scene->PhaseFraction());
		EXPECT_DOUBLE_EQ(expected, Station->TransportOffsetLoopFrac());
		EXPECT_DOUBLE_EQ(expected, Scene->Phase()->Value());
	}
	Scene->Phase()->SetValue(std::numeric_limits<double>::quiet_NaN(), true);
	EXPECT_DOUBLE_EQ(1.0, Scene->PhaseFraction()); EXPECT_DOUBLE_EQ(1.0, Scene->Phase()->Value());
	for (const auto text : { "nan", "0.5junk" }) {
		Scene->Phase()->SetText(text, true);
		EXPECT_DOUBLE_EQ(1.0, Scene->PhaseFraction()); EXPECT_DOUBLE_EQ(1.0, Scene->Phase()->Value());
	}
	const auto laterStation = Scene->AddStation();
	EXPECT_DOUBLE_EQ(1.0, laterStation->TransportOffsetLoopFrac());
}

TEST_F(GuiSceneSettingsTests, ClickToggleReachesExistingAudioHostOwner)
{
	Scene->Click()->SetToggleState(GuiToggleParams::TOGGLE_ON, false);
	EXPECT_TRUE(Scene->ClickEnabled());
	Scene->Click()->SetToggleState(GuiToggleParams::TOGGLE_OFF, false);
	EXPECT_FALSE(Scene->ClickEnabled());
	Scene->Click()->SetToggleState(GuiToggleParams::TOGGLE_ON, false);
	EXPECT_TRUE(Scene->ClickEnabled());
}

TEST_F(GuiSceneSettingsTests, GlobalQuantisationPreservesLocalGridsAndLocalEditUpdatesRadio)
{
	const auto first = Station->AddTake(), second = Station->AddTake();
	ASSERT_TRUE(first); ASSERT_TRUE(second);
	const auto configure = [](const auto& take, bool enabled, midi::MidiQuantisationFraction fraction) {
		auto settings = take->MidiQuantisation(); settings.Enabled = enabled; settings.Fraction = fraction;
		take->SetMidiQuantisation(settings);
	};
	configure(first, true, midi::MidiQuantisationFraction::Quarter);
	configure(second, false, midi::MidiQuantisationFraction::Eighth);
	const auto firstGrid = first->MidiQuantisation(), secondGrid = second->MidiQuantisation();
	for (const auto state : { io::JamFile::GlobalMidiQuantState::All, io::JamFile::GlobalMidiQuantState::Mixed,
		io::JamFile::GlobalMidiQuantState::Off }) {
		Scene->Quantisation()->SetCurrentValue(static_cast<unsigned int>(state), false);
		EXPECT_EQ(state, Scene->QuantisationState());
		EXPECT_EQ(firstGrid, first->MidiQuantisation()); EXPECT_EQ(secondGrid, second->MidiQuantisation());
		EXPECT_EQ(state != io::JamFile::GlobalMidiQuantState::Off, first->ResolvedMidiQuantisation().Enabled);
		EXPECT_EQ(state == io::JamFile::GlobalMidiQuantState::All, second->ResolvedMidiQuantisation().Enabled);
	}
	Scene->Quantisation()->SetCurrentValue(static_cast<unsigned int>(io::JamFile::GlobalMidiQuantState::All), false);
	first->SetMidiQuantisationFromUserEdit(firstGrid);
	EXPECT_EQ(io::JamFile::GlobalMidiQuantState::Mixed, Scene->QuantisationState());
	EXPECT_EQ(static_cast<unsigned int>(io::JamFile::GlobalMidiQuantState::Mixed), Scene->Quantisation()->CurrentValue());
}

TEST_F(GuiSceneSettingsTests, SelectionDepthReachesSceneFromTopPanel)
{
	for (const auto depth : { engine::Scene::VIEW_LOOP, engine::Scene::VIEW_LOOPTAKE, engine::Scene::VIEW_STATION }) {
		Scene->Depth()->SetCurrentValue(static_cast<unsigned int>(depth), false);
		EXPECT_EQ(static_cast<unsigned int>(depth), Scene->SelectionDepth());
	}
}
