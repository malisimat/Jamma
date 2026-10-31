#include "PointerInteraction.h"
#include <algorithm>
#include "GuiPanel.h"

using namespace gui;
using namespace actions;

bool PointerInteraction::IsPress(const TouchAction& action)
{
	// Wheel DOWN events have no matching UP and never own capture or focus.
	return action.State == TouchAction::TOUCH_DOWN && !(action.Touch == TouchAction::TOUCH_MOUSE && action.Index == 4);
}

void PointerInteraction::FocusPress(const std::shared_ptr<base::GuiElement>& active,
	GuiFocusManager& focus, bool finalizePrevious)
{
	if (finalizePrevious)
		if (auto previous = focus.CurrentFocus(); previous && previous != active) previous->FinalizeEdits();
	if (active && active->WantsFocusOnPress()) focus.RequestFocus(active);
	else focus.ClearFocus();
}

ActionResult PointerInteraction::DispatchCapture(TouchAction action, Release release, bool eat)
{
	const auto active = Captured();
	auto result = active ? active->OnAction(active->GlobalToLocal(action)) : ActionResult::NoAction();
	if (action.State == TouchAction::TOUCH_UP
		&& (release == Release::AnyUp || (release == Release::LeftUp && action.Index == 0))) ReleaseCapture();
	result.IsEaten |= eat;
	return result;
}

ActionResult PointerInteraction::DispatchCapture(TouchMoveAction action, CaptureLoss loss, bool eat)
{
	const bool cancelled = action.MouseButtonsDown == 0u
		&& (loss == CaptureLoss::AnyZeroButtons || action.Touch == TouchAction::TOUCH_MOUSE);
	auto result = ActionResult::NoAction();
	if (auto active = Captured())
	{
		if (cancelled) active->ClearPointerState();
		else result = active->OnAction(active->GlobalToLocal(action));
	}
	if (cancelled) ReleaseCapture();
	if (cancelled || eat) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	return result;
}

bool PointerInteraction::CompleteDispatch(const TouchAction& action, const ActionResult& result,
	Owner owner, GuiFocusManager* focus, ActionUndoHistory& history)
{
	if (!result.IsEaten) return false;
	if (result.Undo) history.Add(result.Undo);
	if (IsPress(action))
	{
		if (!HasCapture()) Capture(result.ActiveElement, owner);
		if (focus) FocusPress(result.ActiveElement.lock(), *focus);
	}
	return true;
}

void PointerInteraction::OnSubtreeHidden(const std::shared_ptr<base::GuiElement>& subtree, GuiFocusManager& focus)
{
	const auto belongs = [&subtree](std::shared_ptr<base::GuiElement> element) {
		for (; element; element = element->Parent()) if (element == subtree) return true;
		return false;
	};
	if (belongs(focus.CurrentFocus())) focus.ClearFocus();
	if (belongs(Captured())) { ReleaseCapture(); ConsumeNextRelease(); }
	InvalidateHover();
}

std::optional<ActionResult> PointerInteraction::RoutePanels(TouchAction action,
	std::span<const std::shared_ptr<base::GuiElement>> panels, GuiFocusManager& focus, bool otherPointerOwner)
{
	if (action.State == TouchAction::TOUCH_UP && ConsumesRelease())
	{ ClearConsumedRelease(); return ActionResult{ true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} }; }
	if (IsCapturedBy(Owner::Settings))
	{
		auto result = DispatchCapture(action, Release::AnyUp, true);
		if (action.State == TouchAction::TOUCH_UP) ClearConsumedRelease();
		return result;
	}
	if (HasCapture() || otherPointerOwner) return std::nullopt;
	for (const auto& panel : panels)
	{
		if (!panel || !panel->RouteHitTest(panel->GlobalToLocal(action.Position))) continue;
		auto result = panel->OnAction(panel->GlobalToLocal(action));
		result.IsEaten = true;
		if (IsPress(action))
		{
			auto active = result.ActiveElement.lock();
			if (!active) active = panel;
			Capture(active, Owner::Settings);
			FocusPress(active, focus, true);
		}
		return result;
	}
	return std::nullopt;
}

std::optional<ActionResult> PointerInteraction::RoutePanels(TouchMoveAction action,
	std::span<const std::shared_ptr<base::GuiElement>> panels, bool otherPointerOwner)
{
	if (ConsumesRelease() && action.MouseButtonsDown == 0u)
	{ ClearConsumedRelease(); return ActionResult{ true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} }; }
	if (IsCapturedBy(Owner::Settings))
	{
		auto result = DispatchCapture(action, CaptureLoss::AnyZeroButtons, true);
		if (action.MouseButtonsDown == 0u) { ClearConsumedRelease(); InvalidateHover(); }
		return result;
	}
	if (HasCapture() || otherPointerOwner) return std::nullopt;
	for (const auto& panel : panels)
		if (panel && panel->RouteHitTest(panel->GlobalToLocal(action.Position)))
		{
			panel->OnAction(panel->GlobalToLocal(action));
			return ActionResult{ true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
		}
	return std::nullopt;
}

bool PointerInteraction::ResolveHoverPath(std::span<const std::shared_ptr<base::GuiElement>> roots,
	bool reverse, utils::Position2d cursor, HoverPath& path)
{
	path.clear();
	std::shared_ptr<base::GuiElement> leaf;
	for (std::size_t index = 0u; index < roots.size(); ++index)
	{
		const auto& root = roots[reverse ? roots.size() - 1u - index : index];
		if (root) leaf = root->FindTopmostDescendant(root->ParentToLocal(cursor));
		if (leaf) break;
	}
	if (!leaf) return false;
	while (leaf) { path.push_back(leaf); leaf = leaf->Parent(); }
	std::reverse(path.begin(), path.end());
	return true;
}

void PointerInteraction::_LockPath(const HoverPath& path, std::vector<std::shared_ptr<base::GuiElement>>& result)
{
	result.clear();
	result.reserve(path.size());
	for (const auto& element : path)
	{
		auto locked = element.lock();
		if (!locked) break;
		result.push_back(std::move(locked));
	}
}

void PointerInteraction::ApplyHover(const HoverPath& next, utils::Position2d cursor)
{
	_previousShared.clear();
	_nextShared.clear();
	_LockPath(_hoverPath, _previousShared);
	_LockPath(next, _nextShared);
	// Only the leaf is a pointer target; opted-in panel backgrounds follow its path.
	for (const auto& element : _previousShared)
	{
		element->ApplyHoverState(false);
		if (auto panel = std::dynamic_pointer_cast<GuiPanel>(element)) panel->ApplyPanelHover(false);
	}
	if (!_nextShared.empty())
	{
		const auto& leaf = _nextShared.back();
		leaf->ApplyHoverPoint(leaf->GlobalToLocal(cursor));
	}
	for (const auto& element : _nextShared)
		if (auto panel = std::dynamic_pointer_cast<GuiPanel>(element)) panel->ApplyPanelHover(true);
}
