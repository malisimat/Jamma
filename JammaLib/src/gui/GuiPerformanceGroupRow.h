#pragma once

#include <array>
#include <functional>
#include <optional>
#include "GuiPanel.h"
#include "GuiToggle.h"
#include "../engine/PerformanceGroups.h"

namespace gui
{
    // UI/job owner only, under the existing Scene serialization. Rendering
    // and hit testing share the same presented child bounds. No audio reader.
    class GuiPerformanceGroupButton : public GuiToggle
    {
    public:
        GuiPerformanceGroupButton(GuiToggleParams params,
            std::function<void(base::Action::Modifiers)> activate);
        using GuiToggle::OnAction;
        actions::ActionResult OnAction(actions::TouchAction action) override;
        actions::ActionResult OnAction(actions::KeyAction action) override;
        void ClearPointerState() override;
        void Draw(base::DrawContext& context) override;
        void SetSize(utils::Size2d size) override;
        void SetActive(bool active);
    private:
        std::function<void(base::Action::Modifiers)> _activate;
        std::shared_ptr<GuiPanel> _activeCue;
        bool _pressed = false;
        base::Action::Modifiers _pressModifiers = base::Action::MODIFIER_NONE;
    };

    class GuiPerformanceGroupRow : public GuiPanel
    {
    public:
        static constexpr unsigned int ButtonHeight = 32u;
        static constexpr unsigned int Padding = 4u;
        static constexpr unsigned int Height = ButtonHeight + 2u * Padding;
        static constexpr float TransitionSeconds = 0.12f;
        using Handler = std::function<void(engine::GroupCommand, std::size_t)>;
        GuiPerformanceGroupRow(bool selection);
        void SetHandler(Handler handler) { _handler = std::move(handler); }
        void SetSlots(const std::array<bool, 9>& occupied, std::optional<std::size_t> active);
        void SetSize(utils::Size2d size) override;
        bool AdvanceAnimation(float elapsedSeconds);
        void Draw(base::DrawContext& context) override;
        void DrawTooltip(base::DrawContext& context, utils::Position2d rootPosition);
        bool RouteHitTest(utils::Position2d position) override;
        using GuiPanel::OnAction;
        actions::ActionResult OnAction(actions::TouchAction action) override;
        static std::optional<engine::GroupCommand> ClickCommand(base::Action::Modifiers modifiers, bool add);
        std::shared_ptr<GuiPerformanceGroupButton> Button(std::size_t slot) const;
        std::optional<std::size_t> HoveredSlot() const;
    private:
        struct Geometry { float X = 0.0f; float Width = 0.0f; };
        static base::GuiElementParams _Params(bool selection);
        static GuiToggleParams _ButtonParams(bool selection, std::size_t slot);
        void _Retarget();
        void _Present();
        Handler _handler;
        std::shared_ptr<GuiLabel> _label;
        std::shared_ptr<GuiPanel> _holder;
        std::shared_ptr<GuiPanel> _tooltip;
        std::shared_ptr<GuiLabel> _tooltipLabel;
        std::optional<std::size_t> _tooltipSlot;
        std::array<std::shared_ptr<GuiPerformanceGroupButton>, 10> _buttons;
        std::array<bool, 9> _occupied{};
        std::array<bool, 10> _visible{};
        std::array<Geometry, 10> _presented{};
        std::array<Geometry, 10> _start{};
        std::array<Geometry, 10> _target{};
        std::size_t _visibleSlots = 0u;
        float _elapsed = TransitionSeconds;
        bool _layoutInitialized = false;
    };
}
