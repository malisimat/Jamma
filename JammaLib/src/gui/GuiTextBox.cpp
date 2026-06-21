#include "GuiTextBox.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"
#include "../actions/GuiAction.h"
#include <algorithm>
#include <cmath>

using namespace gui;
using namespace base;
using namespace actions;
using namespace utils;
using graphics::Font;
using graphics::GlDrawContext;
using resources::ResourceLib;

GuiElementParams GuiTextBox::_MakeCaretParams(const std::string& texture)
{
	GuiElementParams p(0,
		base::DrawableParams{ "" },
		base::MoveableParams(Position2d{ 0, 0 }, Position3d{ 0, 0, 0 }, 1.0),
		base::SizeableParams{ 2, 2 },
		"", "", "", {});
	p.Texture = texture;
	p.GuiPassThrough = true;
	return p;
}

GuiLabelParams GuiTextBox::_MakeLabelParams(const GuiTextBoxParams& params)
{
	GuiLabelParams lp;
	lp.String = params.Text;
	const GuiTextFrame frame = GuiLabelParams::ResolveTextFrame(
		params.Size.Width,
		params.Size.Height,
		params.Padding,
		params.Padding,
		true);
	lp.Position = { (int)frame.PaddingX, frame.OffsetY };
	lp.Size = { frame.ContentWidth, frame.TextHeight };
	lp.MinSize = { 1u, frame.TextHeight };
	return lp;
}

GuiTextBox::GuiTextBox(GuiTextBoxParams params) :
	GuiElement(params),
	_text(params.Text),
	_caret((unsigned int)params.Text.size()),
	_anchor((unsigned int)params.Text.size()),
	_padding(params.Padding),
	_editing(false),
	_label(std::make_shared<GuiLabel>(_MakeLabelParams(params))),
	_caretQuad(_MakeCaretParams(params.CaretTexture)),
	_font(),
	_receiver(params.Receiver)
{
}

const std::string& GuiTextBox::Text() const
{
	return _text;
}

void GuiTextBox::SetText(const std::string& text, bool notify)
{
	_text = text;
	_caret = (unsigned int)_text.size();
	_anchor = _caret;
	_SyncLabel();

	if (notify)
		_OnTextChanged();
}

unsigned int GuiTextBox::CaretIndex() const { return _caret; }
bool GuiTextBox::HasSelection() const { return _caret != _anchor; }
unsigned int GuiTextBox::SelectionStart() const { return std::min(_caret, _anchor); }
unsigned int GuiTextBox::SelectionLength() const { return std::max(_caret, _anchor) - std::min(_caret, _anchor); }

std::string GuiTextBox::SelectedText() const
{
	return HasSelection() ? _text.substr(SelectionStart(), SelectionLength()) : std::string();
}

bool GuiTextBox::IsTextEditing() const { return _hasFocus; }
bool GuiTextBox::WantsFocusOnPress() const { return true; }

void GuiTextBox::SetSize(Size2d size)
{
	GuiElement::SetSize(size);
	const GuiTextFrame frame = GuiLabelParams::ResolveTextFrame(
		size.Width,
		size.Height,
		_padding,
		_padding,
		true);
	_label->SetPosition({ (int)frame.PaddingX, frame.OffsetY });
	_label->SetSize({ frame.ContentWidth, frame.TextHeight });
}

// --------------------------------------------------------------------------
// Editing primitives
// --------------------------------------------------------------------------

bool GuiTextBox::_DeleteSelection()
{
	if (!HasSelection())
		return false;

	const unsigned int start = SelectionStart();
	const unsigned int len = SelectionLength();
	_text.erase(start, len);
	_caret = start;
	_anchor = start;
	return true;
}

bool GuiTextBox::_AcceptChar(char c) const
{
	return (c >= 32) && (c < 127);
}

void GuiTextBox::_InsertChar(char c)
{
	if (!_AcceptChar(c))
		return;

	_DeleteSelection();
	_text.insert(_text.begin() + _caret, c);
	_caret++;
	_anchor = _caret;
}

void GuiTextBox::_Backspace()
{
	if (_DeleteSelection())
		return;

	if (_caret > 0)
	{
		_text.erase(_text.begin() + (_caret - 1));
		_caret--;
		_anchor = _caret;
	}
}

void GuiTextBox::_Delete()
{
	if (_DeleteSelection())
		return;

	if (_caret < _text.size())
		_text.erase(_text.begin() + _caret);
}

void GuiTextBox::_SetCaret(unsigned int index, bool select)
{
	_caret = std::min(index, (unsigned int)_text.size());
	if (!select)
		_anchor = _caret;
}

void GuiTextBox::_MoveCaret(int delta, bool select)
{
	int next = (int)_caret + delta;
	next = std::clamp(next, 0, (int)_text.size());
	_SetCaret((unsigned int)next, select);
}

unsigned int GuiTextBox::_CaretFromLocalX(int localX) const
{
	auto font = _font.lock();
	if (!font || _text.empty())
		return (unsigned int)_text.size();
	const float target = (float)(localX - (int)_padding);

	unsigned int best = 0;
	float bestDist = std::abs(target);
	for (unsigned int i = 1; i <= _text.size(); ++i)
	{
		const float x = font->MeasureString(_text.substr(0, i));
		const float dist = std::abs(x - target);
		if (dist < bestDist)
		{
			bestDist = dist;
			best = i;
		}
	}
	return best;
}

void GuiTextBox::_SyncLabel()
{
	_label->SetString(_text);
}

// --------------------------------------------------------------------------
// Notification hooks
// --------------------------------------------------------------------------

void GuiTextBox::_NotifyReceiver(bool commit)
{
	auto receiver = _receiver.lock();
	if (!receiver)
		return;

	GuiAction action;
	action.Index = _index;
	action.ElementType = GuiAction::ACTIONELEMENT_RACK; // generic text payload
	action.Data = GuiAction::GuiString{ _text };
	receiver->OnAction(action);
}

void GuiTextBox::_OnTextChanged()
{
	_SyncLabel();
	_NotifyReceiver(false);
}

void GuiTextBox::_OnCommit()
{
	_NotifyReceiver(true);
}

// --------------------------------------------------------------------------
// Input
// --------------------------------------------------------------------------

ActionResult GuiTextBox::OnAction(TouchAction action)
{
	if (!_isEnabled || !_isVisible)
		return ActionResult::NoAction();

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State) && HitTest(action.Position))
	{
		_state = STATE_DOWN;
		const bool select = (Action::MODIFIER_SHIFT & action.Modifiers) != 0;
		_SetCaret(_CaretFromLocalX(action.Position.X), select);

		return {
			true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr,
			std::static_pointer_cast<base::GuiElement>(shared_from_this())
		};
	}

	if (TouchAction::TouchState::TOUCH_UP == action.State)
	{
		_state = STATE_NORMAL;
		if (HitTest(action.Position))
			return {
				true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr,
				std::static_pointer_cast<base::GuiElement>(shared_from_this())
			};
	}

	return ActionResult::NoAction();
}

ActionResult GuiTextBox::OnAction(KeyAction action)
{
	if (!_isEnabled || !_isVisible || !_hasFocus)
		return ActionResult::NoAction();

	// Editing happens on key-down so the matching key-up is simply swallowed
	// (the scene suppresses global shortcuts while IsTextEditing()).
	if (KeyAction::KEY_DOWN != action.KeyActionType)
		return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };

	const bool shift = (Action::MODIFIER_SHIFT & action.Modifiers) != 0;
	const std::string before = _text;
	bool committed = false;

	switch (action.KeyChar)
	{
	case 0x08: _Backspace(); break;                 // VK_BACK
	case 0x2E: _Delete(); break;                     // VK_DELETE
	case 0x25: _MoveCaret(-1, shift); break;         // VK_LEFT
	case 0x27: _MoveCaret(+1, shift); break;         // VK_RIGHT
	case 0x24: _SetCaret(0, shift); break;           // VK_HOME
	case 0x23: _SetCaret((unsigned int)_text.size(), shift); break; // VK_END
	case 0x0D: committed = true; break;              // VK_RETURN
	default:
		if (auto c = VkToChar(action.KeyChar, shift))
			_InsertChar(*c);
		break;
	}

	if (before != _text)
		_OnTextChanged();

	if (committed)
		_OnCommit();

	return { true, std::to_string(_index), "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
}

std::optional<char> GuiTextBox::VkToChar(unsigned int vk, bool shift)
{
	// Letters: VK 'A'-'Z' map to ASCII; apply case via shift.
	if (vk >= 'A' && vk <= 'Z')
		return (char)(shift ? vk : (vk + 32));

	// Top-row digits.
	if (vk >= '0' && vk <= '9')
	{
		if (!shift)
			return (char)vk;
		static const char shifted[] = { ')', '!', '@', '#', '$', '%', '^', '&', '*', '(' };
		return shifted[vk - '0'];
	}

	// Numpad digits (VK_NUMPAD0..9).
	if (vk >= 0x60 && vk <= 0x69)
		return (char)('0' + (vk - 0x60));

	switch (vk)
	{
	case 0x20: return ' ';                 // VK_SPACE
	case 0x6E: return '.';                 // VK_DECIMAL
	case 0x6D: return '-';                 // VK_SUBTRACT
	case 0x6B: return '+';                 // VK_ADD
	case 0xBE: return shift ? '>' : '.';   // VK_OEM_PERIOD
	case 0xBC: return shift ? '<' : ',';   // VK_OEM_COMMA
	case 0xBD: return shift ? '_' : '-';   // VK_OEM_MINUS
	case 0xBB: return shift ? '+' : '=';   // VK_OEM_PLUS
	case 0xBF: return shift ? '?' : '/';   // VK_OEM_2
	default:   return std::nullopt;
	}
}

// --------------------------------------------------------------------------
// Resources / drawing
// --------------------------------------------------------------------------

void GuiTextBox::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);
	_label->InitResources(resourceLib, forceInit);
	_caretQuad.InitResources(resourceLib, forceInit);

	auto fontOpt = resourceLib.GetClosestFontForControlBox(_label->GetSize().Height, 0u);
	if (fontOpt.has_value())
		_font = fontOpt.value();
}

void GuiTextBox::Draw(base::DrawContext& ctx)
{
	if (!_isVisible)
		return;

	// Background (own texture) via base; no tree children attached.
	GuiElement::Draw(ctx);

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3((float)pos.X, (float)pos.Y, 0.f)));

	_label->Draw(ctx);

	auto font = _font.lock();
	if (font && _hasFocus)
	{
		const int labelH = (int)_label->GetSize().Height;

		// Selection underline.
		if (HasSelection())
		{
			const float x0 = font->MeasureString(_text.substr(0, SelectionStart()));
			const float x1 = font->MeasureString(_text.substr(0, SelectionStart() + SelectionLength()));
			_caretQuad.SetSize({ (unsigned int)std::max(1.0f, x1 - x0), 2u });
			_caretQuad.SetPosition({ (int)_padding + (int)x0, (int)_padding + labelH - 2 });
			_caretQuad.Draw(ctx);
		}

		// Caret.
		const float cx = font->MeasureString(_text.substr(0, _caret));
		_caretQuad.SetSize({ 2u, (unsigned int)labelH });
		_caretQuad.SetPosition({ (int)_padding + (int)cx, (int)_padding });
		_caretQuad.Draw(ctx);
	}

	glCtx.PopMvp();
}
