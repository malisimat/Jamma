#pragma once

#include "Action.h"
#include "TouchAction.h"

namespace actions
{
	class TouchMoveAction :
		public base::Action
	{
	public:
		TouchMoveAction();
		~TouchMoveAction();

	public:
		TouchAction::TouchType Touch;
		int Index;
		unsigned int MouseButtonsDown;
		utils::Position2d Position;
		// Device-relative movement; Y is positive upward, Position remains anchored.
		bool IsRelative = false;
		utils::Position2d RelativeDelta{};
		Modifiers Modifiers;
	};
}
