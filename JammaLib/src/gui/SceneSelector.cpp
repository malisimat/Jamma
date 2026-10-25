#include "SceneSelector.h"

using namespace base;
using namespace gui;
using namespace utils;
using base::Action;
using base::Tweakable;

SceneSelector::SceneSelector(GuiSelectorParams guiParams) :
	_isSelecting(false),
	_selectionTool(SELECTION_PAINT),
	_selectMode(SELECT_NONE),
	_selectDepth(SelectDepth::DEPTH_LOOPTAKE),
	_initPos({0,0}),
	_currentPos({0,0}),
	_currentHover({}),
	_paintedPath({}),
	_currentHoverSelected(false),
	_pressHoverSelected(false),
	_clickPressed(false),
	_currentHoverTweakState(Tweakable::TweakState::TWEAKSTATE_NONE),
	GuiElement(guiParams)
{
}

SceneSelector::SelectMode SceneSelector::CurrentMode() const
{
	return _selectMode;
}

SelectDepth SceneSelector::CurrentSelectDepth() const
{
	return _selectDepth;
}

void SceneSelector::SetSelectDepth(SelectDepth level)
{
	_selectDepth = level;
}

std::vector<unsigned char> SceneSelector::CurrentHover() const
{
	return _currentHover;
}

std::vector<unsigned char> SceneSelector::PaintedPathForTest() const
{
	return _paintedPath;
}

bool SceneSelector::IsClickPressed() const
{
	return _clickPressed;
}

bool SceneSelector::IsMutePressed() const
{
	return _clickPressed && (SELECT_MUTE == _selectMode || SELECT_UNMUTE == _selectMode);
}

bool SceneSelector::IsPaintingMute() const
{
	return _isSelecting && !_clickPressed && (SELECT_MUTE == _selectMode || SELECT_UNMUTE == _selectMode);
}

bool SceneSelector::UpdateCurrentHover(std::vector<unsigned char> path,
	Action::Modifiers modifiers,
	bool isSelected,
	base::Tweakable::TweakState tweakState)
{
	bool isHovering = IsHovering(path);

	if (!isHovering)
	{
		_currentHover = {};
		_currentHoverSelected = false;
		_currentHoverTweakState = Tweakable::TWEAKSTATE_NONE;
		return isHovering;
	}

	_currentHover = path;
	_currentHoverSelected = isSelected;
	_currentHoverTweakState = tweakState;

	return isHovering;
}

actions::ActionResult SceneSelector::OnAction(actions::TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	res.IsEaten = false;

	if (actions::TouchAction::TouchState::TOUCH_DOWN == action.State)
	{
		_initPos = action.Position;
		_pressHoverSelected = _currentHoverSelected;
		_clickPressed = false;
		if (_currentHover.empty())
			_selectMode = SELECT_NONE;
		else
		{
			// Check current status and flip it
			if (0 == action.Index)
			{
				res.IsEaten = true;
				auto mode = Action::MODIFIER_SHIFT & action.Modifiers ?
					(_currentHoverSelected ? SELECT_SELECTREMOVE : SELECT_SELECTADD ) :
					SELECT_SELECT;

				StartPaintSelection(mode, _currentHover);
				_clickPressed = (SELECT_SELECT == mode);

				if (SELECT_SELECT == mode)
					res.ResultType = actions::ACTIONRESULT_INITSELECT;
			}
			else if (1 == action.Index)
			{
				res.IsEaten = true;
				auto mode = Tweakable::TweakState::TWEAKSTATE_MUTED & _currentHoverTweakState ?
					SELECT_UNMUTE :
					SELECT_MUTE;

				StartPaintSelection(mode, _currentHover);
				_clickPressed = true;
				res.ResultType = actions::ACTIONRESULT_INITSELECT;
			}
		}
	}
	else if (actions::TouchAction::TouchState::TOUCH_UP == action.State)
	{
		if (0 == action.Index)
		{
			if ((SELECT_SELECT == _selectMode) ||
				(SELECT_SELECTADD == _selectMode) ||
				(SELECT_SELECTREMOVE == _selectMode))
			{
				res.IsEaten = true;
				res.ResultType = (SELECT_SELECT == _selectMode) ?
					actions::ACTIONRESULT_SELECT : actions::ACTIONRESULT_DEFAULT;

				EndSelection();
			}
		}
		else if (1 == action.Index)
		{
			if ((SELECT_MUTE == _selectMode) ||
				(SELECT_UNMUTE == _selectMode))
			{
				res.IsEaten = true;
				res.ResultType = !_clickPressed ? actions::ACTIONRESULT_DEFAULT : (SELECT_MUTE == _selectMode) ?
					actions::ACTIONRESULT_MUTE :
					actions::ACTIONRESULT_UNMUTE;

				EndSelection();
			}
		}
	}

	return res;
}

actions::ActionResult SceneSelector::OnAction(actions::TouchMoveAction action)
{
	auto res = GuiElement::OnAction(action);
	if (!_isEnabled || !_isVisible || !_isSelecting)
		return res;
	const bool muting = SELECT_MUTE == _selectMode || SELECT_UNMUTE == _selectMode;
	const auto buttonMask = muting ? 2u : 1u;
	if (0u == (action.MouseButtonsDown & buttonMask))
	{
		// Capture loss cancels a pending click, but keeps already painted states.
		EndSelection();
		res.IsEaten = true;
		return res;
	}

	if (SELECT_SELECT == _selectMode || (muting && _clickPressed))
	{
		const auto dx = action.Position.X - _initPos.X;
		const auto dy = action.Position.Y - _initPos.Y;
		if (dx * dx + dy * dy >= 16)
		{
			if (!muting)
				_selectMode = _pressHoverSelected ? SELECT_SELECTREMOVE : SELECT_SELECTADD;
			_clickPressed = false;
			res.ResultType = actions::ACTIONRESULT_INITSELECT;
			res.IsEaten = true;
		}
	}
	return res;
}

actions::ActionResult SceneSelector::OnAction(actions::KeyAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	res.IsEaten = false;

	if (16 == action.KeyChar)
	{
		if ((SELECT_NONE == _selectMode) && (actions::KeyAction::KeyActionType::KEY_DOWN == action.KeyActionType))
		{
			_selectMode = SELECT_NONEADD;
			res.IsEaten = true;
		}
		else if ((SELECT_NONEADD == _selectMode) && (actions::KeyAction::KeyActionType::KEY_UP == action.KeyActionType))
		{
			_selectMode = SELECT_NONE;
			res.IsEaten = true;
		}
	}

	return res;
}

bool SceneSelector::IsHovering(std::vector<unsigned char> path) const
{
	auto depth = 1u + (unsigned int)_selectDepth;

	if (path.size() < depth)
		return false;

	return !path.empty();
}

void SceneSelector::StartPaintSelection(SelectMode mode, std::vector<unsigned char> selection)
{
	_isSelecting = true;
	_selectionTool = SELECTION_PAINT;
	_selectMode = mode;
	_paintedPath = selection;
}

void SceneSelector::StartRectSelection(SelectMode mode, utils::Position2d pos)
{
	_isSelecting = true;
	_selectionTool = SELECTION_RECT;
	_selectMode = mode;

	_initPos = pos;
	_currentPos = pos;
}

void SceneSelector::UpdateRectSelection(utils::Position2d pos)
{
	if (!_isSelecting)
		return;

	if (SELECTION_RECT != _selectionTool)
		return;

	_currentPos = pos;
}

void SceneSelector::EndSelection()
{
	_isSelecting = false;
	_clickPressed = false;
	_selectMode = SELECT_NONE;
}

void gui::SceneSelector::ClearPointerState()
{
 GuiElement::ClearPointerState();
 EndSelection();
 _clickPressed = false;
 _currentHover.clear();
 _paintedPath.clear();
}

SceneSelector::SelectionUpdate SceneSelector::SelectionChanges(actions::ActionResultType result) const
{
	SelectionUpdate update;
	update.HoverPath = _currentHover;
	const bool paintingSelection = _selectMode == SELECT_SELECTADD || _selectMode == SELECT_SELECTREMOVE || IsPaintingMute();
	update.HighlightMidiSiblings = !paintingSelection && _selectDepth == base::DEPTH_LOOP;
	update.ClickPressed = IsClickPressed() && _currentHover == _paintedPath;
	update.MutePressed = IsMutePressed();
	const auto change = [&update](HierarchyEdit edit, const std::vector<unsigned char>& path) {
		update.Changes[update.ChangeCount++] = { edit, path };
	};
	switch (result)
	{
	case actions::ACTIONRESULT_DEFAULT:
		update.ClearPicking = true;
		switch (_selectMode)
		{
		case SELECT_NONE:
		case SELECT_NONEADD:
		case SELECT_SELECT: update.HighlightHover = true; break;
		case SELECT_SELECTADD: change(HierarchyEdit::Select, _currentHover); break;
		case SELECT_SELECTREMOVE: change(HierarchyEdit::Deselect, _currentHover); break;
		case SELECT_MUTE:
		case SELECT_UNMUTE:
			if (IsPaintingMute()) change(_selectMode == SELECT_MUTE ? HierarchyEdit::Mute : HierarchyEdit::Unmute, _currentHover);
			else update.HighlightHover = true;
			break;
		}
		break;
	case actions::ACTIONRESULT_SELECT:
		update.ClearSelection = true;
		update.ClearPicking = update.HighlightHover = true;
		change(HierarchyEdit::Select, _paintedPath);
		break;
	case actions::ACTIONRESULT_MUTE:
	case actions::ACTIONRESULT_UNMUTE:
		update.ClearPicking = update.HighlightHover = true;
		change(result == actions::ACTIONRESULT_MUTE ? HierarchyEdit::Mute : HierarchyEdit::Unmute, _paintedPath);
		break;
	case actions::ACTIONRESULT_INITSELECT:
		update.ClearPicking = true;
		if (_selectMode == SELECT_SELECTADD || _selectMode == SELECT_SELECTREMOVE)
		{
			const auto edit = _selectMode == SELECT_SELECTADD ? HierarchyEdit::Select : HierarchyEdit::Deselect;
			change(edit, _paintedPath);
			change(edit, _currentHover);
		}
		else if (IsPaintingMute())
		{
			const auto edit = _selectMode == SELECT_MUTE ? HierarchyEdit::Mute : HierarchyEdit::Unmute;
			change(edit, _paintedPath);
			change(edit, _currentHover);
		}
		update.HighlightHover = !paintingSelection;
		break;
	case actions::ACTIONRESULT_CLEARSELECT:
		update.ClearSelection = update.ClearPicking = true;
		break;
	default: break;
	}
	return update;
}
