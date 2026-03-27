#include "GuiPanel.h"

using namespace base;
using namespace gui;

GuiPanel::GuiPanel(GuiElementParams params) :
	GuiElement(params)
{
}

bool GuiPanel::AddChild(const std::shared_ptr<base::GuiElement>& child)
{
	auto it = std::find(_children.begin(), _children.end(), child);

	if (it == _children.end())
	{
		_children.push_back(child);
		return true;
	}

	return false;
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
