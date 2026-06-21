#pragma once

#include <functional>
#include <memory>
#include "GuiElement.h"
#include "../actions/TouchAction.h"

namespace gui
{
	struct GuiScrollBarParams : public base::GuiElementParams
	{
		GuiScrollBarParams()
		{
			GuiPassThrough = false;
		}

		unsigned int MinThumb     = 16u;   // minimum thumb length in pixels.
		std::string  ThumbTexture = "blue";
	};

	// Vertical scrollbar.  Reports a normalised scroll position in [0, 1] via a
	// callback.  The track length is the element height; the thumb length is
	// derived from the viewport/content ratio.  Range math is exposed as static
	// helpers so it can be unit-tested without a GL context.
	class GuiScrollBar : public base::GuiElement
	{
	public:
		explicit GuiScrollBar(GuiScrollBarParams params);

	public:
		// Update viewport/content lengths (pixels) used to size the thumb.
		void SetMetrics(double viewportLength, double contentLength);
		double Value() const;
		void SetValue(double value);
		void SetOnScroll(std::function<void(double)> onScroll);
		bool IsScrollable() const;

		using base::GuiElement::OnAction;
		virtual void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;

		// Range math (track length and lengths in pixels).
		static double ThumbFraction(double viewport, double content);
		static unsigned int ThumbLength(unsigned int track, double viewport, double content, unsigned int minThumb);
		static int ThumbOffset(unsigned int track, unsigned int thumbLength, double value);
		static double ValueFromOffset(unsigned int track, unsigned int thumbLength, int offset);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;

	private:
		void _UpdateThumb();

		static base::GuiElementParams _MakeThumbParams(const GuiScrollBarParams& params);

		double            _value;
		double            _viewportLength;
		double            _contentLength;
		unsigned int      _minThumb;
		base::GuiElement  _thumb;
		bool              _dragging;
		int               _dragStartY;
		double            _dragStartValue;
		std::function<void(double)> _onScroll;
	};
}
