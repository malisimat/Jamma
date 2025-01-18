#pragma once

#include "Action.h"
#include "CommonTypes.h"

namespace actions
{
	class TouchAction :
		public base::Action
	{
	public:
		TouchAction();
		~TouchAction();

	public:
		enum TouchState
		{
			TOUCH_DOWN,
			TOUCH_UP
		};

		enum TouchType
		{
			TOUCH_MOUSE,
			TOUCH_FINGER
		};

	public:
		TouchState State;
		TouchType Touch;
		int Index;
		int Value;
		utils::Position2d Position;
		Modifiers Modifiers;
	};
}
