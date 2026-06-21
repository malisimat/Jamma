#include "GuiScrollPanel.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"
#include <algorithm>

using namespace gui;
using namespace base;
using namespace actions;
using namespace utils;
using graphics::GlDrawContext;
using resources::ResourceLib;

GuiScrollBarParams GuiScrollPanel::_MakeScrollBarParams(const GuiScrollPanelParams& params)
{
	GuiScrollBarParams sb;
	sb.Texture = params.ScrollBarTexture;
	sb.TextureShader = params.TextureShader;
	sb.ThumbTexture = params.ThumbTexture;
	const int w = (int)params.ScrollBarWidth;
	const int h = (int)params.Size.Height;
	sb.Size = { (unsigned int)std::max(1, w), (unsigned int)std::max(1, h) };
	sb.Position = { std::max(0, (int)params.Size.Width - w), 0 };
	return sb;
}

GuiScrollPanel::GuiScrollPanel(GuiScrollPanelParams params) :
	GuiPanel(params),
	_content(nullptr),
	_scrollBar(std::make_shared<GuiScrollBar>(_MakeScrollBarParams(params))),
	_scrollBarWidth(params.ScrollBarWidth),
	_wheelStep(params.WheelStep),
	_scrollOffset(0),
	_draggingScrollBar(false)
{
	_scrollBar->SetOnScroll([this](double frac) { SetScrollFraction(frac); });
}

void GuiScrollPanel::SetContent(std::shared_ptr<base::GuiElement> content)
{
	_content = std::move(content);
	_scrollOffset = 0;
	_UpdateMetrics();
}

std::shared_ptr<base::GuiElement> GuiScrollPanel::Content() const { return _content; }

unsigned int GuiScrollPanel::ViewportWidth() const
{
	const int w = (int)GetSize().Width - (int)_scrollBarWidth;
	return (unsigned int)std::max(0, w);
}

unsigned int GuiScrollPanel::ViewportHeight() const { return GetSize().Height; }

unsigned int GuiScrollPanel::_ContentHeight() const
{
	return _content ? _content->GetSize().Height : 0u;
}

int GuiScrollPanel::MaxScrollOffset() const
{
	return std::max(0, (int)_ContentHeight() - (int)ViewportHeight());
}

int GuiScrollPanel::ScrollOffset() const { return _scrollOffset; }

void GuiScrollPanel::_ClampOffset()
{
	_scrollOffset = std::clamp(_scrollOffset, 0, MaxScrollOffset());
}

void GuiScrollPanel::SetScrollOffset(int offset)
{
	_scrollOffset = offset;
	_ClampOffset();

	const int maxOff = MaxScrollOffset();
	_scrollBar->SetValue(maxOff > 0 ? (double)_scrollOffset / (double)maxOff : 0.0);
}

void GuiScrollPanel::SetScrollFraction(double fraction)
{
	const int maxOff = MaxScrollOffset();
	_scrollOffset = (int)std::round(std::clamp(fraction, 0.0, 1.0) * (double)maxOff);
	_ClampOffset();
	_scrollBar->SetValue(maxOff > 0 ? (double)_scrollOffset / (double)maxOff : 0.0);
}

void GuiScrollPanel::_UpdateMetrics()
{
	_scrollBar->SetMetrics((double)ViewportHeight(), (double)_ContentHeight());
	_ClampOffset();
}

void GuiScrollPanel::SetSize(Size2d size)
{
	GuiElement::SetSize(size);
	_scrollBar->SetSize({ _scrollBarWidth, size.Height });
	_scrollBar->SetPosition({ std::max(0, (int)size.Width - (int)_scrollBarWidth), 0 });
	_UpdateMetrics();
}

void GuiScrollPanel::InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::InitResources(resourceLib, forceInit);
	if (_content)
		_content->InitResources(resourceLib, forceInit);
	_UpdateMetrics();
}

void GuiScrollPanel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiPanel::_InitResources(resourceLib, forceInit);
	_scrollBar->InitResources(resourceLib, forceInit);
}

void GuiScrollPanel::Draw(base::DrawContext& ctx)
{
	if (!_isVisible)
		return;

	GuiElement::Draw(ctx); // background

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3((float)pos.X, (float)pos.Y, 0.f)));

	// Content, shifted up by the current scroll offset.
	if (_content)
	{
		auto clipPos = GlobalPosition();
		clipPos.X += (int)_ContentClipPadding;
		clipPos.Y += (int)_ContentClipPadding;

		const int clipWidth = std::max(0, (int)ViewportWidth() - 2 * (int)_ContentClipPadding);
		const int clipHeight = std::max(0, (int)ViewportHeight() - 2 * (int)_ContentClipPadding);
		glCtx.PushScissorRect(clipPos, {
			(unsigned int)clipWidth,
			(unsigned int)clipHeight
		});

		glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(0.f, -(float)_scrollOffset, 0.f)));
		_content->Draw(ctx);
		glCtx.PopMvp();

		glCtx.PopScissorRect();
	}

	_scrollBar->Draw(ctx);

	glCtx.PopMvp();
}

ActionResult GuiScrollPanel::OnAction(TouchAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	// Mouse wheel (Window encodes wheel as TouchAction index 4, value = notches).
	if ((TouchAction::TouchState::TOUCH_DOWN == action.State) && (4 == action.Index) && HitTest(action.Position))
	{
		SetScrollOffset(_scrollOffset - action.Value * (int)_wheelStep);
		return {
			true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	// Scrollbar (the panel mediates so it stays the single active element for
	// the drag, keeping parent-relative coordinate transforms correct).
	auto sbAction = _scrollBar->ParentToLocal(action);
	auto sbRes = _scrollBar->OnAction(sbAction);
	if (sbRes.IsEaten)
	{
		_draggingScrollBar = (TouchAction::TouchState::TOUCH_DOWN == action.State);
		return {
			true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	// Content, with the scroll offset folded into the local coordinate.
	if (_content)
	{
		auto contentAction = action;
		contentAction.Position.Y += _scrollOffset;
		auto res = _content->OnAction(_content->ParentToLocal(contentAction));
		if (res.IsEaten)
		{
			if (!res.ActiveElement.lock())
				res.ActiveElement = std::static_pointer_cast<base::GuiElement>(shared_from_this());
			return res;
		}
	}

	return ActionResult::NoAction();
}

ActionResult GuiScrollPanel::OnAction(TouchMoveAction action)
{
	if (_draggingScrollBar)
		return _scrollBar->OnAction(_scrollBar->ParentToLocal(action));

	if (_content)
	{
		auto contentAction = action;
		contentAction.Position.Y += _scrollOffset;
		return _content->OnAction(_content->ParentToLocal(contentAction));
	}

	return ActionResult::NoAction();
}
