#include "GuiToggle.h"
using namespace graphics;

using namespace base;
using namespace gui;
using namespace actions;
using namespace resources;
using namespace utils;

GuiToggle::GuiToggle(GuiToggleParams params) :
	GuiButton(params),
	_toggleIndex(params.ToggleIndex),
	_toggleState(params.InitState),
	_toggledTexture(ImageParams(DrawableParams{ params.ToggledTexture }, SizeableParams{ params.Size,params.MinSize }, params.TextureShader, params.Rot90, params.FlipH, params.FlipV, params.TextureOpacity)),
	_toggledOverTexture(ImageParams(DrawableParams{ params.ToggledOverTexture }, SizeableParams{ params.Size,params.MinSize }, params.TextureShader, params.Rot90, params.FlipH, params.FlipV, params.TextureOpacity)),
	_toggledDownTexture(ImageParams(DrawableParams{ params.ToggledDownTexture }, SizeableParams{ params.Size,params.MinSize }, params.TextureShader, params.Rot90, params.FlipH, params.FlipV, params.TextureOpacity)),
	_toggledOutTexture(ImageParams(DrawableParams{ params.ToggledOutTexture }, SizeableParams{ params.Size,params.MinSize }, params.TextureShader, params.Rot90, params.FlipH, params.FlipV, params.TextureOpacity)),
	_buttonParams(params)
{
}

void GuiToggle::SetSize(Size2d size)
{
	GuiButton::SetSize(size);

	_toggledTexture.SetSize(_sizeParams.Size);
	_toggledOverTexture.SetSize(_sizeParams.Size);
	_toggledDownTexture.SetSize(_sizeParams.Size);
	_toggledOutTexture.SetSize(_sizeParams.Size);
}

Image& GuiToggle::_CurrentTexture()
{
	if (_toggleState == GuiToggleParams::TOGGLE_ON)
		return _ResolveStateTexture(_state, _toggledTexture, _toggledOverTexture, _toggledDownTexture, _toggledOutTexture);
	// An untoggled drag outside retains the normal appearance.
	return _state == STATE_OUT ? _texture : GuiElement::_CurrentTexture();
}

ActionResult GuiToggle::OnAction(TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	if (res.IsEaten)
	{
		ActionResultType resultType = ACTIONRESULT_DEFAULT;
		std::string source = "";

		if (TouchAction::TouchState::TOUCH_UP == action.State)
		{
			_toggleState = GuiToggleParams::TOGGLE_ON == _toggleState ? GuiToggleParams::TOGGLE_OFF : GuiToggleParams::TOGGLE_ON;
			_state = STATE_NORMAL;
			source = std::to_string(_index);
			resultType = ACTIONRESULT_TOGGLE;

			_OnToggleChange(false);
		}

		return {
			true,
			source,
			"",
			resultType,
			nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	return ActionResult::NoAction();
}

ActionResult GuiToggle::OnAction(KeyAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	if (_hasFocus
		&& (KeyAction::KEY_UP == action.KeyActionType)
		&& ((13 == action.KeyChar) || (32 == action.KeyChar)))
	{
		_toggleState = GuiToggleParams::TOGGLE_ON == _toggleState ?
			GuiToggleParams::TOGGLE_OFF : GuiToggleParams::TOGGLE_ON;
		_state = STATE_NORMAL;
		_OnToggleChange(false);

		return {
			true,
			std::to_string(_index),
			"",
			ACTIONRESULT_TOGGLE,
			nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	return GuiElement::OnAction(action);
}

GuiToggleParams::ToggleState GuiToggle::Toggle()
{
	switch (_toggleState)
	{
	case GuiToggleParams::TOGGLE_OFF:
		_toggleState = GuiToggleParams::TOGGLE_ON;
		break;
	case GuiToggleParams::TOGGLE_ON:
		_toggleState = GuiToggleParams::TOGGLE_OFF;
		break;
	}

	_OnToggleChange(false);

	return _toggleState;
}

GuiToggleParams::ToggleState GuiToggle::GetToggleState() const
{
	return _toggleState;
}

void GuiToggle::SetToggleState(GuiToggleParams::ToggleState state, bool bypassUpdates)
{
	_toggleState = state;

	_OnToggleChange(bypassUpdates);
}

void GuiToggle::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_toggledTexture.InitResources(resourceLib, forceInit);
	_toggledOverTexture.InitResources(resourceLib, forceInit);
	_toggledDownTexture.InitResources(resourceLib, forceInit);
	_toggledOutTexture.InitResources(resourceLib, forceInit);

	GuiElement::_InitResources(resourceLib, forceInit);
}

void GuiToggle::_ReleaseResources()
{
	GuiElement::_ReleaseResources();

	_toggledTexture.ReleaseResources();
	_toggledOverTexture.ReleaseResources();
	_toggledDownTexture.ReleaseResources();
	_toggledOutTexture.ReleaseResources();
}

void GuiToggle::_OnToggleChange(bool bypassUpdates)
{
	if (_receiver && !bypassUpdates)
	{
		GuiAction action;
		action.ElementType = GuiAction::ACTIONELEMENT_TOGGLE;
		action.Index = _toggleIndex;
		action.Data = GuiAction::GuiInt(_toggleState);
		_receiver->OnAction(action);
	}
}
