#pragma once

#include <vector>
#include <algorithm>
#include "../base/Jammable.h"
#include "../base/Action.h"

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
			SELECT_NONEADD,
			SELECT_SELECT,
			SELECT_SELECTADD,
			SELECT_SELECTREMOVE,
			SELECT_MUTE,
			SELECT_UNMUTE
		};

	public:
		GuiSelector(GuiSelectorParams guiParams);

	public:
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;

		SelectMode CurrentMode() const;
		base::SelectDepth CurrentSelectDepth() const;
		void SetSelectDepth(base::SelectDepth level);
		std::vector<unsigned char> CurrentHover() const;
		bool UpdateCurrentHover(std::vector<unsigned char> path,
			base::Action::Modifiers modifiers,
			bool isSelected,
			base::Tweakable::TweakState tweakState);

	protected:
		bool IsHovering(std::vector<unsigned char> path) const;

		void StartPaintSelection(SelectMode mode, std::vector<unsigned char> selection);

		void StartRectSelection(SelectMode mode, utils::Position2d pos);
		void UpdateRectSelection(utils::Position2d pos);

		void EndSelection();

	protected:
		bool _isSelecting;
		SelectMode _selectMode;
		base::SelectDepth _selectDepth;
		SelectionTool _selectionTool;
		utils::Position2d _initPos;
		utils::Position2d _currentPos;
		std::vector<unsigned char> _currentHover;
		bool _currentHoverSelected;
		base::Tweakable::TweakState _currentHoverTweakState;
	};
}
