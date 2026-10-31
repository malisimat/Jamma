#pragma once

#include <array>
#include <optional>
#include <vector>
#include <algorithm>
#include "../base/Jammable.h"
#include "../base/Action.h"

namespace gui
{
	class GuiSelectorParams : public base::GuiElementParams {};

	class SceneSelector :
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
		SceneSelector(GuiSelectorParams guiParams);

	public:
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;

		// Empty-background click commands latch their button/modifiers at press.
		// Movement cancels the click while keeping pointer ownership until release.
		enum class BackgroundCommand { UnmuteAll, MuteAll, SelectAll };
		bool BeginBackgroundClick(const actions::TouchAction& action);
		bool OwnsBackgroundClick() const { return _backgroundClick.has_value(); }
		std::optional<BackgroundCommand> BackgroundPreview() const;
		std::optional<BackgroundCommand> ReleaseBackgroundClick(const actions::TouchAction& action);
		void MoveBackgroundClick(const actions::TouchMoveAction& action, bool allowed);
		void CancelBackgroundClick() { if (_backgroundClick) _backgroundClick->Cancelled = true; }
		void ClearBackgroundClick() { _backgroundClick.reset(); }
		static std::vector<unsigned char> PathAtDepth(std::vector<unsigned char> path, unsigned int depth);

		enum class HierarchyEdit { Select, Deselect, Mute, Unmute };
		struct TargetChange { HierarchyEdit Edit; std::vector<unsigned char> Path; };
		struct SelectionUpdate
		{
			bool ClearSelection = false;
			bool ClearPicking = false;
			bool HighlightHover = false;
			bool HighlightMidiSiblings = false;
			bool ClickPressed = false;
			bool MutePressed = false;
			std::vector<unsigned char> HoverPath;
			std::array<TargetChange, 2> Changes{};
			std::size_t ChangeCount = 0u;
		};
		SelectionUpdate SelectionChanges(actions::ActionResultType result) const;
		SelectMode CurrentMode() const;
		base::SelectDepth CurrentSelectDepth() const;
		void SetSelectDepth(base::SelectDepth level);
		std::vector<unsigned char> CurrentHover() const;
		std::vector<unsigned char> PaintedPathForTest() const;
		void ClearPointerState() override;
		bool IsClickPressed() const;
		bool IsMutePressed() const;
		bool IsPaintingMute() const;
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
		struct BackgroundClick
		{
			int Button;
			BackgroundCommand Command;
			utils::Position2d PressPosition;
			bool Cancelled = false, Moved = false;
		};
		std::optional<BackgroundClick> _backgroundClick;
		bool _isSelecting;
		SelectMode _selectMode;
		base::SelectDepth _selectDepth;
		SelectionTool _selectionTool;
		utils::Position2d _initPos;
		utils::Position2d _currentPos;
		std::vector<unsigned char> _currentHover;
		std::vector<unsigned char> _paintedPath;
		bool _currentHoverSelected;
		bool _pressHoverSelected;
		bool _clickPressed;
		base::Tweakable::TweakState _currentHoverTweakState;
	};
}
