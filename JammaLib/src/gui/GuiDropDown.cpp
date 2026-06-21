#include "GuiDropDown.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"
#include "../actions/GuiAction.h"
#include <algorithm>

using namespace gui;
using namespace base;
using namespace actions;
using namespace utils;
using graphics::GlDrawContext;
using resources::ResourceLib;

GuiElementParams GuiDropDownList::_MakeQuadParams(const std::string& texture)
{
	GuiElementParams p(0,
		DrawableParams{ "" },
		MoveableParams(Position2d{ 0, 0 }, Position3d{ 0, 0, 0 }, 1.0),
		SizeableParams{ 1, 1 },
		"", "", "", {});
	p.Texture = texture;
	p.GuiPassThrough = true;
	return p;
}

std::shared_ptr<GuiLabel> GuiDropDownList::_MakeRowLabel(const std::string& text,
	int y,
	unsigned int width,
	unsigned int rowHeight,
	unsigned int padding)
{
	GuiLabelParams lp;
	lp.String = text;
	const unsigned int pad = std::min(padding, rowHeight / 2u);
	const unsigned int textHeight = ResourceLib::ResolveTextPixelHeightFromControlBox(rowHeight, pad);
	lp.Position = { (int)pad, y + (int)pad };
	lp.Size = { std::max(1u, width > 2u * pad ? width - 2u * pad : 1u), textHeight };
	lp.MinSize = { 1u, textHeight };
	return std::make_shared<GuiLabel>(lp);
}

// ===========================================================================
// GuiDropDownList
// ===========================================================================

GuiDropDownList::GuiDropDownList(GuiElementParams params,
	std::vector<std::string> items,
	unsigned int rowHeight,
	unsigned int padding,
	const std::string& highlightTexture) :
	GuiElement(params),
	_items(std::move(items)),
	_rowHeight(rowHeight),
	_highlight(-1),
	_highlightQuad(_MakeQuadParams(highlightTexture)),
	_onSelect()
{
	int y = 0;
	for (const auto& item : _items)
	{
		_rowLabels.push_back(_MakeRowLabel(item, y, params.Size.Width, _rowHeight, padding));
		y += (int)_rowHeight;
	}
}

void GuiDropDownList::SetOnSelect(std::function<void(int)> onSelect)
{
	_onSelect = std::move(onSelect);
}

void GuiDropDownList::SetHighlight(int index)
{
	if (_items.empty())
	{
		_highlight = -1;
		return;
	}
	_highlight = std::clamp(index, 0, (int)_items.size() - 1);
}

int GuiDropDownList::Highlight() const { return _highlight; }

int GuiDropDownList::_RowFromLocalY(int localY) const
{
	if (_rowHeight == 0u || _items.empty())
		return -1;

	const int row = localY / (int)_rowHeight;
	if (row < 0 || row >= (int)_items.size())
		return -1;

	return row;
}

void GuiDropDownList::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);
	_highlightQuad.InitResources(resourceLib, forceInit);
	for (auto& label : _rowLabels)
		label->InitResources(resourceLib, forceInit);
}

void GuiDropDownList::Draw(base::DrawContext& ctx)
{
	if (!_isVisible)
		return;

	GuiElement::Draw(ctx); // background

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3((float)pos.X, (float)pos.Y, 0.f)));

	if (_highlight >= 0 && _highlight < (int)_items.size())
	{
		_highlightQuad.SetSize({ GetSize().Width, _rowHeight });
		_highlightQuad.SetPosition({ 0, _highlight * (int)_rowHeight });
		_highlightQuad.Draw(ctx);
	}

	for (auto& label : _rowLabels)
		label->Draw(ctx);

	glCtx.PopMvp();
}

ActionResult GuiDropDownList::OnAction(TouchAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	const int row = _RowFromLocalY(action.Position.Y);

	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
	{
		if (row >= 0)
			_highlight = row;
		return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
	}

	if (TouchAction::TouchState::TOUCH_UP == action.State)
	{
		if (row >= 0 && _onSelect)
			_onSelect(row);
		return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
	}

	return ActionResult::NoAction();
}

ActionResult GuiDropDownList::OnAction(TouchMoveAction action)
{
	const int row = _RowFromLocalY(action.Position.Y);
	if (row >= 0)
		_highlight = row;

	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
}

ActionResult GuiDropDownList::OnAction(KeyAction action)
{
	if (KeyAction::KEY_DOWN != action.KeyActionType)
		return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };

	switch (action.KeyChar)
	{
	case 0x28: SetHighlight(_highlight + 1); break;  // VK_DOWN
	case 0x26: SetHighlight(_highlight - 1); break;  // VK_UP
	case 0x0D:                                        // VK_RETURN
		if (_highlight >= 0 && _onSelect)
			_onSelect(_highlight);
		break;
	default:
		break;
	}

	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
}

// ===========================================================================
// GuiDropDown
// ===========================================================================

std::shared_ptr<GuiLabel> GuiDropDown::_MakeClosedLabel(const GuiDropDownParams& params)
{
	GuiLabelParams lp;
	lp.String = (params.InitIndex < params.Items.size()) ? params.Items[params.InitIndex] : std::string();
	const unsigned int pad = std::min(params.Padding, params.Size.Height / 2u);
	const unsigned int textHeight = ResourceLib::ResolveTextPixelHeightFromControlBox(params.Size.Height, pad);
	lp.Position = { (int)pad, (int)pad };
	const int w = std::max(1, (int)params.Size.Width - (int)(2u * pad));
	const int h = (int)textHeight;
	lp.Size = { (unsigned int)w, (unsigned int)h };
	lp.MinSize = { 1u, (unsigned int)h };
	return std::make_shared<GuiLabel>(lp);
}

std::shared_ptr<GuiDropDownList> GuiDropDown::_MakeList(const GuiDropDownParams& params)
{
	GuiElementParams lp(0,
		DrawableParams{ "" },
		MoveableParams(Position2d{ 0, 0 }, Position3d{ 0, 0, 0 }, 1.0),
		SizeableParams{ params.Size.Width, std::max(1u, params.RowHeight * (unsigned int)params.Items.size()) },
		"", "", "", {});
	lp.Texture = params.ListTexture;
	lp.TextureShader = params.TextureShader;
	lp.GuiPassThrough = false;
	return std::make_shared<GuiDropDownList>(lp,
		params.Items,
		params.RowHeight,
		params.Padding,
		params.HighlightTexture);
}

GuiDropDown::GuiDropDown(GuiDropDownParams params) :
	GuiElement(params),
	_items(params.Items),
	_selectedIndex(params.Items.empty() ? -1 : (int)std::min((size_t)params.InitIndex, params.Items.size() - 1)),
	_rowHeight(params.RowHeight),
	_padding(params.Padding),
	_label(_MakeClosedLabel(params)),
	_list(_MakeList(params)),
	_popupHost(nullptr),
	_open(false),
	_receiver(params.Receiver)
{
	_list->SetOnSelect([this](int index) { _Select(index, true); });
}

void GuiDropDown::SetPopupHost(GuiPopupHost* host) { _popupHost = host; }
int GuiDropDown::SelectedIndex() const { return _selectedIndex; }

std::string GuiDropDown::SelectedText() const
{
	return (_selectedIndex >= 0 && _selectedIndex < (int)_items.size()) ? _items[_selectedIndex] : std::string();
}

bool GuiDropDown::IsOpen() const { return _open; }
bool GuiDropDown::WantsFocusOnPress() const { return true; }

void GuiDropDown::SetSelectedIndex(int index, bool notify)
{
	if (_items.empty())
		return;
	_Select(std::clamp(index, 0, (int)_items.size() - 1), notify);
}

void GuiDropDown::_SyncLabel()
{
	_label->SetString(SelectedText());
}

void GuiDropDown::_Select(int index, bool notify)
{
	_selectedIndex = std::clamp(index, 0, (int)_items.size() - 1);
	_SyncLabel();
	Close();

	if (notify)
	{
		if (auto receiver = _receiver.lock())
		{
			GuiAction action;
			action.Index = _index;
			action.ElementType = GuiAction::ACTIONELEMENT_RADIO;
			action.Data = GuiAction::GuiInt{ _selectedIndex };
			receiver->OnAction(action);
		}
	}
}

void GuiDropDown::Open()
{
	if (_open || _items.empty() || nullptr == _popupHost)
		return;

	// Position the list directly beneath the closed control in global coords.
	auto global = GlobalPosition();
	_list->SetPosition({ global.X, global.Y + (int)GetSize().Height });
	_list->SetHighlight(_selectedIndex < 0 ? 0 : _selectedIndex);
	_popupHost->Open(_list, shared_from_this());
	_open = true;
}

void GuiDropDown::Close()
{
	if (!_open)
		return;

	if (_popupHost && _popupHost->Top() == _list)
		_popupHost->Close();

	_open = false;
}

void GuiDropDown::SetSize(Size2d size)
{
	GuiElement::SetSize(size);
	const unsigned int pad = std::min(_padding, size.Height / 2u);
	const unsigned int textHeight = ResourceLib::ResolveTextPixelHeightFromControlBox(size.Height, pad);
	const int w = std::max(1, (int)size.Width - (int)(2u * pad));
	const int h = (int)textHeight;
	_label->SetSize({ (unsigned int)w, (unsigned int)h });
}

void GuiDropDown::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);
	_label->InitResources(resourceLib, forceInit);
	_list->InitResources(resourceLib, forceInit);
}

void GuiDropDown::Draw(base::DrawContext& ctx)
{
	if (!_isVisible)
		return;

	GuiElement::Draw(ctx); // background

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3((float)pos.X, (float)pos.Y, 0.f)));
	_label->Draw(ctx);
	glCtx.PopMvp();
}

ActionResult GuiDropDown::OnAction(TouchAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State) && HitTest(action.Position))
	{
		_state = STATE_DOWN;
		return {
			true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	if ((TouchAction::TouchState::TOUCH_UP == action.State) && HitTest(action.Position))
	{
		_state = STATE_NORMAL;
		_open ? Close() : Open();
		return {
			true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	return ActionResult::NoAction();
}

ActionResult GuiDropDown::OnAction(KeyAction action)
{
	if (!_isEnabled || !_isVisible || !_hasFocus)
		return ActionResult::NoAction();

	if ((KeyAction::KEY_UP == action.KeyActionType) && !_open
		&& ((13 == action.KeyChar) || (32 == action.KeyChar) || (0x28 == action.KeyChar)))
	{
		Open();
		return { true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
	}

	return ActionResult::NoAction();
}
