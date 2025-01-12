#include "GuiSelector.h"

using namespace gui;
using namespace utils;
using base::Action;
using base::Tweakable;

GuiSelector::GuiSelector(GuiSelectorParams guiParams) :
	_isSelecting(false),
	_selectionTool(SELECTION_PAINT),
	_selectMode(SELECT_SELECT),
	_initPos({0,0}),
	_currentPos({0,0}),
	_currentHover({}),
	_currentHoverState(Tweakable::TweakState::TWEAKSTATE_DEFAULT),
	_selections({}),
	GuiElement(guiParams)
{
}

std::vector<unsigned char> GuiSelector::CurrentHover() const
{
	return _currentHover;
}

bool GuiSelector::UpdateCurrentHover(std::vector<unsigned char> path, Action::Modifiers modifiers)
{
	bool isHovering = path.size() > 2 ?
		(path[0] != 0) || (path[1] != 0) || (path[2] != 0) :
		false;

	if (!isHovering)
	{
		_currentHover = {};
		return isHovering;
	}

	if (path != _currentHover)
	{
		_currentHover = path;

		if (_isSelecting)
		{
			switch (_selectionTool)
			{
			case SELECTION_PAINT:
				if (modifiers & Action::Modifiers::MODIFIER_ALT)
					RemovePaintSelection(path);
				else
					AddPaintSelection(path);

				break;
			}
		}
	}

	return isHovering;
}

void GuiSelector::ClearSelection()
{
	_selections.clear();
}

actions::ActionResult GuiSelector::OnAction(actions::TouchAction action)
{
	actions::ActionResult result;
	result.IsEaten = false;

	if (actions::TouchAction::TouchState::TOUCH_DOWN == action.State)
	{
		if (!_currentHover.empty())
		{
			// Check current status and flip it
			if (0 == action.Index)
			{
				result.IsEaten = true;
				auto mode = (Action::Modifiers::MODIFIER_ALT & action.Modifiers) > 0 ?
					SELECT_DESELECT :
					SELECT_SELECT;

				StartPaintSelection(mode, _currentHover);
			}
			else if (1 == action.Index)
			{
				result.IsEaten = true;
				auto mode = (Action::Modifiers::MODIFIER_ALT & action.Modifiers) > 0 ?
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
			if ((SELECT_SELECT == _selectMode) || (SELECT_DESELECT == _selectMode))
			{
				result.IsEaten = true;
				result.ResultType = SELECT_SELECT == _selectMode ?
					actions::ActionResultType::ACTIONRESULT_SELECT :
					actions::ActionResultType::ACTIONRESULT_DESELECT;

				EndSelection();
			}
		}
		else if (1 == action.Index)
		{
			if ((SELECT_MUTE == _selectMode) || (SELECT_UNMUTE == _selectMode))
			{
				result.IsEaten = true;
				result.ResultType = SELECT_MUTE == _selectMode ?
					actions::ActionResultType::ACTIONRESULT_MUTE :
					actions::ActionResultType::ACTIONRESULT_UNMUTE;

				EndSelection();
			}
		}
	}

	return result;
}

void GuiSelector::StartPaintSelection(SelectMode mode, std::vector<unsigned char> selection)
{
	Reset();

	_isSelecting = true;
	_selectionTool = SELECTION_PAINT;
	_selectMode = mode;

	_selections.push_back(selection);
}

void GuiSelector::AddPaintSelection(std::vector<unsigned char> selection)
{
	if (_isSelecting && (SELECTION_PAINT != _selectionTool))
		return;

	RemovePaintSelection(selection);
	_selections.push_back(selection);
}

void GuiSelector::RemovePaintSelection(std::vector<unsigned char> selection)
{
	if (_isSelecting && (SELECTION_PAINT != _selectionTool))
		return;

	std::vector<std::vector<unsigned char>> newSelections;

	for (auto& s : _selections)
	{
		if (s != selection)
			newSelections.push_back(s);
	}

	_selections = newSelections;
}

std::vector<std::vector<unsigned char>>::const_iterator GuiSelector::begin() const
{
	return _selections.cbegin();
}

std::vector<std::vector<unsigned char>>::const_iterator GuiSelector::end() const
{
	return _selections.cend();
}

void GuiSelector::StartRectSelection(SelectMode mode, utils::Position2d pos)
{
	Reset();

	_isSelecting = true;
	_selectionTool = SELECTION_RECT;
	_selectMode = mode;

	_initPos = pos;
	_currentPos = pos;
}

void GuiSelector::UpdateRectSelection(utils::Position2d pos)
{
	if (_isSelecting && (SELECTION_RECT != _selectionTool))
		return;

	_currentPos = pos;
}

void GuiSelector::EndSelection()
{
	_isSelecting = false;
}

void GuiSelector::Reset()
{
	_selections.clear();
}
