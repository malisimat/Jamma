#include "GuiFocusManager.h"

using namespace gui;
using base::GuiElement;

bool GuiFocusManager::RequestFocus(const std::shared_ptr<GuiElement>& element)
{
	auto current = _focus.lock();
	if (current == element)
		return element != nullptr;

	if (current)
		current->ClearFocus();

	if (!element)
	{
		_focus.reset();
		return false;
	}

	if (!element->RequestFocus())
	{
		_focus.reset();
		return false;
	}

	_focus = element;
	return true;
}

void GuiFocusManager::ClearFocus()
{
	if (auto current = _focus.lock())
		current->ClearFocus();

	_focus.reset();
}

bool GuiFocusManager::HasFocus(const std::shared_ptr<GuiElement>& element) const
{
	return element && (_focus.lock() == element);
}

std::shared_ptr<GuiElement> GuiFocusManager::CurrentFocus() const
{
	return _focus.lock();
}

bool GuiFocusManager::IsEditingText() const
{
	auto current = _focus.lock();
	return current && current->IsTextEditing();
}
