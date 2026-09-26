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
	_contentHost(nullptr),
	_scrollBar(std::make_shared<GuiScrollBar>(_MakeScrollBarParams(params))),
	_scrollBarWidth(params.ScrollBarWidth),
	_wheelStep(params.WheelStep),
	_scrollOffset(0),
	_draggingScrollBar(false)
{
	_scrollBar->SetOnScroll([this](double frac) { SetScrollFraction(frac); });
	_UpdateMetrics();
}

void GuiScrollPanel::SetContent(std::shared_ptr<base::GuiElement> content)
{
	_content = std::move(content);
	_contentHost.reset();
	if (_content)
	{
		base::GuiElementParams hostParams;
		hostParams.Size = _content->GetSize();
		hostParams.MinSize = hostParams.Size;
		hostParams.GuiPassThrough = true;
		_contentHost = std::make_shared<base::GuiElement>(hostParams);
		_contentHost->AddChild(_content);
		_contentHost->SetParent(shared_from_this());
	}
	_scrollOffset = 0;
	_UpdateContentHostPosition();
	_UpdateMetrics();
}

std::shared_ptr<base::GuiElement> GuiScrollPanel::Content() const { return _content; }

unsigned int GuiScrollPanel::ViewportWidth() const
{
	const int w = (int)GetSize().Width - (IsScrollBarVisible() ? (int)_scrollBarWidth : 0);
	return (unsigned int)std::max(0, w);
}

unsigned int GuiScrollPanel::ViewportHeight() const { return GetSize().Height; }

bool GuiScrollPanel::IsScrollBarVisible() const
{
	return _scrollBar && _scrollBar->IsVisible();
}

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
	_UpdateContentHostPosition();
}

void GuiScrollPanel::_UpdateContentHostPosition()
{
	if (_contentHost)
	{
		// Top-align content so the first item stays put as later items append.
		const int topAlignedPosition = static_cast<int>(ViewportHeight()) - static_cast<int>(_ContentHeight());
		_contentHost->SetPosition({ 0, topAlignedPosition + _scrollOffset });
	}
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
	if (_contentHost && _content)
		_contentHost->SetSize(_content->GetSize());
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
	if (_contentHost)
		_contentHost->InitResources(resourceLib, forceInit);
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

	_UpdateMetrics();

	GuiElement::Draw(ctx); // background

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3((float)pos.X, (float)pos.Y, 0.f)));

	if (_contentHost)
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

		_contentHost->Draw(ctx);

		glCtx.PopScissorRect();
	}

	_scrollBar->Draw(ctx);

	glCtx.PopMvp();
}

ActionResult GuiScrollPanel::OnAction(TouchAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	_UpdateMetrics();

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

	if (_contentHost && _IsInViewport(action.Position))
	{
		auto res = _contentHost->OnAction(_contentHost->ParentToLocal(action));
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

	if (_contentHost && _IsInViewport(action.Position))
		return _contentHost->OnAction(_contentHost->ParentToLocal(action));

	return ActionResult::NoAction();
}

bool GuiScrollPanel::RouteHitTest(Position2d localPos)
{
	if (!_isEnabled || !_isVisible)
		return false;

	return _HitTest(localPos);
}

std::shared_ptr<base::GuiElement> GuiScrollPanel::FindTopmostDescendant(Position2d localPos)
{
	if (!RouteHitTest(localPos))
		return nullptr;

	auto scrollBarLocal = _scrollBar->ParentToLocal(localPos);
	auto scrollBarHit = _scrollBar->FindTopmostDescendant(scrollBarLocal);
	if (scrollBarHit)
		return scrollBarHit;

	if (_contentHost && _IsInViewport(localPos))
	{
		auto contentLocal = _contentHost->ParentToLocal(localPos);
		auto contentHit = _contentHost->FindTopmostDescendant(contentLocal);
		if (contentHit)
			return contentHit;
	}

	if (_guiParams.GuiPassThrough || !_HitTest(localPos))
		return nullptr;

	return std::static_pointer_cast<base::GuiElement>(shared_from_this());
}

void GuiScrollPanel::ClearPointerState()
{
	GuiPanel::ClearPointerState();
	if (_contentHost)
		_contentHost->ClearPointerState();
	if (_scrollBar)
		_scrollBar->ClearPointerState();
}

bool GuiScrollPanel::_IsInViewport(Position2d localPos) const
{
	const int minX = static_cast<int>(_ContentClipPadding);
	const int minY = static_cast<int>(_ContentClipPadding);
	const int maxX = std::max(minX, static_cast<int>(ViewportWidth()) - static_cast<int>(_ContentClipPadding));
	const int maxY = std::max(minY, static_cast<int>(ViewportHeight()) - static_cast<int>(_ContentClipPadding));

	return (localPos.X >= minX)
		&& (localPos.X < maxX)
		&& (localPos.Y >= minY)
		&& (localPos.Y < maxY);
}
