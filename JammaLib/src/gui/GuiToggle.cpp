#include "GuiToggle.h"
using namespace graphics;

using namespace base;
using namespace gui;
using namespace actions;
using namespace resources;
using namespace utils;

GuiToggle::GuiToggle(GuiToggleParams params) :
	_toggleState(params.InitState),
	_toggledTexture(ImageParams(DrawableParams{ params.ToggledTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_toggledOverTexture(ImageParams(DrawableParams{ params.ToggledOverTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_toggledDownTexture(ImageParams(DrawableParams{ params.ToggledDownTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_toggledOutTexture(ImageParams(DrawableParams{ params.ToggledOutTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_buttonParams(params),
	GuiElement(params)
{
}

void GuiToggle::SetSize(Size2d size)
{
	GuiElement::SetSize(size);

	_toggledTexture.SetSize(_sizeParams.Size);
	_toggledOverTexture.SetSize(_sizeParams.Size);
	_toggledDownTexture.SetSize(_sizeParams.Size);
	_toggledOutTexture.SetSize(_sizeParams.Size);
}

void GuiToggle::Draw(DrawContext& ctx)
{
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)));

	bool hasDrawn = false;

	switch (_state)
	{
	case STATE_OVER:
		switch (_toggleState)
		{
		case GuiToggleParams::TOGGLE_ON:
			if (_toggledOverTexture.IsDrawInitialised())
			{
				_toggledOverTexture.Draw(ctx);
				hasDrawn = true;
			}
			break;
		default:
			if (_overTexture.IsDrawInitialised())
			{
				_overTexture.Draw(ctx);
				hasDrawn = true;
			}
			break;
		}

		break;
	case STATE_DOWN:
		switch (_toggleState)
		{
		case GuiToggleParams::TOGGLE_ON:
			if (_toggledDownTexture.IsDrawInitialised())
			{
				_toggledDownTexture.Draw(ctx);
				hasDrawn = true;
			}
			break;
		default:
			if (_downTexture.IsDrawInitialised())
			{
				_downTexture.Draw(ctx);
				hasDrawn = true;
			}
			break;
		}

		break;
	case STATE_OUT:
		switch (_toggleState)
		{
		case GuiToggleParams::TOGGLE_ON:
			if (_toggledOutTexture.IsDrawInitialised())
			{
				_toggledOutTexture.Draw(ctx);
				hasDrawn = true;
			}
			break;
		default:
			if (_outTexture.IsDrawInitialised())
			{
				_outTexture.Draw(ctx);
				hasDrawn = true;
			}
			break;
		}

		break;
	}

	if (!hasDrawn)
	{
		switch (_toggleState)
		{
		case GuiToggleParams::TOGGLE_ON:
			_toggledTexture.Draw(ctx);
			break;
		default:
			_texture.Draw(ctx);
			break;
		}
	}

	for (auto& child : _children)
		child->Draw(ctx);

	glCtx.PopMvp();
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
		action.Index = _index;
		action.Data = GuiAction::GuiInt(_toggleState);
		_receiver->OnAction(action);
	}
}