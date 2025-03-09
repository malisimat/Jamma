#include "GuiRadio.h"

using namespace graphics;

using namespace base;
using namespace gui;
using namespace actions;
using namespace resources;
using namespace utils;

const unsigned int GuiRadio::_ToggleWidth = 128;
const unsigned int GuiRadio::_ToggleHeight = 64;
const unsigned int GuiRadio::_ToggleGap = 0;

GuiRadio::GuiRadio(GuiRadioParams params) :
	GuiButton(params),
	_currentValue(0u),
	_radioParams(params)
{
	unsigned int count = 0;

	for (auto& params : _radioParams.ToggleParams)
	{
		std::cout << "Type: " << typeid(params).name() << std::endl;

		params.Index = count;

		_children.push_back(std::make_shared<GuiToggle>(params));

		count++;
	}

	_OnRadioValueChange(params.InitValue, true);
}

ActionResult GuiRadio::OnAction(GuiAction action)
{
	if (GuiAction::ACTIONELEMENT_TOGGLE == action.ElementType)
	{
		if (action.Index < _children.size())
		{
			_OnRadioValueChange(action.Index, false);

			ActionResultType resultType = ACTIONRESULT_RADIO;
			auto source = std::to_string(_index);

			return {
				true,
				source,
				"",
				resultType,
				nullptr,
				std::static_pointer_cast<base::GuiElement>(shared_from_this())
			};
		}
	}

	return ActionResult::NoAction();
}

unsigned int GuiRadio::CurrentValue() const
{
	return _currentValue;
}

void GuiRadio::SetCurrentValue(unsigned int value, bool bypassUpdates)
{
	_OnRadioValueChange(value, bypassUpdates);
}

void GuiRadio::_InitReceivers()
{
	for (unsigned int i = 0; i < _children.size(); i++)
	{
		_children[i]->SetParent(shared_from_this());

		auto toggle = std::dynamic_pointer_cast<GuiToggle>(_children[i]);

		if (toggle)
			toggle->SetReceiver(ActionReceiver::shared_from_this());
	}
}

void GuiRadio::_OnRadioValueChange(unsigned int value, bool bypassUpdates)
{
	_currentValue = value;

	for (unsigned int i = 0; i < _children.size(); i++)
	{
		auto toggle = std::dynamic_pointer_cast<GuiToggle>(_children[i]);

		if (toggle)
		{
			if (i == _currentValue)
				toggle->SetToggleState(GuiToggleParams::TOGGLE_ON, true);
			else
				toggle->SetToggleState(GuiToggleParams::TOGGLE_OFF, true);
		}
	}

	if (_receiver && !bypassUpdates)
	{
		GuiAction action;
		action.ElementType = GuiAction::ACTIONELEMENT_RADIO;
		action.Index = _index;
		action.Data = GuiAction::GuiInt(_currentValue);
		_receiver->OnAction(action);
	}
}