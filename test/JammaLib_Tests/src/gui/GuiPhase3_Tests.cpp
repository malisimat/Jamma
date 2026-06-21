#include "gtest/gtest.h"
#include "gui/GuiButton.h"
#include "gui/GuiToggle.h"
#include "gui/GuiFocusManager.h"
#include "gui/GuiPopupHost.h"
#include "gui/GuiTextBox.h"
#include "gui/GuiNumericInput.h"
#include "gui/GuiDropDown.h"
#include "gui/GuiScrollBar.h"
#include "gui/GuiScrollPanel.h"
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
using gui::GuiPopupHost;
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
using actions::KeyAction;
using actions::TouchAction;
using actions::TouchMoveAction;

namespace
{
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
}

// ---------------------------------------------------------------------------
// GuiFocusManager
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// GuiPopupHost
// ---------------------------------------------------------------------------

TEST(GuiPopupHost, OpenAndCloseTrackTopmost) {
	GuiPopupHost host;
	auto popup = std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 }));

	EXPECT_FALSE(host.IsOpen());
	host.Open(popup);
	EXPECT_TRUE(host.IsOpen());
	EXPECT_EQ(popup, host.Top());

	host.Close();
	EXPECT_FALSE(host.IsOpen());
}

TEST(GuiPopupHost, OutsidePressDismissesAndConsumes) {
	GuiPopupHost host;
	auto popup = std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 }));
	host.Open(popup);

	auto res = host.OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 0, 0 }));

	EXPECT_TRUE(res.IsEaten);
	EXPECT_FALSE(host.IsOpen());
}

TEST(GuiPopupHost, InsidePressRoutesToPopup) {
	GuiPopupHost host;
	auto popup = std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 }));
	host.Open(popup);

	auto res = host.OnAction(MakeTouch(TouchAction::TOUCH_DOWN, { 110, 110 }));

	EXPECT_TRUE(res.IsEaten);
	EXPECT_TRUE(host.IsOpen());
}

TEST(GuiPopupHost, EscapeDismissesTopmost) {
	GuiPopupHost host;
	auto popup = std::make_shared<GuiButton>(MakeSizedButton({ 100, 100 }, { 50, 50 }));
	host.Open(popup);

	auto res = host.OnAction(MakeKey(27, KeyAction::KEY_UP));

	EXPECT_TRUE(res.IsEaten);
	EXPECT_FALSE(host.IsOpen());
}

// ---------------------------------------------------------------------------
// GuiToggle keyboard support
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// GuiScrollBar range math (static helpers)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// GuiScrollPanel offset behaviour
// ---------------------------------------------------------------------------

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
	p.ScrollBarWidth = 12u;
	auto panel = std::make_shared<GuiScrollPanel>(p);

	EXPECT_EQ(88u, panel->ViewportWidth());
	EXPECT_EQ(50u, panel->ViewportHeight());
}

// ---------------------------------------------------------------------------
// GuiTextBox editing
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// GuiNumericInput
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// GuiDropDown selection
// ---------------------------------------------------------------------------

TEST(GuiDropDown, ClickThenOpenSelectsItem) {
	GuiPopupHost host;
	GuiDropDownParams dp;
	dp.Items = { "Sine", "Square", "Saw" };
	dp.InitIndex = 0u;
	dp.RowHeight = 20u;
	dp.Size = { 120, 24 };
	dp.MinSize = { 120, 24 };
	auto dd = std::make_shared<GuiDropDown>(dp);
	dd->SetPopupHost(&host);

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
