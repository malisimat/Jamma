#include "GuiNumericInput.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace gui;
using namespace actions;
using namespace utils;

GuiNumericInputParams GuiNumericInput::_WithInitialText(GuiNumericInputParams params)
{
	// Seed the text field with the formatted initial value.
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", std::max(0, params.Decimals), params.InitValue);
	params.Text = buf;
	return params;
}

GuiNumericInput::GuiNumericInput(GuiNumericInputParams params) :
	GuiTextBox(_WithInitialText(params)),
	_min(params.Min),
	_max(params.Max),
	_step(params.Step),
	_decimals(std::max(0, params.Decimals)),
	_value(0.0),
	_dragging(false),
	_dragMoved(false),
	_dragStart{ 0, 0 },
	_dragStartValue(0.0)
{
	_value = _Clamp(params.InitValue);
}

double GuiNumericInput::Value() const
{
	return _value;
}

void GuiNumericInput::SetValue(double value, bool notify)
{
	_ApplyValue(value, notify);
}

double GuiNumericInput::_Clamp(double v) const
{
	return std::clamp(v, _min, _max);
}

std::string GuiNumericInput::_Format(double v) const
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", _decimals, v);
	return buf;
}

void GuiNumericInput::_ApplyValue(double v, bool notify)
{
	_value = _Clamp(v);
	SetText(_Format(_value), notify);
}

bool GuiNumericInput::_AcceptChar(char c) const
{
	return (c >= '0' && c <= '9') || (c == '.') || (c == '-') || (c == '+');
}

void GuiNumericInput::_OnCommit()
{
	try
	{
		const double parsed = std::stod(_text);
		_ApplyValue(parsed, false);
	}
	catch (...)
	{
		// Parse failure: revert to the last valid value.
		SetText(_Format(_value), false);
	}

	GuiTextBox::_OnCommit();
}

ActionResult GuiNumericInput::OnAction(TouchAction action)
{
	if (TouchAction::TouchState::TOUCH_DOWN == action.State && HitTest(action.Position))
	{
		_dragging = true;
		_dragMoved = false;
		_dragStart = action.Position;
		_dragStartValue = _value;
	}
	else if (TouchAction::TouchState::TOUCH_UP == action.State)
	{
		_dragging = false;
	}

	return GuiTextBox::OnAction(action);
}

ActionResult GuiNumericInput::OnAction(TouchMoveAction action)
{
	if (_dragging)
	{
		const int dy = action.Position.Y - _dragStart.Y;
		if (std::abs(dy) > 2)
			_dragMoved = true;

		if (_dragMoved)
		{
			_ApplyValue(_dragStartValue + (double)dy * _step, true);
			return { true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
		}
	}

	return GuiTextBox::OnAction(action);
}
