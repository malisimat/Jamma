#include "GuiScrollBar.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"
#include <algorithm>
#include <cmath>

using namespace gui;
using namespace base;
using namespace actions;
using namespace utils;
using graphics::GlDrawContext;
using resources::ResourceLib;

GuiElementParams GuiScrollBar::_MakeThumbParams(const GuiScrollBarParams& params)
{
	GuiElementParams p(0,
		DrawableParams{ "" },
		MoveableParams(Position2d{ 0, 0 }, Position3d{ 0, 0, 0 }, 1.0),
		SizeableParams{ params.Size.Width, params.MinThumb },
		"", "", "", {});
	p.Texture = params.ThumbTexture;
	p.GuiPassThrough = false;
	return p;
}

GuiScrollBar::GuiScrollBar(GuiScrollBarParams params) :
	GuiElement(params),
	_value(0.0),
	_viewportLength(1.0),
	_contentLength(1.0),
	_minThumb(params.MinThumb),
	_thumb(_MakeThumbParams(params)),
	_dragging(false),
	_dragStartY(0),
	_dragStartValue(0.0),
	_onScroll()
{
	_UpdateThumb();
}

double GuiScrollBar::ThumbFraction(double viewport, double content)
{
	if (content <= 0.0 || viewport >= content)
		return 1.0;
	return std::clamp(viewport / content, 0.0, 1.0);
}

unsigned int GuiScrollBar::ThumbLength(unsigned int track, double viewport, double content, unsigned int minThumb)
{
	const double frac = ThumbFraction(viewport, content);
	unsigned int len = (unsigned int)std::round((double)track * frac);
	len = std::max(len, minThumb);
	return std::min(len, track);
}

int GuiScrollBar::ThumbOffset(unsigned int track, unsigned int thumbLength, double value)
{
	if (thumbLength >= track)
		return 0;
	const double v = std::clamp(value, 0.0, 1.0);
	return (int)std::round((double)(track - thumbLength) * v);
}

double GuiScrollBar::ValueFromOffset(unsigned int track, unsigned int thumbLength, int offset)
{
	if (thumbLength >= track)
		return 0.0;
	const double v = (double)offset / (double)(track - thumbLength);
	return std::clamp(v, 0.0, 1.0);
}

void GuiScrollBar::SetMetrics(double viewportLength, double contentLength)
{
	_viewportLength = std::max(0.0, viewportLength);
	_contentLength = std::max(0.0, contentLength);
	_UpdateThumb();
}

double GuiScrollBar::Value() const { return _value; }

void GuiScrollBar::SetValue(double value)
{
	_value = std::clamp(value, 0.0, 1.0);
	_UpdateThumb();
}

void GuiScrollBar::SetOnScroll(std::function<void(double)> onScroll)
{
	_onScroll = std::move(onScroll);
}

bool GuiScrollBar::IsScrollable() const
{
	return _contentLength > _viewportLength;
}

void GuiScrollBar::SetSize(Size2d size)
{
	GuiElement::SetSize(size);
	_UpdateThumb();
}

void GuiScrollBar::_UpdateThumb()
{
	const unsigned int track = GetSize().Height;
	const unsigned int thumbLen = ThumbLength(track, _viewportLength, _contentLength, _minThumb);
	const int offset = ThumbOffset(track, thumbLen, _value);

	_thumb.SetSize({ GetSize().Width, thumbLen });
	_thumb.SetPosition({ 0, offset });
}

void GuiScrollBar::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);
	_thumb.InitResources(resourceLib, forceInit);
}

void GuiScrollBar::Draw(base::DrawContext& ctx)
{
	if (!_isVisible)
		return;

	GuiElement::Draw(ctx); // track background

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3((float)pos.X, (float)pos.Y, 0.f)));
	_thumb.Draw(ctx);
	glCtx.PopMvp();
}

ActionResult GuiScrollBar::OnAction(TouchAction action)
{
	if (!_isEnabled || !_isVisible || !IsScrollable())
		return ActionResult::NoAction();

	if (TouchAction::TouchState::TOUCH_DOWN == action.State && HitTest(action.Position))
	{
		_dragging = true;
		_dragStartY = action.Position.Y;
		_dragStartValue = _value;
		return {
			true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	if (TouchAction::TouchState::TOUCH_UP == action.State && _dragging)
	{
		_dragging = false;
		return {
			true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	return ActionResult::NoAction();
}

ActionResult GuiScrollBar::OnAction(TouchMoveAction action)
{
	if (!_dragging)
		return ActionResult::NoAction();

	const unsigned int track = GetSize().Height;
	const unsigned int thumbLen = ThumbLength(track, _viewportLength, _contentLength, _minThumb);
	const int startOffset = ThumbOffset(track, thumbLen, _dragStartValue);
	const int dy = action.Position.Y - _dragStartY;

	_value = ValueFromOffset(track, thumbLen, startOffset + dy);
	_UpdateThumb();

	if (_onScroll)
		_onScroll(_value);

	return { true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
}
