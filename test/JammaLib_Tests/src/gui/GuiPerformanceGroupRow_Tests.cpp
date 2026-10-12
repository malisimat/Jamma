#include "gtest/gtest.h"
#include "gui/GuiPerformanceGroupRow.h"
#include "gui/GuiMainPanel.h"
#include "gui/GuiHud.h"
#include "graphics/GlDrawContext.h"

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

class SelectionGroupPanelTests : public ::testing::Test
{
protected:
    std::shared_ptr<gui::GuiRadio> radio;
    std::shared_ptr<gui::GuiMainPanel> panel;
    std::shared_ptr<gui::GuiScrollPanel> scroll;
    std::shared_ptr<gui::GuiPerformanceGroupRow> mute, selection;
    void SetUp() override
    {
        gui::GuiRadioParams radios;
        radios.Size = { 384u, 64u };
        for (unsigned int index = 0; index < 3; ++index)
        {
            gui::GuiToggleParams toggle;
            toggle.Size = { 128u, 64u };
            radios.ToggleParams.push_back(toggle);
        }
        radio = std::make_shared<gui::GuiRadio>(radios);
        gui::GuiMainPanelParams params;
        params.Size = { 800u, 600u };
        params.SelectionOnly = true;
        params.Settings = { { gui::SettingsPage::Selection, "", radio, 100u } };
        panel = std::make_shared<gui::GuiMainPanel>(params);
        panel->Init();
        scroll = std::dynamic_pointer_cast<gui::GuiScrollPanel>(panel->TryGetChild(0)->TryGetChild(0));
        ASSERT_NE(nullptr, scroll);
        mute = std::dynamic_pointer_cast<gui::GuiPerformanceGroupRow>(scroll->Content()->TryGetChild(1));
        selection = std::dynamic_pointer_cast<gui::GuiPerformanceGroupRow>(scroll->Content()->TryGetChild(2));
        ASSERT_NE(nullptr, mute); ASSERT_NE(nullptr, selection);
        panel->AdvanceAnimation(0.12f);
    }
};

TEST_F(SelectionGroupPanelTests, StackSharesActualRadioBoundsForEmptySingleAndFullRows)
{
    for (const unsigned int count : { 0u, 1u, 9u })
    {
        engine::PerformanceGroups groups;
        for (unsigned int slot = 1; slot <= count; ++slot)
        {
            groups.Mute.Store(slot, {});
            groups.Selection.Store(slot, {});
        }
        panel->RefreshPerformanceGroups(groups);
        panel->AdvanceAnimation(0.12f);
        for (const auto& row : { mute, selection })
        {
            EXPECT_EQ(radio->GetSize().Width, row->GetSize().Width);
            EXPECT_EQ(radio->GlobalPosition().X, row->GlobalPosition().X);
            EXPECT_EQ(count != 0u, row->Button(1)->IsVisible());
            EXPECT_EQ(count < 9u, row->Button(0)->IsEnabled());
        }
        EXPECT_GT(radio->Position().Y, mute->Position().Y + static_cast<int>(mute->GetSize().Height));
        EXPECT_GT(mute->Position().Y, selection->Position().Y + static_cast<int>(selection->GetSize().Height));
        if (count < 9u) EXPECT_EQ(mute->Button(0), panel->FindTopmostDescendant(mute->Button(0)->GlobalPosition() + utils::Position2d{ 2, 8 }));
    }
}

TEST_F(SelectionGroupPanelTests, CollapseClearsPressAndHoverAndReopenRefreshesRetainedRows)
{
    int calls = 0;
    panel->SetPerformanceGroupHandler([&](bool, engine::GroupCommand, std::size_t) { ++calls; });
    auto add = mute->Button(0);
    const auto point = add->GlobalPosition() + utils::Position2d{ 12, 12 };
    ASSERT_TRUE(panel->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_DOWN, point)).IsEaten);
    panel->SetExpanded(false);
    EXPECT_EQ(base::GuiElement::STATE_NORMAL, add->GetState());
    EXPECT_FALSE(mute->IsEnabled());
    add->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_UP, { 12, 12 }));
    EXPECT_EQ(0, calls);
    for (int frame = 0; frame < 6; ++frame) panel->AdvanceAnimation(0.05f);
    EXPECT_FALSE(panel->TryGetChild(0)->IsVisible());
    EXPECT_FALSE(panel->RouteHitTest(point));
    engine::PerformanceGroups groups;
    groups.Mute.Store(1, {});
    panel->RefreshPerformanceGroups(groups);
    panel->AdvanceAnimation(0.12f);
    panel->SetExpanded(true);
    for (int frame = 0; frame < 6; ++frame) panel->AdvanceAnimation(0.05f);
    EXPECT_TRUE(mute->IsEnabled());
    EXPECT_TRUE(mute->Button(1)->IsVisible());
    EXPECT_EQ(gui::GuiToggleParams::TOGGLE_ON, mute->Button(1)->GetToggleState());
    add->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_UP, { 12, 12 }));
    EXPECT_EQ(0, calls);
}

TEST_F(SelectionGroupPanelTests, SmallViewportScrollsWithoutWideningRowsBeyondRadioClip)
{
    for (const auto size : { utils::Size2d{ 220u, 120u }, { 80u, 60u }, { 1u, 1u }, { 0u, 0u }, { 800u, 600u } })
    {
        panel->SetViewportSize(size);
        panel->AdvanceAnimation(0.12f);
        EXPECT_EQ(radio->GetSize().Width, mute->GetSize().Width);
        EXPECT_EQ(radio->GetSize().Width, selection->GetSize().Width);
        if (scroll->ViewportWidth() >= 4u) EXPECT_LE(radio->GetSize().Width + 4u, scroll->ViewportWidth());
        EXPECT_GE(scroll->ScrollOffset(), 0);
        EXPECT_LE(scroll->ScrollOffset(), scroll->MaxScrollOffset());
        if (size.Height == 120u) EXPECT_TRUE(scroll->IsScrollBarVisible());
        if (size.Height == 600u) EXPECT_FALSE(scroll->IsScrollBarVisible());
    }
}

TEST_F(SelectionGroupPanelTests, SettingsStayBelowExpandedSelectionWhenViewportHasUsableSpace)
{
    gui::GuiMainPanelParams params;
    params.Size = { 800u, 500u };
    auto settings = std::make_shared<gui::GuiMainPanel>(params);
    settings->Init();
    for (const auto size : { utils::Size2d{ 800u, 500u }, { 800u, 600u }, { 800u, 900u } })
    {
        panel->SetViewportSize(size);
        settings->SetViewportSize(size);
        const auto selectionFrame = panel->TryGetChild(0);
        const auto settingsFrame = settings->TryGetChild(0);
        EXPECT_GT(settingsFrame->GetSize().Height, 0u);
        EXPECT_LT(settingsFrame->GlobalPosition().Y + static_cast<int>(settingsFrame->GetSize().Height),
            selectionFrame->GlobalPosition().Y);
    }
}

TEST_F(SelectionGroupPanelTests, CapturedReleaseOutsideScrollClipCancelsPartlyVisibleButton)
{
    panel->SetViewportSize({ 800u, 150u });
    panel->AdvanceAnimation(0.12f);
    auto add = mute->Button(0);
    const auto clip = scroll->ContentRect().Translated(scroll->GlobalPosition());
    const auto position = add->GlobalPosition();
    const auto press = utils::Position2d{ position.X + 12, clip.Bottom + 2 };
    const auto release = utils::Position2d{ press.X, clip.Bottom - 1 };
    ASSERT_TRUE(clip.Contains(press));
    ASSERT_FALSE(clip.Contains(release));
    ASSERT_TRUE(utils::Size2d::RectTest(add->GetSize(), add->GlobalToLocal(press)));
    ASSERT_TRUE(utils::Size2d::RectTest(add->GetSize(), add->GlobalToLocal(release)));
    ASSERT_EQ(add, panel->FindTopmostDescendant(press));
    int calls = 0;
    panel->SetPerformanceGroupHandler([&](bool, engine::GroupCommand, std::size_t) { ++calls; });
    auto down = panel->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_DOWN, press));
    ASSERT_EQ(add, down.ActiveElement.lock());
    auto up = add->OnAction(PerformanceGroupGuiTestHelpers::Touch(actions::TouchAction::TOUCH_UP, add->GlobalToLocal(release)));
    EXPECT_TRUE(up.IsEaten);
    EXPECT_EQ(0, calls);
}

TEST(GuiPerformanceGroups, HoverPreviewRequiresOccupiedVisibleSlotAndClearsWithPointerState)
{
    auto row = PerformanceGroupGuiTestHelpers::Row();
    row->SetSlots({ false, true }, 2u);
    row->AdvanceAnimation(0.12f);
    row->Button(1)->ApplyHoverState(true);
    EXPECT_FALSE(row->HoveredSlot()); // Interior hole is visible but has no snapshot.
    row->Button(1)->ClearPointerState();
    row->Button(0)->ApplyHoverState(true);
    EXPECT_FALSE(row->HoveredSlot()); // Add never previews current state.
    row->Button(0)->ClearPointerState();
    row->Button(2)->ApplyHoverState(true);
    EXPECT_EQ(row->HoveredSlot(), 2u);
    EXPECT_EQ(row->Button(2)->GetToggleState(), gui::GuiToggleParams::TOGGLE_ON);
    row->ClearPointerState();
    EXPECT_FALSE(row->HoveredSlot());
    row->Button(2)->ApplyHoverState(true);
    row->SetVisible(false);
    EXPECT_FALSE(row->HoveredSlot());
    row->SetVisible(true);
    row->Button(2)->ApplyHoverState(true);
    row->SetSlots({}, {});
    EXPECT_FALSE(row->HoveredSlot());
}

TEST_F(SelectionGroupPanelTests, HoverPreviewStopsImmediatelyOnCollapseAndHiddenAncestors)
{
    engine::PerformanceGroups groups;
    groups.Mute.Store(1u, {});
    groups.Selection.Store(1u, {});
    panel->RefreshPerformanceGroups(groups);
    panel->AdvanceAnimation(0.12f);
    mute->Button(1)->ApplyHoverState(true);
    ASSERT_TRUE(panel->HoveredPerformanceGroup());
    EXPECT_EQ(*panel->HoveredPerformanceGroup(), std::make_pair(false, std::size_t(1)));
    panel->SetExpanded(false);
    EXPECT_FALSE(panel->HoveredPerformanceGroup());
    panel->SetExpanded(true);
    panel->AdvanceAnimation(0.30f);
    EXPECT_FALSE(panel->HoveredPerformanceGroup()); // No resurrected old hover.
    selection->Button(1)->ApplyHoverState(true);
    EXPECT_EQ(*panel->HoveredPerformanceGroup(), std::make_pair(true, std::size_t(1)));
    scroll->SetVisible(false);
    EXPECT_FALSE(panel->HoveredPerformanceGroup());
    scroll->SetVisible(true);
    scroll->SetEnabled(false);
    EXPECT_FALSE(panel->HoveredPerformanceGroup());
    scroll->SetEnabled(true);
    selection->SetPosition({ 0, 10000 });
    EXPECT_FALSE(panel->HoveredPerformanceGroup()); // Entire button outside ancestor clip.
}

TEST(GuiPerformanceGroups, RenderPreviewResetsUniformsAndDoesNotTouchActualInteractionState)
{
    graphics::GlDrawContext context({ 100u, 100u }, base::DrawContext::SCREEN);
    const glm::vec3 tint(0.259f, 0.588f, 1.0f);
    context.SetUniform("LoopSelected", 1.0f);
    context.SetUniform("LoopMuted", 1.0f);
    context.SetUniform("LoopHover", 0.0f);
    context.SetUniform("ObjectId", 123u);
    context.SetGroupPreview(true, tint, { { { 0u, 1u, 2u }, true }, { { 0u, 1u, 3u }, false } });
    context.ApplyGroupPreview({ 0u, 1u, 2u });
    EXPECT_EQ(std::any_cast<float>(*context.GetUniform("GroupPreview")), 1.0f);
    EXPECT_EQ(std::any_cast<glm::vec3>(*context.GetUniform("GroupPreviewTint")), tint);
    EXPECT_EQ(context.GroupPreview({ 0u, 1u, 3u }), -1.0f);
    EXPECT_EQ(context.GroupPreview({ 9u }), -1.0f); // New/uncaptured target.
    context.SetGroupPreview(false, glm::vec3(1.0f), {});
    context.ApplyGroupPreview({});
    EXPECT_EQ(std::any_cast<float>(*context.GetUniform("GroupPreview")), 0.0f);
    EXPECT_EQ(context.GroupPreview({ 0u, 1u, 2u }), 0.0f);
    EXPECT_EQ(std::any_cast<float>(*context.GetUniform("LoopSelected")), 1.0f);
    EXPECT_EQ(std::any_cast<float>(*context.GetUniform("LoopMuted")), 1.0f);
    EXPECT_EQ(std::any_cast<float>(*context.GetUniform("LoopHover")), 0.0f);
    EXPECT_EQ(std::any_cast<unsigned int>(*context.GetUniform("ObjectId")), 123u);
}
