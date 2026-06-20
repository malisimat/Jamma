#pragma once

#include <string>
#include "GuiTextBox.h"

namespace gui
{
	struct GuiNumericInputParams : public GuiTextBoxParams
	{
		double Min       = 0.0;
		double Max       = 1.0;
		double Step      = 0.01;   // value change per pixel of drag.
		double InitValue = 0.0;
		int    Decimals  = 2;

		static GuiNumericInputParams PanelInput(unsigned int width)
		{
			GuiNumericInputParams params;
			params.Texture = "rounded_but";
			params.Size = { width, DefaultHeight };
			params.MinSize = { DefaultMinWidth, DefaultMinHeight };
			params.Padding = DefaultPadding;
			return params;
		}
	};

	// Numeric entry composed from GuiTextBox plus drag/step adjustment.
	//
	// Typing is restricted to digits, sign and decimal point.  Vertical dragging
	// adjusts the value by Step per pixel.  The value is parsed and clamped on
	// commit (Enter or focus loss); parse failures revert to the last valid value.
	class GuiNumericInput : public GuiTextBox
	{
	public:
		explicit GuiNumericInput(GuiNumericInputParams params);

	public:
		double Value() const;
		void SetValue(double value, bool notify = false);

		using GuiTextBox::OnAction;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;

	protected:
		virtual bool _AcceptChar(char c) const override;
		virtual void _OnCommit() override;

	private:
		double _Clamp(double v) const;
		std::string _Format(double v) const;
		void _ApplyValue(double v, bool notify);

		static GuiNumericInputParams _WithInitialText(GuiNumericInputParams params);

		double _min;
		double _max;
		double _step;
		int    _decimals;
		double _value;

		bool              _dragging;
		bool              _dragMoved;
		utils::Position2d _dragStart;
		double            _dragStartValue;
	};
}
