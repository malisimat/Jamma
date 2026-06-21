#include "GuiPopupHost.h"

using namespace gui;
using namespace actions;
using base::GuiElement;

void GuiPopupHost::Open(std::shared_ptr<GuiElement> element,
	std::shared_ptr<GuiElement> owner)
{
	if (!element)
		return;

	_popups.push_back({ element, owner });
}

void GuiPopupHost::Close()
{
	if (!_popups.empty())
		_popups.pop_back();
}

void GuiPopupHost::CloseAll()
{
	_popups.clear();
}

bool GuiPopupHost::IsOpen() const
{
	return !_popups.empty();
}

std::shared_ptr<GuiElement> GuiPopupHost::Top() const
{
	return _popups.empty() ? nullptr : _popups.back().Element;
}

std::shared_ptr<GuiElement> GuiPopupHost::OwnerOfTop() const
{
	return _popups.empty() ? nullptr : _popups.back().Owner.lock();
}

void GuiPopupHost::Draw(base::DrawContext& ctx)
{
	for (auto& popup : _popups)
		if (popup.Element)
			popup.Element->Draw(ctx);
}

ActionResult GuiPopupHost::OnAction(TouchAction action)
{
	if (_popups.empty())
		return ActionResult::NoAction();

	auto top = _popups.back().Element;

	// Popups live in global coordinates, so parent-local == global - Position().
	auto local = top->ParentToLocal(action);
	const bool inside = top->HitTest(local.Position);

	if (inside)
		return top->OnAction(local);

	// Outside press dismisses the topmost popup and consumes the event so it
	// does not activate whatever sits beneath the popup.
	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
		Close();

	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
}

ActionResult GuiPopupHost::OnAction(TouchMoveAction action)
{
	if (_popups.empty())
		return ActionResult::NoAction();

	auto top = _popups.back().Element;
	top->OnAction(top->ParentToLocal(action));

	// Capture move events while open so hover does not leak to the scene.
	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
}

ActionResult GuiPopupHost::OnAction(KeyAction action)
{
	if (_popups.empty())
		return ActionResult::NoAction();

	// Escape (VK_ESCAPE) dismisses the topmost popup.
	if ((27 == action.KeyChar) && (KeyAction::KEY_UP == action.KeyActionType))
	{
		Close();
		return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
	}

	auto res = _popups.back().Element->OnAction(action);
	if (res.IsEaten)
		return res;

	// Capture keys while open.
	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
}
