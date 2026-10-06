#include "GuiPanel.h"

using namespace base;
using namespace gui;

actions::ActionResult GuiCommandReceiver::OnAction(actions::GuiAction action)
{
	if (auto owner = _owner.lock())
	{
		action.Index = _commandIndex;
		return owner->OnAction(action);
	}
	return actions::ActionResult::NoAction();
}

GuiPanel::GuiPanel(GuiElementParams params) :
	GuiElement(params)
{
}

void GuiPanel::AddChild(std::shared_ptr<base::GuiElement> child)
{
	GuiElement::AddChild(child);
}

bool GuiPanel::RemoveChild(const std::shared_ptr<base::GuiElement>& child)
{
	auto it = std::find(_children.begin(), _children.end(), child);
	
	if (it != _children.end())
	{
		_children.erase(it);
		return true;
	}

	return false;
}
