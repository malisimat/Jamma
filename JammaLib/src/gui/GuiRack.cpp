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
const unsigned int GuiRack::_RouterSpacingY = 64;
const utils::Size2d GuiRack::_ChannelToggleSize = { 32, 64 };
const utils::Size2d GuiRack::_RouterToggleSize = { 64, 32 };
const unsigned int GuiRack::_RouterTogglePaddingBottom = 8;
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
	_router(nullptr),
	_rackParams(params)
{
	_masterPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_MASTER, params.Size));
	_masterSlider = std::make_shared<gui::GuiSlider>(_GetSliderParams(0, params.Size));
	_channelToggle = std::make_shared<gui::GuiToggle>(_GetToggleParams(GuiRackParams::RACK_CHANNELS, params.Size));
	_channelPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_CHANNELS, params.Size));
	_routerToggle = std::make_shared<gui::GuiToggle>(_GetToggleParams(GuiRackParams::RACK_ROUTER, params.Size));
	_routerPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_ROUTER, params.Size));

	_router = std::make_shared<gui::GuiRouter>(_GetRouterParams(params.Size),
		params.NumInputChannels,
		params.NumOutputChannels);

	_children.push_back(_masterPanel);
	_children.push_back(_masterSlider);
	_masterPanel->AddChild(_channelToggle);
	_masterPanel->AddChild(_channelPanel);
	_channelPanel->AddChild(_routerToggle);
	_channelPanel->AddChild(_routerPanel);
	_routerPanel->AddChild(_router);

	SetNumInputChannels(params.NumInputChannels);
	SetNumOutputChannels(params.NumOutputChannels);

	_OnRackChange((unsigned int)params.InitState, true);
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

	bool toggleOn;
	bool routerVisible = _routerPanel->IsVisible();
	GuiRackParams::RackState newRackState;

	if (_receiver)
	{
		switch (action.ElementType)
		{
		case GuiAction::ACTIONELEMENT_TOGGLE:
			toggleOn = false;
			if (auto i = std::get_if<GuiAction::GuiInt>(&action.Data))
				toggleOn = (1 == i->Value);

			newRackState = action.Index <= GuiRackParams::RACK_ROUTER ?
				(GuiRackParams::RackState)action.Index :
				GuiRackParams::RACK_MASTER;
			
			switch (newRackState)
			{
			case GuiRackParams::RACK_ROUTER:
				_rackState = toggleOn ? GuiRackParams::RACK_ROUTER : GuiRackParams::RACK_CHANNELS;
				break;
			case GuiRackParams::RACK_CHANNELS:
				if (routerVisible && toggleOn)
					_rackState = GuiRackParams::RACK_ROUTER;
				else
					_rackState = toggleOn ? GuiRackParams::RACK_CHANNELS : GuiRackParams::RACK_MASTER;

				break;
			}
			_OnRackChange(action.Index, true);
			break;
		case GuiAction::ACTIONELEMENT_SLIDER:
			action.ElementType = GuiAction::ACTIONELEMENT_RACK;
			if (action.Index > 0)
				action.Index -= 1; // Account for other children of _channelPanel

			if (_receiver)
				_receiver->OnAction(action);

			break;
		case GuiAction::ACTIONELEMENT_ROUTER:
			action.ElementType = GuiAction::ACTIONELEMENT_RACK;
			if (_receiver)
				_receiver->OnAction(action);

			break;
		}
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

	_OnRackChange(0, bypassUpdates);
}

void GuiRack::_InitReceivers()
{
	_masterSlider->SetReceiver(ActionReceiver::shared_from_this());
	_masterSlider->SetValue(_masterSlider->Value(), true);

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
	_channelPanel->AddChild(slider);
}

void GuiRack::_OnRackChange(unsigned int index, bool bypassUpdates)
{
	switch (_rackState)
	{
	case GuiRackParams::RACK_MASTER:
		_masterPanel->SetVisible(true);
		_channelToggle->SetToggleState(GuiToggleParams::TOGGLE_OFF, 1 == index ? true : bypassUpdates);
		_channelPanel->SetVisible(false);
		_routerPanel->SetVisible(false);
		break;
	case GuiRackParams::RACK_CHANNELS:
		_masterPanel->SetVisible(true);
		_channelToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, 1 == index ? true : bypassUpdates);
		_channelPanel->SetVisible(true);
		_routerToggle->SetToggleState(GuiToggleParams::TOGGLE_OFF, 2 == index ? true : bypassUpdates);
		_routerPanel->SetVisible(false);
		break;
	case GuiRackParams::RACK_ROUTER:
		_masterPanel->SetVisible(true);
		_channelToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, 1 == index ? true : bypassUpdates);
		_channelPanel->SetVisible(true);
		_routerToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, 2 == index ? true : bypassUpdates);
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

base::GuiElementParams GuiRack::_GetPanelParams(GuiRackParams::RackState state, utils::Size2d size)
{
	GuiElementParams params;

	auto sliderSize = _CalcSliderSize(size);

	switch (state)
	{
	case GuiRackParams::RACK_ROUTER:
		params.Size = { _CalcChannelPannelWidth(sliderSize),
			_CalcRouterHeight(size)};
		params.Position = { 0, -(int)(params.Size.Height + _RouterToggleSize.Height + _RouterTogglePaddingBottom) };
		break;
	case GuiRackParams::RACK_CHANNELS:
		params.Size = { _CalcChannelPannelWidth(sliderSize),
			size.Height };
		params.Position = { (int)size.Width, 0 };
		break;
	default:
		params.Size = { size.Width, size.Height };
		params.Position = { 0, 0 };
		break;
	}

	return params;
}

unsigned int GuiRack::_CalcChannelPannelWidth(utils::Size2d sliderSize)
{
	return _rackParams.NumInputChannels * (sliderSize.Width + _SliderGap.Width);
}

unsigned int GuiRack::_CalcRouterHeight(utils::Size2d size)
{
	auto sliderSize = _CalcSliderSize(size);
	return sliderSize.Width + _RouterSpacingY;
}

gui::GuiSliderParams GuiRack::_GetSliderParams(unsigned int index, utils::Size2d size)
{
	auto sliderSize = _CalcSliderSize(size);

	GuiSliderParams sliderParams;
	sliderParams.Index = index;
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
			((int)_SliderGap.Width + (int)sliderSize.Width) * (int)(index-1),
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
		toggleParams.ToggleIndex = 1;
		toggleParams.Position = {
			(int)(size.Width - _ChannelToggleSize.Width),
			((int)size.Height - (int)_ChannelToggleSize.Height) / 2 };
		toggleParams.Size = _ChannelToggleSize;
		toggleParams.Rot90 = true;
		break;
	case GuiRackParams::RACK_ROUTER:
		toggleParams.ToggleIndex = 2;
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
		_CalcRouterHeight(size)
	};
	routerParams.Position = { 0, 0 };
	routerParams.MinSize = routerParams.Size;
	routerParams.InputType = GuiRouterParams::CHANNEL_BUS;
	routerParams.OutputType = GuiRouterParams::CHANNEL_DEVICE;
	routerParams.InputSpacing = sliderSize.Width + _SliderGap.Width;
	routerParams.InputSize = sliderSize.Width;
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

unsigned int GuiRack::NumInputChannels() const
{
	return (unsigned int)_channelSliders.size();
}

unsigned int GuiRack::NumOutputChannels() const
{
	if (nullptr != _router)
		return _router->NumOutputs();

	return 0u;
}

void GuiRack::SetNumInputChannels(unsigned int channels)
{
	auto current = (unsigned int)_channelSliders.size();

	if (current > channels)
		_channelSliders.erase(_channelSliders.begin() + channels, _channelSliders.end());
	else if (current < channels)
	{
		for (auto i = current; i < channels; ++i)
		{
			_AddChannel(i+1, _rackParams.Size);
		}
	}

	_router->SetNumInputs(channels);
}

void GuiRack::SetNumOutputChannels(unsigned int channels)
{
	_router->SetNumOutputs(channels);
}

void GuiRack::AddRoute(unsigned int inputChan, unsigned int outputChan)
{
	_router->AddRoute(inputChan, outputChan);
}

void GuiRack::ClearRoutes()
{
	_router->ClearRoutes();
}
