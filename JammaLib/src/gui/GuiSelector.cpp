#include "GuiSelector.h"

using namespace gui;
using namespace utils;
using base::Action;
using base::Tweakable;

GuiSelector::GuiSelector(GuiSelectorParams guiParams) :
	_isSelecting(false),
	_selectionTool(SELECTION_PAINT),
	_selectMode(SELECT_NONE),
	_initPos({0,0}),
	_currentPos({0,0}),
	_currentHover({}),
	_currentHoverSelected(false),
	_currentHoverTweakState(Tweakable::TweakState::TWEAKSTATE_NONE),
	GuiElement(guiParams)
{
}

GuiSelector::SelectMode GuiSelector::CurrentMode() const
{
	return _selectMode;
}

std::vector<unsigned char> GuiSelector::CurrentHover() const
{
	return _currentHover;
}

bool GuiSelector::UpdateCurrentHover(std::vector<unsigned char> path,
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

actions::ActionResult GuiSelector::OnAction(actions::TouchAction action)
{
	actions::ActionResult result = GuiElement::OnAction(action);
	result.IsEaten = false;

	if (actions::TouchAction::TouchState::TOUCH_DOWN == action.State)
	{
		if (_currentHover.empty())
			_selectMode = SELECT_NONE;
		else
		{
			// Check current status and flip it
			if (0 == action.Index)
			{
				result.IsEaten = true;
				auto mode = Action::MODIFIER_CTRL & action.Modifiers ?
					(_currentHoverSelected ? SELECT_SELECTREMOVE : SELECT_SELECTADD ) :
					SELECT_SELECT;

				StartPaintSelection(mode, _currentHover);

				if (SELECT_SELECT == mode)
					result.ResultType = actions::ACTIONRESULT_INITSELECT;
			}
			else if (1 == action.Index)
			{
				result.IsEaten = true;
				auto mode = Tweakable::TweakState::TWEAKSTATE_MUTED & _currentHoverTweakState ?
					SELECT_UNMUTE :
					SELECT_MUTE;

				StartPaintSelection(mode, _currentHover);
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
				result.IsEaten = true;
				result.ResultType = actions::ACTIONRESULT_SELECT;

				EndSelection();
			}
		}
		else if (1 == action.Index)
		{
			if ((SELECT_MUTE == _selectMode) ||
				(SELECT_UNMUTE == _selectMode))
			{
				result.IsEaten = true;
				result.ResultType = (SELECT_MUTE == _selectMode) ?
					actions::ACTIONRESULT_MUTE :
					actions::ACTIONRESULT_UNMUTE;

				EndSelection();
			}
		}
	}

	return result;
}

actions::ActionResult GuiSelector::OnAction(actions::KeyAction action)
{
	actions::ActionResult result = GuiElement::OnAction(action);
	result.IsEaten = false;

	if (17 == action.KeyChar)
	{
		if ((SELECT_NONE == _selectMode) && (actions::KeyAction::KeyActionType::KEY_DOWN == action.KeyActionType))
		{
			_selectMode = SELECT_NONEADD;
			result.IsEaten = true;
		}
		else if ((SELECT_NONEADD == _selectMode) && (actions::KeyAction::KeyActionType::KEY_UP == action.KeyActionType))
		{
			_selectMode = SELECT_NONE;
			result.IsEaten = true;
		}
	}

	return result;
}

bool GuiSelector::IsHovering(std::vector<unsigned char> path) const
{
	return path.size() > 2 ?
		(path[0] != 0) || (path[1] != 0) || (path[2] != 0) :
		false;
}

void GuiSelector::StartPaintSelection(SelectMode mode, std::vector<unsigned char> selection)
{
	_isSelecting = true;
	_selectionTool = SELECTION_PAINT;
	_selectMode = mode;
}

void GuiSelector::StartRectSelection(SelectMode mode, utils::Position2d pos)
{
	_isSelecting = true;
	_selectionTool = SELECTION_RECT;
	_selectMode = mode;

	_initPos = pos;
	_currentPos = pos;
}

void GuiSelector::UpdateRectSelection(utils::Position2d pos)
{
	if (!_isSelecting)
		return;

	if (SELECTION_RECT != _selectionTool)
		return;

	_currentPos = pos;
}

void GuiSelector::EndSelection()
{
	_isSelecting = false;
	_selectMode = SELECT_NONE;
}