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
		GuiData Data;
	};
}
