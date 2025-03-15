#pragma once

#include <vector>
#include "Action.h"
#include "../base/ActionSender.h"
#include "../base/ActionUndo.h"
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
			ACTIONELEMENT_SLIDER,
			ACTIONELEMENT_RADIO,
			ACTIONELEMENT_ROUTER
		};

		struct GuiInt {
			int Value;
		};

		struct GuiDouble {
			double Value;
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
		using GuiData = std::variant<GuiInt, GuiDouble, GuiString, GuiIntArray, GuiConnections>;

		GuiAction();
		~GuiAction();

	public:
		unsigned int Index;
		ActionElementType ElementType;
		GuiData Data;
	};


	class GuiActionUndo :
		public base::ActionUndo
	{
	public:
		GuiActionUndo(double value,
			std::weak_ptr<base::ActionSender> sender);
		~GuiActionUndo();

	public:
		virtual base::UndoType UndoType() const override
		{
			return base::UNDO_DOUBLE;
		}

		double Value() const;

	public:
		double _value;
	};
}
