#pragma once

#include <memory>
#include "GuiPanel.h"
#include "GuiScrollBar.h"
#include "../actions/TouchAction.h"

namespace gui
{
	struct GuiScrollPanelParams : public base::GuiElementParams
	{
		static constexpr unsigned int DefaultPanelPaddingWidth = 10u;
		static constexpr unsigned int DefaultPanelMinWidth = 60u;
		static constexpr unsigned int DefaultPanelMinHeight = 40u;

		GuiScrollPanelParams()
		{
			GuiPassThrough = false;
		}

		unsigned int ScrollBarWidth = 12u;
		unsigned int WheelStep      = 24u;   // pixels scrolled per wheel notch.
		std::string  ScrollBarTexture = "rounded_but";
		std::string  ThumbTexture      = "blue";

		static GuiScrollPanelParams PanelScroll(unsigned int width, unsigned int height)
		{
			GuiScrollPanelParams params;
			params.Texture = "rounded_but";
			params.Size = { width + DefaultPanelPaddingWidth, height };
			params.MinSize = { DefaultPanelMinWidth, DefaultPanelMinHeight };
			return params;
		}
	};

	// A vertically scrollable viewport hosting a single content element plus a
	// scrollbar.  The content is expected to be sized to its full (logical)
	// height; the panel shows a window of height == panel height and offsets the
	// content vertically.  Mouse-wheel and scrollbar dragging both drive the
	// offset.  Scroll math is testable without a GL context.
	class GuiScrollPanel : public GuiPanel
	{
	public:
		explicit GuiScrollPanel(GuiScrollPanelParams params);

	public:
		void SetContent(std::shared_ptr<base::GuiElement> content);
		std::shared_ptr<base::GuiElement> Content() const;

		int ScrollOffset() const;
		void SetScrollOffset(int offset);
		void SetScrollFraction(double fraction);
		int MaxScrollOffset() const;
		unsigned int ViewportWidth() const;
		unsigned int ViewportHeight() const;

		using base::GuiElement::OnAction;
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual void InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;

	private:
		unsigned int _ContentHeight() const;
		void _UpdateMetrics();
		void _ClampOffset();

		static GuiScrollBarParams _MakeScrollBarParams(const GuiScrollPanelParams& params);

		std::shared_ptr<base::GuiElement> _content;
		std::shared_ptr<GuiScrollBar>     _scrollBar;
		static constexpr unsigned int _ContentClipPadding = 2u;
		unsigned int _scrollBarWidth;
		unsigned int _wheelStep;
		int          _scrollOffset;
		bool         _draggingScrollBar;
	};
}
