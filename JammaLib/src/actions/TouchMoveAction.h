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
		utils::Position2d Position;
		Modifiers Modifiers;
	};
}
