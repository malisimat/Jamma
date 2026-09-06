#include "GuiPopupManager.h"

using namespace gui;
using namespace actions;
using base::GuiElement;

void GuiPopupManager::Open(std::shared_ptr<GuiElement> element,
	std::shared_ptr<GuiElement> owner)
{
	if (!element)
		return;

	_popups.push_back({ element, owner });
}

void GuiPopupManager::Close()
{
	if (!_popups.empty())
		_popups.pop_back();
}

void GuiPopupManager::CloseAll()
{
	_popups.clear();
}

bool GuiPopupManager::IsOpen() const
{
	return !_popups.empty();
}

std::shared_ptr<GuiElement> GuiPopupManager::Top() const
{
	return _popups.empty() ? nullptr : _popups.back().Element;
}

std::shared_ptr<GuiElement> GuiPopupManager::OwnerOfTop() const
{
	return _popups.empty() ? nullptr : _popups.back().Owner.lock();
}

void GuiPopupManager::Draw(base::DrawContext& ctx)
{
	for (auto& popup : _popups)
		if (popup.Element)
			popup.Element->Draw(ctx);
}

ActionResult GuiPopupManager::OnAction(TouchAction action)
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

ActionResult GuiPopupManager::OnAction(TouchMoveAction action)
{
	if (_popups.empty())
		return ActionResult::NoAction();

	auto top = _popups.back().Element;
	top->OnAction(top->ParentToLocal(action));

	// Capture move events while open so hover does not leak to the scene.
	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
}

ActionResult GuiPopupManager::OnAction(KeyAction action)
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
