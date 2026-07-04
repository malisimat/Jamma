#pragma once

#include <vector>
#include "GuiPanel.h"

namespace gui
{
	// Direction in which children are stacked.
	enum class StackDirection : std::uint8_t
	{
		Horizontal,
		Vertical
	};

	struct GuiStackPanelParams : public base::GuiElementParams
	{
		static constexpr unsigned int PanelRowSpacing = 6u;
		static constexpr unsigned int PanelSectionSpacing = 8u;
		static constexpr unsigned int PanelContentSpacing = 2u;
		static constexpr unsigned int PanelContentPadding = 2u;

		StackDirection Direction    = StackDirection::Vertical;
		unsigned int   Spacing      = 0u;  // gap in pixels between children.
		unsigned int   PaddingH     = 0u;  // horizontal padding (left+right each).
		unsigned int   PaddingV     = 0u;  // vertical padding (top+bottom each).
		bool           WrapContent  = false; // Horizontal only: wrap to a new row
		                                     // when the row is too wide.

		static GuiStackPanelParams PanelHorizontalRow(unsigned int width, unsigned int height)
		{
			GuiStackPanelParams params;
			params.Direction = StackDirection::Horizontal;
			params.Spacing = PanelRowSpacing;
			params.Size = { width, height };
			params.MinSize = { 80u, height };
			return params;
		}

		static GuiStackPanelParams PanelContentStack(unsigned int width, unsigned int height)
		{
			GuiStackPanelParams params;
			params.Direction = StackDirection::Vertical;
			params.Spacing = PanelContentSpacing;
			params.PaddingH = PanelContentPadding;
			params.PaddingV = PanelContentPadding;
			params.Size = { width, height };
			params.MinSize = { 60u, 40u };
			return params;
		}
	};

	// A retained stack-layout container.
	// Children are stacked in the chosen direction.  A child with
	// LayoutSizing::Fill in the stacking axis stretches to fill remaining space.
	// Call InvalidateLayout() whenever child sizes or the panel size change.
	// Layout is computed lazily on Draw() or explicitly via ComputeLayout().
	class GuiStackPanel : public GuiPanel
	{
	public:
		explicit GuiStackPanel(GuiStackPanelParams params);

	public:
		void SetDirection(StackDirection direction);
		void SetSpacing(unsigned int spacing);
		void SetPadding(unsigned int horizontal, unsigned int vertical);
		void SetWrapContent(bool wrap);

		virtual void AddChild(std::shared_ptr<base::GuiElement> child) override;
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual bool RouteHitTest(utils::Position2d localPos) override;

		void InvalidateLayout();
		void ComputeLayout();

		// Initialise resources for self + children, then immediately run layout.
		virtual void InitResources(resources::ResourceLib& resourceLib,
		                           bool forceInit) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib,
		                            bool forceInit) override;

	private:
		void _ComputeVertical();
		void _ComputeHorizontal();
		void _ComputeHorizontalWrapped();

		StackDirection _direction;
		unsigned int   _spacing;
		utils::Size2d  _padding;
		bool           _wrapContent;
		bool           _layoutDirty;
	};
}
