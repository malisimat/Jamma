#include "GuiAction.h"

using namespace actions;

GuiAction::GuiAction() :
	Data{}
{
}

GuiAction::~GuiAction()
{
}

GuiActionUndo::GuiActionUndo(double value,
	std::weak_ptr<base::ActionSender> sender) :
	_value(value),
	ActionUndo(sender)
{
}

GuiActionUndo::~GuiActionUndo()
{
}

double GuiActionUndo::Value() const
{
	return _value;
}
