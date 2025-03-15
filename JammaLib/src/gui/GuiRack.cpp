#include "GuiToggle.h"
#include "GuiRack.h"
using namespace graphics;

using namespace base;
using namespace gui;
using namespace actions;
using namespace resources;
using namespace utils;

const utils::Size2d GuiRack::_SliderGap = { 4, 4 };
const unsigned int GuiRack::_ChannelTogglePaddingLeft = 35;
const double GuiRack::_RouterHeightFrac = 0.8;
const utils::Size2d GuiRack::_ChannelToggleSize = { 32, 64 };
const utils::Size2d GuiRack::_RouterToggleSize = { 64, 32 };
const utils::Size2d GuiRack::_DragGap = { 4, 4 };
const utils::Size2d GuiRack::_DragSize = { 32, 32 };

GuiRack::GuiRack(GuiRackParams params) :
	GuiElement(params),
	_rackState(params.InitState),
	_masterPanel(nullptr),
	_masterSlider(nullptr),
	_channelToggle(nullptr),
	_channelPanel(nullptr),
	_routerToggle(nullptr),
	_routerPanel(nullptr),
	_rackParams({}),
	_receiver(params.Receiver)
{
	_masterPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_MASTER, params.Size));
	_masterSlider = std::make_shared<gui::GuiSlider>(_GetSliderParams(0, params.Size));
	_channelToggle = std::make_shared<gui::GuiToggle>(_GetToggleParams(GuiRackParams::RACK_CHANNELS, params.Size));
	_channelPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_CHANNELS, params.Size));
	_routerToggle = std::make_shared<gui::GuiToggle>(_GetToggleParams(GuiRackParams::RACK_ROUTER, params.Size));
	_routerPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_ROUTER, params.Size));

	_router = std::make_shared<gui::GuiRouter>(_GetRouterParams(params.Size),
		params.NumChannels,
		params.NumChannels);

	_children.push_back(_masterPanel);
	_children.push_back(_masterSlider);
	_children.push_back(_channelToggle);
	_children.push_back(_routerToggle);
	_children.push_back(_routerPanel);

	SetNumChannels(params.NumChannels);
}


void GuiRack::SetSize(utils::Size2d size)
{
	auto panelParams = _GetPanelParams(GuiRackParams::RACK_MASTER, size);
	_masterPanel->SetSize(panelParams.Size);
	auto sliderParams = _GetSliderParams(0, size);
	_masterSlider->SetSize(sliderParams.Size);
	auto toggleParams = _GetToggleParams(GuiRackParams::RACK_CHANNELS, size);
	_channelToggle->SetSize(toggleParams.Size);

	GuiElement::SetSize(size);
}

ActionResult GuiRack::OnAction(GuiAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	auto receiver = _receiver.lock();

	switch (action.ElementType)
	{
	case GuiAction::ACTIONELEMENT_TOGGLE:
		_rackState = action.Index < GuiRackParams::RACK_ROUTER ?
			(GuiRackParams::RackState)action.Index : GuiRackParams::RACK_MASTER;
		_OnRackChange(false);
		break;
	case GuiAction::ACTIONELEMENT_SLIDER:
		if (receiver)
			receiver->OnAction(action);

		break;
	case GuiAction::ACTIONELEMENT_ROUTER:
		if (receiver)
			receiver->OnAction(action);

		break;
	}

	return ActionResult::NoAction();
}

GuiRackParams::RackState GuiRack::GetRackState() const
{
	return _rackState;
}

void GuiRack::SetRackState(GuiRackParams::RackState state, bool bypassUpdates)
{
	_rackState = state;

	_OnRackChange(bypassUpdates);
}

void GuiRack::_InitReceivers()
{
	_masterSlider->SetReceiver(ActionReceiver::shared_from_this());
	_masterSlider->SetValue(_masterSlider->Value());

	_channelToggle->SetReceiver(ActionReceiver::shared_from_this());

	for (auto& slider : _channelSliders)
	{
		slider->SetReceiver(ActionReceiver::shared_from_this());
	}

	_routerToggle->SetReceiver(ActionReceiver::shared_from_this());
	_router->SetReceiver(ActionReceiver::shared_from_this());
}

void GuiRack::_AddChannel(unsigned int index, utils::Size2d size)
{
	auto slider = std::make_shared<gui::GuiSlider>(_GetSliderParams(index, size));
	_channelSliders.push_back(slider);
	_children.push_back(slider);
}

void gui::GuiRack::_OnRackChange(bool bypassUpdates)
{
	switch (_rackState)
	{
	case GuiRackParams::RACK_MASTER:
		_masterPanel->SetVisible(true);
		_channelToggle->SetToggleState(GuiToggleParams::TOGGLE_OFF, bypassUpdates);
		_channelPanel->SetVisible(false);
		_routerToggle->SetToggleState(GuiToggleParams::TOGGLE_OFF, bypassUpdates);
		_routerPanel->SetVisible(false);
		break;
	case GuiRackParams::RACK_CHANNELS:
		_masterPanel->SetVisible(true);
		_channelToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, bypassUpdates);
		_channelPanel->SetVisible(true);
		_routerToggle->SetToggleState(GuiToggleParams::TOGGLE_OFF, bypassUpdates);
		_routerPanel->SetVisible(false);
		break;
	case GuiRackParams::RACK_ROUTER:
		_masterPanel->SetVisible(true);
		_channelToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, bypassUpdates);
		_channelPanel->SetVisible(true);
		_routerToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, bypassUpdates);
		_routerPanel->SetVisible(true);
		break;
	}

	//if (_receiver && !bypassUpdates)
	//{
	//	GuiAction action;
	//	action.ElementType = GuiAction::ACTIONELEMENT_RACK;
	//	action.Index = _index;
	//	action.Data = GuiAction::GuiInt(_rackState);
	//	_receiver->OnAction(action);
	//}
}

utils::Size2d GuiRack::_CalcSliderSize(utils::Size2d size)
{
	return {
		size.Width - _ChannelToggleSize.Width - _ChannelTogglePaddingLeft - (2u * _SliderGap.Width),
		size.Height - (2 * _SliderGap.Height)
	};
}

base::GuiElementParams gui::GuiRack::_GetPanelParams(GuiRackParams::RackState state, utils::Size2d size)
{
	GuiElementParams params;

	auto sliderSize = _CalcSliderSize(size);

	switch (state)
	{
	case GuiRackParams::RACK_MASTER:
		params.Size = { size.Width, size.Height };
		params.Position = { 0, 0 };
		break;
	case GuiRackParams::RACK_CHANNELS:
		params.Size = { _CalcChannelPannelWidth(sliderSize),
			size.Height };
		params.Position = { (int)size.Width, 0 };
		break;
	}

	return params;
}

unsigned int gui::GuiRack::_CalcChannelPannelWidth(utils::Size2d sliderSize)
{
	return _rackParams.NumChannels * (sliderSize.Width + _SliderGap.Width);
}

gui::GuiSliderParams gui::GuiRack::_GetSliderParams(unsigned int index, utils::Size2d size)
{
	auto sliderSize = _CalcSliderSize(size);

	GuiSliderParams sliderParams;
	sliderParams.Min = 0.0;
	sliderParams.Max = 6.0;
	sliderParams.InitValue = _rackParams.InitLevel;
	sliderParams.Orientation = GuiSliderParams::SLIDER_VERTICAL;

	sliderParams.Size = sliderSize;
	sliderParams.MinSize = { std::max(40u,sliderParams.Size.Width), std::max(40u, sliderParams.Size.Height) };

	if (0 == index)
	{
		sliderParams.Position = { (int)_SliderGap.Width, (int)_SliderGap.Height };
	}
	else
	{
		sliderParams.Position = {
			((int)_SliderGap.Width + (int)sliderSize.Width) * (int)index,
			(int)_SliderGap.Height
		};
	}

	sliderParams.DragControlOffset = { (int)(sliderParams.Size.Width / 2) - (int)(_DragSize.Width / 2), (int)_DragGap.Height };
	sliderParams.DragControlSize = _DragSize;
	sliderParams.DragGap = _DragGap;
	sliderParams.Texture = "fader_back";
	sliderParams.DragTexture = "fader";
	sliderParams.DragOverTexture = "fader_over";

	return sliderParams;
}

gui::GuiToggleParams GuiRack::_GetToggleParams(GuiRackParams::RackState state, utils::Size2d size)
{
	GuiToggleParams toggleParams;

	auto sliderSize = _CalcSliderSize(size);

	switch (state)
	{
	case GuiRackParams::RACK_CHANNELS:
		toggleParams.Position = {
			(int)(size.Width - _ChannelToggleSize.Width),
			((int)size.Height - (int)_ChannelToggleSize.Height) / 2 };
		toggleParams.Size = _ChannelToggleSize;
		break;
	case GuiRackParams::RACK_ROUTER:
		toggleParams.Position = {
			(int)size.Width + (((int)_CalcChannelPannelWidth(sliderSize) - (int)_RouterToggleSize.Width) / 2),
			 -(int)_RouterToggleSize.Height
		};
		toggleParams.Size = _RouterToggleSize;
		break;
	}

	toggleParams.MinSize = toggleParams.Size;
	toggleParams.Texture = "arrow";
	toggleParams.OverTexture = "arrow_over";
	toggleParams.DownTexture = "arrow_down";
	toggleParams.ToggledTexture = "arrowup2";
	toggleParams.ToggledOverTexture = "arrowup2_over";
	toggleParams.ToggledDownTexture = "arrowup2_down";

	return toggleParams;
}

gui::GuiRouterParams GuiRack::_GetRouterParams(utils::Size2d size)
{
	GuiRouterParams routerParams;

	auto sliderSize = _CalcSliderSize(size);

	routerParams.Size = {
		_CalcChannelPannelWidth(sliderSize),
		(unsigned int)((double)size.Height * _RouterHeightFrac)
	};
	routerParams.Position = { (int)_rackParams.Size.Width, -(int)routerParams.Size.Height };
	routerParams.MinSize = routerParams.Size;
	routerParams.InputType = GuiRouterParams::CHANNEL_BUS;
	routerParams.OutputType = GuiRouterParams::CHANNEL_DEVICE;
	routerParams.InputSpacing = GuiRouterParams::BusWidth + GuiRouterParams::BusGap;
	routerParams.InputSize = GuiRouterParams::BusWidth;
	routerParams.OutputSpacing = GuiRouterParams::BusWidth + GuiRouterParams::BusGap;
	routerParams.OutputSize = GuiRouterParams::BusWidth;
	routerParams.Texture = "router";
	routerParams.PinTexture = "";
	routerParams.LinkTexture = "";
	routerParams.DeviceInactiveTexture = "router";
	routerParams.DeviceActiveTexture = "router_inactive";
	routerParams.ChannelInactiveTexture = "router";
	routerParams.ChannelActiveTexture = "router_inactive";
	routerParams.OverTexture = "router_over";
	routerParams.DownTexture = "router_down";
	routerParams.HighlightTexture = "router_over";
	routerParams.LineShader = "colour";

	return routerParams;
}

unsigned int GuiRack::NumChannels() const
{
	return (unsigned int)_channelSliders.size();
}

void GuiRack::SetNumChannels(unsigned int channels)
{
	auto current = (unsigned int)_channelSliders.size();

	if (current > channels)
		_channelSliders.erase(_channelSliders.begin() + channels, _channelSliders.end());
	else if (current < channels)
	{
		for (auto i = current; i < channels; ++i)
		{
			_AddChannel(i, _rackParams.Size);
		}
	}

	_router->SetNumInputs(channels);
}
