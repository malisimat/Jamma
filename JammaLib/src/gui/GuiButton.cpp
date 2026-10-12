#include "GuiButton.h"
#include <algorithm>

using namespace base;
using namespace gui;
using namespace utils;

GuiLabelParams GuiButton::_MakeLabelParams(const GuiButtonParams& params)
{
	GuiLabelParams lp;
	lp.String = params.Text;
	lp.VerticalAlign = GuiTextVerticalAlign::Center;
	lp.ClipText = true;

	const GuiTextFrame frame = GuiLabelParams::ResolveTextFrame(
		params.Size.Width,
		params.Size.Height,
		params.TextPadding,
		params.TextPadding,
		true);

	lp.Position = { (int)frame.PaddingX, frame.OffsetY };
	lp.Size = { frame.ContentWidth, frame.TextHeight };
	lp.MinSize = { 1u, frame.TextHeight };
	lp.CenterHorizontally = true;
	return lp;
}

GuiButton::GuiButton(GuiButtonParams params) :
	_buttonParams(params),
	GuiElement(params),
	_label(params.Text.empty() ? nullptr : std::make_shared<GuiLabel>(_MakeLabelParams(params)))
{
	if (_label)
		_children.push_back(_label);
}

actions::ActionResult GuiButton::OnAction(actions::TouchAction action)
{
	const bool activate = _pressed && action.State == actions::TouchAction::TOUCH_UP && HitTest(action.Position) && _isEnabled && _isVisible;
	if (action.State == actions::TouchAction::TOUCH_DOWN) _pressed = _isEnabled && _isVisible && HitTest(action.Position);
	else _pressed = false;
	auto result = GuiElement::OnAction(action);
	if (activate && _buttonParams.DispatchCommand && GetReceiver())
	{
		actions::GuiAction command; command.Index = _index; command.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON; command.InputModifiers = action.Modifiers;
		GetReceiver()->OnAction(command);
	}
	if (result.IsEaten && action.State == actions::TouchAction::TOUCH_DOWN && !result.ActiveElement.lock())
		result.ActiveElement = std::static_pointer_cast<base::GuiElement>(shared_from_this());
	return result;
}

void GuiButton::SetSize(Size2d size)
{
	GuiElement::SetSize(size);

	if (!_label)
		return;

	const GuiTextFrame frame = GuiLabelParams::ResolveTextFrame(
		size.Width,
		size.Height,
		_buttonParams.TextPadding,
		_buttonParams.TextPadding,
		true);

	_label->SetPosition({ (int)frame.PaddingX, frame.OffsetY });
	_label->SetSize({ frame.ContentWidth, frame.TextHeight });
}

void GuiButton::SetText(const std::string& text)
{
	_buttonParams.Text = text;

	if (!_label)
	{
		if (text.empty())
			return;

		_label = std::make_shared<GuiLabel>(_MakeLabelParams(_buttonParams));
		_children.push_back(_label);
		return;
	}

	if (text.empty())
	{
		auto it = std::find(_children.begin(), _children.end(), _label);
		if (it != _children.end())
			_children.erase(it);
		_label.reset();
		return;
	}

	_label->SetString(text);

	const GuiTextFrame frame = GuiLabelParams::ResolveTextFrame(
		_sizeParams.Size.Width,
		_sizeParams.Size.Height,
		_buttonParams.TextPadding,
		_buttonParams.TextPadding,
		true);

	_label->SetPosition({ (int)frame.PaddingX, frame.OffsetY });
	_label->SetSize({ frame.ContentWidth, frame.TextHeight });
}

actions::ActionResult GuiButton::OnAction(actions::KeyAction action)
{
	auto result = GuiElement::OnAction(action);
	if (_buttonParams.DispatchCommand && result.IsEaten && result.ResultType == actions::ACTIONRESULT_ACTIVATE && GetReceiver())
	{
		actions::GuiAction command; command.Index = _index; command.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON; command.InputModifiers = action.Modifiers;
		GetReceiver()->OnAction(command);
	}
	return result;
}

void GuiButton::ClearPointerState()
{
	_pressed = false;
	GuiElement::ClearPointerState();
}
