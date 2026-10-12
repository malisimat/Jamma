#include "gtest/gtest.h"
#include "gui/GuiPerformanceGroupRow.h"
#include "gui/GuiMainPanel.h"
#include "gui/GuiHud.h"

class PerformanceGroupGuiTestHelpers
{
public:
    static actions::TouchAction Touch(actions::TouchAction::TouchState state, utils::Position2d position,
        base::Action::Modifiers modifiers = base::Action::MODIFIER_NONE)
    {
        actions::TouchAction action;
        action.State = state;
        action.Position = position;
        action.Modifiers = modifiers;
        action.Index = 0;
        action.Touch = actions::TouchAction::TOUCH_MOUSE;
        return action;
    }

    static std::shared_ptr<gui::GuiPerformanceGroupRow> Row()
    {
        auto row = std::make_shared<gui::GuiPerformanceGroupRow>(false);
        row->Init();
        row->SetSize({ 480u, gui::GuiPerformanceGroupRow::Height });
        row->AdvanceAnimation(0.12f);
        return row;
    }
};

TEST(GuiPerformanceGroups, SparseGeometryKeepsNumbersAndFirstCaptureDividesEqually)
{
    auto row = PerformanceGroupGuiTestHelpers::Row();
    EXPECT_FALSE(row->Button(1)->IsVisible());
    EXPECT_TRUE(row->Button(0)->IsVisible());
    std::array<bool, 9> occupied{};
    occupied[0] = true;
    row->SetSlots(occupied, 1u);
    row->AdvanceAnimation(0.12f);
    EXPECT_EQ(row->Button(1)->GetSize().Width, row->Button(0)->GetSize().Width);
    occupied[6] = true;
    occupied[0] = false;
    row->SetSlots(occupied, 7u);
    row->AdvanceAnimation(0.12f);
    for (std::size_t slot = 1u; slot <= 7u; ++slot) EXPECT_TRUE(row->Button(slot)->IsVisible());
    EXPECT_FALSE(row->Button(8)->IsVisible());
    EXPECT_EQ(row->Button(1)->GetToggleState(), gui::GuiToggleParams::TOGGLE_OFF);
    EXPECT_EQ(row->Button(7)->GetToggleState(), gui::GuiToggleParams::TOGGLE_ON);
    occupied.fill(true);
    row->SetSlots(occupied, 2u);
    EXPECT_FALSE(row->Button(0)->IsEnabled());
    row->AdvanceAnimation(0.12f);
    EXPECT_TRUE(row->Button(0)->IsVisible());
}

TEST(GuiPerformanceGroups, GeometryRetargetsFromCurrentBoundsAndRoutesPresentedHits)
{
    auto row = PerformanceGroupGuiTestHelpers::Row();
    std::array<bool, 9> occupied{};
    occupied[0] = true;
    row->SetSlots(occupied, 1u);
    row->AdvanceAnimation(0.06f);
    const auto width = row->Button(0)->GetSize().Width;
    const auto position = row->Button(0)->Position();
    occupied[2] = true;
    row->SetSlots(occupied, 3u);
    EXPECT_EQ(row->Button(0)->GetSize().Width, width);
    EXPECT_EQ(row->Button(0)->Position(), position);
    const auto point = row->Button(0)->GlobalPosition() + utils::Position2d{ 2, 8 };
    EXPECT_EQ(row->FindTopmostDescendant(point), row->Button(0));
    row->AdvanceAnimation(0.12f);
    EXPECT_FALSE(row->AdvanceAnimation(0.01f));
    row->SetSize({ 0u, 0u });
    EXPECT_FALSE(row->RouteHitTest({ 1, 1 }));
    row->SetSize({ 480u, 40u });
    row->AdvanceAnimation(0.12f);
    EXPECT_TRUE(row->Button(3)->IsVisible());
}

TEST(GuiPerformanceGroups, CapturedClickUsesPressModifiersAndNeverTogglesOccupancy)
{
    auto row = PerformanceGroupGuiTestHelpers::Row();
    row->SetSlots({ true, false, false, false, false, false, false, false, false }, 1u);
    row->AdvanceAnimation(0.12f);
    int calls = 0;
    engine::GroupCommand command{};
    row->SetHandler([&](engine::GroupCommand value, std::size_t slot) {
        ++calls; command = value; EXPECT_EQ(slot, 1u);
    });
    auto button = row->Button(1u);
    auto down = button->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_DOWN,
        { 12, 12 }, base::Action::MODIFIER_SHIFT));
    EXPECT_EQ(down.ActiveElement.lock(), button);
    button->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_UP, { 12, 12 }));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(command, engine::GroupCommand::Store);
    EXPECT_EQ(button->GetToggleState(), gui::GuiToggleParams::TOGGLE_ON);
    button->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_DOWN, { 12, 12 }));
    EXPECT_TRUE(button->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_UP, { -20, -20 })).IsEaten);
    EXPECT_EQ(calls, 1);
    button->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_UP, { 12, 12 }));
    EXPECT_EQ(calls, 1); // Release without a press never activates.
    const auto unsupported = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_ALT | base::Action::MODIFIER_SHIFT);
    EXPECT_FALSE(gui::GuiPerformanceGroupRow::ClickCommand(unsupported, false));
    EXPECT_FALSE(gui::GuiPerformanceGroupRow::ClickCommand(base::Action::MODIFIER_CTRL, false));
}

TEST(GuiPerformanceGroups, SettingsFollowActualTopStripBoundsAcrossCollapseAndSmallViewport)
{
    gui::GuiMainPanelParams params;
    params.Size = { 800u, 600u };
    auto panel = std::make_shared<gui::GuiMainPanel>(params);
    panel->Init();
    panel->SetOccupiedTopStripBounds({ 200, 300, 760, 590 });
    auto frame = panel->TryGetChild(0u);
    EXPECT_LE(frame->Position().Y + static_cast<int>(frame->GetSize().Height), 292);
    panel->SetExpanded(false);
    panel->AdvanceAnimation(0.05f);
    EXPECT_LE(frame->Position().Y + static_cast<int>(frame->GetSize().Height), 292);
    panel->SetOccupiedTopStripBounds({ 0, 20, 100, 100 });
    EXPECT_EQ(frame->GetSize().Height, 0u);
    panel->SetViewportSize({ 0u, 0u });
    EXPECT_FALSE(panel->RouteHitTest({ 0, 0 }));
}

TEST(GuiPerformanceGroups, DisablingAddOrHidingRowCancelsCapturedActivation)
{
    auto row = PerformanceGroupGuiTestHelpers::Row();
    int calls = 0;
    row->SetHandler([&](engine::GroupCommand, std::size_t) { ++calls; });
    auto add = row->Button(0u);
    add->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_DOWN, { 12, 12 }));
    std::array<bool, 9> occupied;
    occupied.fill(true);
    row->SetSlots(occupied, 1u);
    occupied[8] = false;
    row->SetSlots(occupied, 1u);
    row->AdvanceAnimation(0.12f);
    add->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_UP, { 12, 12 }));
    EXPECT_EQ(calls, 0);
    auto button = row->Button(1u);
    button->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_DOWN, { 12, 12 }));
    row->SetVisible(false);
    button->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_UP, { 12, 12 }));
    EXPECT_EQ(calls, 0);
}
