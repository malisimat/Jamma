#include "GuiPerformanceGroupRow.h"

#include <algorithm>
#include <cmath>
#include "../graphics/GlDrawContext.h"
#include "GuiScrollPanel.h"

using namespace gui;
using namespace base;
using namespace actions;

GuiPerformanceGroupButton::GuiPerformanceGroupButton(GuiToggleParams params,
    std::function<void(Action::Modifiers)> activate) : GuiToggle(params), _activate(std::move(activate))
{
    GuiElementParams cue;
    cue.Texture = "performance_group_on";
    cue.TextureShader = "texture_tinted";
    cue.TintColor = glm::vec3(1.0f);
    cue.Size = { 16u, 3u };
    cue.MinSize = { 0u, 0u };
    _activeCue = std::make_shared<GuiPanel>(cue);
    _activeCue->SetVisible(false);
    AddChild(_activeCue);
}

ActionResult GuiPerformanceGroupButton::OnAction(TouchAction action)
{
    const auto row = _parent ? _parent->Parent() : nullptr;
    if (!IsVisible() || !IsEnabled() || (row && (!row->IsVisible() || !row->IsEnabled())))
    {
        ClearPointerState();
        return ActionResult::NoAction();
    }
    if (action.Touch == TouchAction::TOUCH_MOUSE && action.Index != 0) return ActionResult::NoAction();
    const bool wasPressed = _pressed;
    const auto globalPoint = GlobalPosition() + action.Position;
    bool inClip = !row || utils::Size2d::RectTest(row->GetSize(), row->GlobalToLocal(globalPoint));
    // Scene sends captured releases directly to this button. Recheck every
    // scroll ancestor so release outside a partially clipped button cancels.
    for (auto ancestor = Parent(); ancestor; ancestor = ancestor->Parent())
    {
        if (!ancestor->IsVisible() || !ancestor->IsEnabled()) inClip = false;
        if (const auto scroll = std::dynamic_pointer_cast<GuiScrollPanel>(ancestor))
            inClip = inClip && scroll->ContentRect().Translated(scroll->GlobalPosition()).Contains(globalPoint);
    }
    if (!inClip && action.State == TouchAction::TOUCH_DOWN) return ActionResult::NoAction();
    auto result = GuiButton::OnAction(action); // Occupancy is model state, never a click toggle.
    if (action.State == TouchAction::TOUCH_DOWN && result.IsEaten)
    {
        _pressed = true;
        _pressModifiers = action.Modifiers;
    }
    else if (action.State == TouchAction::TOUCH_UP)
    {
        _pressed = false;
        if (wasPressed && result.IsEaten && inClip && _activate) _activate(_pressModifiers);
        if (wasPressed) result.IsEaten = true; // Consume captured release even outside.
    }
    return result;
}

ActionResult GuiPerformanceGroupButton::OnAction(KeyAction action)
{
    return GuiElement::OnAction(action); // Dedicated controller owns keyboard commands.
}

void GuiPerformanceGroupButton::ClearPointerState()
{
    _pressed = false;
    GuiToggle::ClearPointerState();
}

void GuiPerformanceGroupButton::Draw(DrawContext& context)
{
    auto opacity = context.WithOpacity(IsEnabled() ? 1.0f : 0.30f);
    GuiToggle::Draw(context);
}

void GuiPerformanceGroupButton::SetSize(utils::Size2d size)
{
    GuiToggle::SetSize(size);
    const auto width = size.Width > 8u ? size.Width - 8u : 0u;
    _activeCue->SetPosition({ 4, 3 });
    _activeCue->SetSize({ width, 3u });
}

void GuiPerformanceGroupButton::SetActive(bool active) { _activeCue->SetVisible(active); }

GuiElementParams GuiPerformanceGroupRow::_Params(bool selection)
{
    GuiElementParams params;
    params.Size = { 480u, Height };
    params.MinSize = { 0u, 0u };
    return params;
}

GuiToggleParams GuiPerformanceGroupRow::_ButtonParams(bool selection, std::size_t slot)
{
    GuiToggleParams params;
    const bool add = slot == 0u;
    params.Texture = add ? "performance_group_add" : "performance_group";
    params.OverTexture = params.Texture + "_over";
    params.DownTexture = params.Texture + "_down";
    params.ToggledTexture = "performance_group_on";
    params.ToggledOverTexture = "performance_group_on_over";
    params.ToggledDownTexture = "performance_group_on_down";
    params.TextureShader = "texture_tinted";
    params.TintColor = add ? glm::vec3(1.0f) : selection ? glm::vec3(1.0f, 0.875f, 0.208f) : glm::vec3(0.259f, 0.588f, 1.0f);
    params.Text = add ? "+" : std::to_string(slot);
    params.TextPadding = 0u;
    params.Size = { 32u, ButtonHeight };
    params.MinSize = { 0u, 0u };
    return params;
}

GuiPerformanceGroupRow::GuiPerformanceGroupRow(bool selection) : GuiPanel(_Params(selection))
{
    auto label = GuiLabelParams::PanelHeader(selection ? "Selection" : "Mute", 84u);
    label.Size.Height = Height;
    label.MinSize = { 0u, 0u };
    _label = std::make_shared<GuiLabel>(label);
    AddChild(_label);
    GuiElementParams holder;
    holder.Texture = "performance_group_holder";
    holder.TextureShader = "texture_tinted";
    holder.TintColor = _ButtonParams(selection, 1u).TintColor;
    holder.Size = { 400u, Height };
    holder.MinSize = { 0u, 0u };
    _holder = std::make_shared<GuiPanel>(holder);
    AddChild(_holder);
    for (std::size_t index = 0u; index < _buttons.size(); ++index)
    {
        const std::size_t slot = index == 9u ? 0u : index + 1u;
        _buttons[index] = std::make_shared<GuiPerformanceGroupButton>(_ButtonParams(selection, slot),
            [this, slot](Action::Modifiers modifiers) {
                const auto command = ClickCommand(modifiers, slot == 0u);
                if (command && _handler) _handler(*command, slot);
            });
        _holder->AddChild(_buttons[index]);
    }
    GuiElementParams tooltip;
    tooltip.Texture = "panel_fill_flat";
    tooltip.TextureShader = "texture_tinted";
    tooltip.TintColor = GuiStyle::Graphite();
    tooltip.Size = { 480u, 24u };
    tooltip.MinSize = { 0u, 0u };
    _tooltip = std::make_shared<GuiPanel>(tooltip);
    auto text = GuiLabelParams::PanelHeader("", 480u);
    text.Size.Height = 20u;
    text.Ellipsize = true;
    text.Position = { 4, 2 };
    _tooltipLabel = std::make_shared<GuiLabel>(text);
    _tooltip->AddChild(_tooltipLabel);
    _tooltip->SetVisible(false);
    AddChild(_tooltip);
    SetSize(GetSize());
}

std::optional<engine::GroupCommand> GuiPerformanceGroupRow::ClickCommand(Action::Modifiers modifiers, bool add)
{
    if (add) return modifiers == Action::MODIFIER_NONE ? std::optional(engine::GroupCommand::Add) : std::nullopt;
    switch (modifiers)
    {
    case Action::MODIFIER_NONE: return engine::GroupCommand::RecallOrStore;
    case Action::MODIFIER_SHIFT: return engine::GroupCommand::Store;
    case Action::MODIFIER_ALT: return engine::GroupCommand::Clear;
    default: return std::nullopt;
    }
}

void GuiPerformanceGroupRow::SetSlots(const std::array<bool, 9>& occupied, std::optional<std::size_t> active)
{
    const bool changed = occupied != _occupied;
    _occupied = occupied;
    _visibleSlots = 0u;
    for (std::size_t i = 0u; i < occupied.size(); ++i)
    {
        _buttons[i]->SetToggleState(occupied[i] ? GuiToggleParams::TOGGLE_ON : GuiToggleParams::TOGGLE_OFF, true);
        _buttons[i]->SetActive(active == i + 1u && occupied[i]);
        if (occupied[i]) _visibleSlots = i + 1u;
    }
    const bool addEnabled = std::find(occupied.begin(), occupied.end(), false) != occupied.end();
    if (!addEnabled) _buttons[9]->ClearPointerState();
    _buttons[9]->SetEnabled(addEnabled);
    if (changed) _Retarget();
}

void GuiPerformanceGroupRow::SetSize(utils::Size2d size)
{
    const auto old = GetSize();
    GuiPanel::SetSize(size);
    const auto labelWidth = size.Width >= 160u ? 84u : 0u;
    _label->SetVisible(labelWidth != 0u);
    _label->SetSize({ labelWidth, std::min(size.Height, Height) });
    _holder->SetPosition({ static_cast<int>(labelWidth), 0 });
    _holder->SetSize({ size.Width - labelWidth, std::min(size.Height, Height) });
    _tooltip->SetSize({ size.Width, 24u });
    _tooltip->SetPosition({ 0, static_cast<int>(Height + 2u) });
    _tooltipLabel->SetSize({ size.Width > 8u ? size.Width - 8u : 0u, 20u });
    if (!_layoutInitialized || old.Width != size.Width || old.Height != size.Height) _Retarget();
}

void GuiPerformanceGroupRow::_Retarget()
{
    _start = _presented; // Retarget from current geometry, including rapid changes.
    const float usable = static_cast<float>(_holder->GetSize().Width > 2u * Padding ? _holder->GetSize().Width - 2u * Padding : 0u);
    const auto count = _visibleSlots + 1u;
    const float gap = std::min(4.0f, usable / static_cast<float>(2u * count));
    const float width = std::max(0.0f, (usable - gap * static_cast<float>(count - 1u)) / static_cast<float>(count));
    for (std::size_t i = 0u; i < _buttons.size(); ++i)
    {
        const bool visible = i < _visibleSlots || i == 9u;
        const auto position = i == 9u ? _visibleSlots : i;
        _target[i] = { static_cast<float>(Padding) + static_cast<float>(position) * (width + gap), visible ? width : 0.0f };
        if (visible && !_visible[i] && _layoutInitialized)
            _start[i] = { _presented[9].X, 0.0f };
        if (!visible && _visible[i]) _buttons[i]->ClearPointerState();
        _visible[i] = visible;
    }
    _elapsed = _layoutInitialized ? 0.0f : TransitionSeconds;
    _layoutInitialized = true;
    _Present();
}

void GuiPerformanceGroupRow::_Present()
{
    const float fraction = std::clamp(_elapsed / TransitionSeconds, 0.0f, 1.0f);
    const float eased = fraction * fraction * (3.0f - 2.0f * fraction);
    const auto height = _holder->GetSize().Height > 2u * Padding ? std::min(ButtonHeight, _holder->GetSize().Height - 2u * Padding) : 0u;
    for (std::size_t i = 0u; i < _buttons.size(); ++i)
    {
        auto& presented = _presented[i];
        presented.X = _start[i].X + (_target[i].X - _start[i].X) * eased;
        presented.Width = _start[i].Width + (_target[i].Width - _start[i].Width) * eased;
        const int left = static_cast<int>(std::lround(presented.X));
        const int right = static_cast<int>(std::lround(presented.X + presented.Width));
        _buttons[i]->SetPosition({ left, static_cast<int>(Padding) });
        _buttons[i]->SetSize({ static_cast<unsigned int>(std::max(0, right - left)), height });
        _buttons[i]->SetVisible(_visible[i] && right > left && height > 0u);
    }
}

bool GuiPerformanceGroupRow::AdvanceAnimation(float elapsedSeconds)
{
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0f || _elapsed >= TransitionSeconds) return false;
    _elapsed = std::min(TransitionSeconds, _elapsed + elapsedSeconds);
    _Present();
    return true;
}

void GuiPerformanceGroupRow::Draw(DrawContext& context)
{
    if (!IsVisible()) return;
    auto& gl = dynamic_cast<graphics::GlDrawContext&>(context);
    gl.PushScissorRect(GlobalPosition(), GetSize());
    GuiPanel::Draw(context);
    gl.PopScissorRect();
}

std::shared_ptr<GuiPerformanceGroupButton> GuiPerformanceGroupRow::Button(std::size_t slot) const
{
    return slot <= 9u ? _buttons[slot == 0u ? 9u : slot - 1u] : nullptr;
}

bool GuiPerformanceGroupRow::RouteHitTest(utils::Position2d position)
{
    return IsVisible() && IsEnabled() && utils::Size2d::RectTest(GetSize(), position);
}

ActionResult GuiPerformanceGroupRow::OnAction(TouchAction action)
{
    if (action.State == TouchAction::TOUCH_DOWN && !RouteHitTest(action.Position)) return ActionResult::NoAction();
    return GuiPanel::OnAction(action);
}

void GuiPerformanceGroupRow::DrawTooltip(DrawContext& context, utils::Position2d rootPosition)
{
    if (!IsVisible() || !IsEnabled()) return;
    std::optional<std::size_t> hovered;
    for (std::size_t i = 0u; i < _buttons.size(); ++i)
        if (_buttons[i]->IsVisible() && _buttons[i]->IsEnabled() && _buttons[i]->GetState() == STATE_OVER) hovered = i;
    if (!hovered) return;
    if (_tooltipSlot != hovered)
    {
        _tooltipLabel->SetString(*hovered == 9u ? "Capture current state into the lowest empty slot"
            : "Click: recall / capture   Shift: overwrite   Alt: clear");
        _tooltipSlot = hovered;
    }
    auto& gl = dynamic_cast<graphics::GlDrawContext&>(context);
    const auto position = GlobalPosition() - rootPosition;
    gl.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3(position.X, position.Y, 0.0f)));
    _tooltip->SetVisible(true);
    _tooltip->Draw(context);
    _tooltip->SetVisible(false);
    gl.PopMvp();
}
