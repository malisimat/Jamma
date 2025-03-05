#pragma once

#include <vector>
#include "Action.h"
#include "CommonTypes.h"

namespace actions
{

	class GuiAction :
		public base::Action
	{
	public:
		enum ActionElementType
		{
			ACTIONELEMENT_TOGGLE,
			ACTIONELEMENT_ROUTER
		};

		struct GuiInt {
			int Value;
		};

		struct GuiFloat {
			float Value;
		};

		struct GuiString {
			std::string Value;
		};

		struct GuiIntArray {
			std::vector<int> Values;
		};

		struct GuiConnections {
			std::vector<std::pair<unsigned int, unsigned int>> Connections;
		};

	public:
		using GuiData = std::variant<GuiInt, GuiFloat, GuiString, GuiIntArray, GuiConnections>;

		GuiAction();
		~GuiAction();

	public:
		unsigned int Index;
		ActionElementType ElementType;
		GuiData Data;
	};
}
