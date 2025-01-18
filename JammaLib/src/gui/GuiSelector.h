#pragma once

#include <vector>
#include <algorithm>
#include "GuiElement.h"
#include "Tweakable.h"
#include "../base/Action.h"
#include <Tweakable.h>

namespace gui
{
	class GuiSelectorParams : public base::GuiElementParams {};

	class GuiSelector :
		public base::GuiElement
	{
	public:
		enum SelectionTool
		{
			SELECTION_PAINT,
			SELECTION_RECT
		};

		enum SelectMode
		{
			SELECT_NONE,
			SELECT_SELECT,
			SELECT_DESELECT,
			SELECT_MUTE,
			SELECT_UNMUTE
		};

	public:
		GuiSelector(GuiSelectorParams guiParams);

	public:
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;

		std::vector<unsigned char> CurrentHover() const;
		bool UpdateCurrentHover(std::vector<unsigned char> path,
			base::Action::Modifiers modifiers,
			bool isSelected,
			base::Tweakable::TweakState tweakState);
		void ClearSelection();

		std::vector<std::vector<unsigned char>>::const_iterator begin() const;
		std::vector<std::vector<unsigned char>>::const_iterator end() const;

	protected:
		bool IsHovering(std::vector<unsigned char> path) const;

		void StartPaintSelection(SelectMode mode, std::vector<unsigned char> selection);
		void AddPaintSelection(std::vector<unsigned char> selection);
		void RemovePaintSelection(std::vector<unsigned char> selection);

		void StartRectSelection(SelectMode mode, utils::Position2d pos);
		void UpdateRectSelection(utils::Position2d pos);

		void EndSelection();
		void Reset();

	protected:
		bool _isSelecting;
		SelectMode _selectMode;
		SelectionTool _selectionTool;
		utils::Position2d _initPos;
		utils::Position2d _currentPos;
		std::vector<unsigned char> _currentHover;
		bool _currentHoverSelected;
		base::Tweakable::TweakState _currentHoverTweakState;
		std::vector<std::vector<unsigned char>> _selections;
	};
}
