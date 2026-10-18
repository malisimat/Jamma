#include "GuiPopupManager.h"
#include <algorithm>

using namespace gui;
using namespace actions;
using base::GuiElement;

void GuiPopupManager::Open(std::shared_ptr<GuiElement> element,
	std::shared_ptr<GuiElement> owner, std::function<void()> onClosed, std::function<bool()> canDismiss)
{
	if (!element)
		return;

	// Covering a popup interrupts any gesture that began on its buttons.
	if (auto top = Top()) top->ClearPointerState();

	// Clear the owner's pointer state before the popup captures input.
	if (owner)
	{
		auto root = owner;
		while (auto parent = root->Parent())
			root = parent;
		root->ClearPointerState();
	}

	_popups.push_back({ element, owner, std::move(onClosed), std::move(canDismiss) });
}

void GuiPopupManager::Close()
{
	if (!_popups.empty())
	{
		auto popup = std::move(_popups.back());
		_popups.pop_back();
		popup.Element->ClearPointerState();
		if (popup.OnClosed) popup.OnClosed();
	}
}

void GuiPopupManager::Close(const std::shared_ptr<GuiElement>& element)
{
	if (!element) return;
	std::vector<Popup> closed;
	for (auto it = _popups.begin(); it != _popups.end();)
		if (it->Element == element) { closed.push_back(std::move(*it)); it = _popups.erase(it); }
		else ++it;
	for (auto& popup : closed) { popup.Element->ClearPointerState(); if (popup.OnClosed) popup.OnClosed(); }
}

void GuiPopupManager::CloseAll()
{
	auto closed = std::move(_popups);
	_popups.clear();
	for (auto& popup : closed) { popup.Element->ClearPointerState(); if (popup.OnClosed) popup.OnClosed(); }
}

void GuiPopupManager::CloseOwnedBy(const std::shared_ptr<GuiElement>& subtree)
{
	if (!subtree) return;
	std::vector<Popup> closed;
	for (auto it = _popups.begin(); it != _popups.end();)
	{
		bool belongs = false;
		for (auto owner = it->Owner.lock(); owner; owner = owner->Parent())
			if (owner == subtree) { belongs = true; break; }
		if (belongs) { closed.push_back(std::move(*it)); it = _popups.erase(it); }
		else ++it;
	}
	for (auto& popup : closed) { popup.Element->ClearPointerState(); if (popup.OnClosed) popup.OnClosed(); }
}

bool GuiPopupManager::ConsumeDismissedRelease(TouchAction action)
{
	if (!_dismissedPress || action.Touch != _dismissedPress->Touch || action.Index != _dismissedPress->Index)
		return false;
	if (action.State == TouchAction::TOUCH_UP) { _dismissedPress.reset(); return true; }
	_dismissedPress.reset();
	return false;
}

bool GuiPopupManager::Contains(const std::shared_ptr<GuiElement>& element) const
{
	return std::any_of(_popups.begin(), _popups.end(),
		[&element](const auto& popup) { return popup.Element == element; });
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
	{
		auto result = top->OnAction(local);
		// Child dispatch stops at the first consumer; release also disarms siblings.
		if (action.State == TouchAction::TOUCH_UP) top->ClearPointerState();
		return result;
	}

	// Releases outside never reach the buttons, so cancel their retained press.
	if (action.State == TouchAction::TOUCH_UP) top->ClearPointerState();

	// Outside press dismisses the topmost popup and consumes the event so it
	// does not activate whatever sits beneath the popup.
	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
	{
		_dismissedPress = action;
		if (!_popups.back().CanDismiss || _popups.back().CanDismiss()) Close();
	}

	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
}

ActionResult GuiPopupManager::OnAction(TouchMoveAction action)
{
	if (_popups.empty())
		return ActionResult::NoAction();

	auto top = _popups.back().Element;
	// Native capture loss is represented by a mouse move with no held buttons.
	if (action.Touch == TouchAction::TOUCH_MOUSE && action.MouseButtonsDown == 0u)
		top->ClearPointerState();
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
		if (!_popups.back().CanDismiss || _popups.back().CanDismiss()) Close();
		return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
	}

	auto res = _popups.back().Element->OnAction(action);
	if (res.IsEaten)
		return res;

	// Capture keys while open.
	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<GuiElement>() };
}
