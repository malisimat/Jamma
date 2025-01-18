#pragma once

#include "Action.h"

namespace actions
{
	class KeyAction :
		public base::Action
	{
	public:
		KeyAction();
		~KeyAction();

	public:
		enum KeyActionType
		{
			KEY_UP,
			KEY_DOWN
		};

	public:
		KeyActionType KeyActionType;
		unsigned int KeyChar;
		bool IsSystem;
		Action::Modifiers Modifiers;
	};
}
