#include "GuiToggle.h"
#include "GuiRack.h"
#include "GuiVstRack.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
using namespace graphics;

using namespace base;
using namespace gui;
using namespace actions;
using namespace resources;
using namespace utils;

namespace gui
{
	class GuiStationMidiToggle : public GuiToggle
	{
	public:
		explicit GuiStationMidiToggle(GuiToggleParams params) : GuiToggle(params) {}
		void Draw(base::DrawContext& context) override
		{
			const auto tint = _guiParams.TintColor;
			if (GetToggleState() == GuiToggleParams::TOGGLE_OFF && GetState() != STATE_DOWN)
				_guiParams.TintColor = glm::vec3(0.45f);
			GuiToggle::Draw(context); _guiParams.TintColor = tint;
		}
	};
}

const utils::Size2d GuiRack::_SliderGap = { 8, 4 };
const unsigned int GuiRack::_ChannelTogglePaddingLeft = 18;
const unsigned int GuiRack::_ChannelExpanderInset = 8;
const unsigned int GuiRack::_RouterSpacingY = 64;
const utils::Size2d GuiRack::_ChannelToggleSize = { 32, 64 };
const utils::Size2d GuiRack::_RouterToggleSize = { 64, 32 };
const unsigned int GuiRack::_RouterTogglePaddingBottom = 8;
const utils::Size2d GuiRack::_DragGap = { 4, 4 };
const utils::Size2d GuiRack::_MidiChannelToggleSize = { 42, 42 };
const utils::Size2d GuiRack::_MidiChannelToggleGap = { 6, 6 };
const unsigned int GuiRack::_MidiChannelPanelPadding = 8;

GuiRack::GuiRack(GuiRackParams params) :
	GuiElement(params),
	_receiversInitialized(false),
	_rackState(params.InitState),
	_masterPanel(nullptr),
	_masterSlider(nullptr),
	_channelToggle(nullptr),
	_channelPanel(nullptr),
	_routerToggle(nullptr),
	_routerPanel(nullptr),
	_router(nullptr),
	_midiChannelPanel(nullptr),
	_midiChannelToggles(),
	_rackParams(params)
{
	const auto vstSpace = params.EnableVstControl ? GuiButtonParams::DefaultHeight + _SliderGap.Height : 0u;
	params.Size.Height = std::max(params.Size.Height, _MinimumScaleLayoutHeight + vstSpace);
	const auto minFaderWidth = params.EnableVstControl ? 64u : 40u;
	params.Size.Width = std::max(params.Size.Width, minFaderWidth + _ChannelToggleSize.Width + _ChannelTogglePaddingLeft + 2u * _SliderGap.Width);
	_rackParams.Size = params.Size;
	_masterPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_MASTER, params.Size));
	_masterSlider = std::make_shared<gui::GuiSlider>(_GetSliderParams(0, params.Size));
	_channelToggle = std::make_shared<gui::GuiToggle>(_GetToggleParams(GuiRackParams::RACK_CHANNELS, params.Size));
	_channelPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_CHANNELS, params.Size));
	_routerToggle = std::make_shared<gui::GuiToggle>(_GetToggleParams(GuiRackParams::RACK_ROUTER, params.Size));
	_routerPanel = std::make_shared<base::GuiElement>(_GetPanelParams(GuiRackParams::RACK_ROUTER, params.Size));
	_midiChannelPanel = std::make_shared<base::GuiElement>(base::GuiElementParams());

	_router = std::make_shared<gui::GuiRouter>(_GetRouterParams(params.Size),
		params.NumInputChannels,
		params.NumOutputChannels);

	for (auto channel = 0u; channel < 16u; ++channel)
	{
		auto toggleParams = gui::GuiToggleParams::PanelSecondary();
		toggleParams.ToggleIndex = MidiChannelToggleBaseIndex + channel;
		toggleParams.Size = _MidiChannelToggleSize;
		toggleParams.MinSize = _MidiChannelToggleSize;
		toggleParams.Position = { 0, 0 };
		toggleParams.Text = std::to_string(channel + 1u);
		toggleParams.Rot90 = false;
		if (params.EnableVstRackMidiRouting)
		{
			toggleParams.TextFontSize = graphics::FontOptions::FONT_SMALL;
			toggleParams.TextPadding = 2u;
		}
		std::shared_ptr<GuiToggle> toggle = params.EnableVstRackMidiRouting ?
			std::make_shared<GuiStationMidiToggle>(toggleParams) : std::make_shared<GuiToggle>(toggleParams);
		_midiChannelToggles.push_back(toggle);
		_midiChannelPanel->AddChild(toggle);
	}

	_children.push_back(_masterPanel);
	_masterPanel->AddChild(_masterSlider);
	if (params.EnableStationLabel)
	{
		_stationLabel = std::make_shared<GuiStationLabel>();
		_masterPanel->AddChild(_stationLabel);
		_stationLabel->SetVisible(true);
	}
	if (params.EnableVstControl && !params.EnableVstRack)
	{
		const std::array<std::string, 4> names = { "vst_empty", "vst_closed", "vst_open", "vst_failed" };
		for (size_t i = 0; i < _vstControls.size(); ++i)
		{
			GuiButtonParams button;
			button.Index = VstControlActionIndex;
			button.Text = "VST";
			button.TextFontSize = graphics::FontOptions::FONT_SMALL;
			button.TextPadding = 2;
			button.DispatchCommand = true;
			button.CapturePressModifiers = true;
			button.Texture = names[i];
			button.OverTexture = names[i] + "_over";
			button.DownTexture = names[i] + "_down";
			button.Size = { 40u, GuiButtonParams::DefaultHeight };
			button.MinSize = button.Size;
			_vstControls[i] = std::make_shared<GuiButton>(button);
			_masterPanel->AddChild(_vstControls[i]);
		}
		SetVstControlState(VstControlState::Empty);
	}
	_masterPanel->AddChild(_channelToggle);
	_masterPanel->AddChild(_channelPanel);
	if (params.EnableVstRack)
	{
		_vstRack = params.EnableVstRackMidiRouting ? std::make_shared<GuiVstRack>(_midiChannelPanel, _midiChannelToggles) : std::make_shared<GuiVstRack>();
		_masterPanel->AddChild(_vstRack);
	}
	if (!params.EnableVstRackMidiRouting) _channelPanel->AddChild(_midiChannelPanel);
	_channelPanel->AddChild(_routerToggle);
	_channelPanel->AddChild(_routerPanel);
	_routerPanel->AddChild(_router);

	SetNumInputChannels(params.NumInputChannels);
	SetNumOutputChannels(params.NumOutputChannels);
	SetAllowedMidiChannels(params.AllowedMidiChannels, true);

	_OnRackChange((unsigned int)params.InitState, true);
	SetSize(params.Size);
}


void GuiRack::SetSize(utils::Size2d size)
{
	// The silence, unity and maximum anchors need distinct pixel rows even on
	// compact layouts. This leaves at least three pixels of handle travel.
	const auto vstSpace = _rackParams.EnableVstControl ? GuiButtonParams::DefaultHeight + _SliderGap.Height : 0u;
	size.Height = std::max(size.Height, _MinimumScaleLayoutHeight + vstSpace);
	// Leave room for the small Loop target hint as well as the VST glyphs.
	const auto minFaderWidth = _rackParams.EnableVstControl ? 64u : 40u;
	size.Width = std::max(size.Width, minFaderWidth + _ChannelToggleSize.Width + _ChannelTogglePaddingLeft + 2u * _SliderGap.Width);
	_rackParams.Size = size;

	auto masterPanelParams = _GetPanelParams(GuiRackParams::RACK_MASTER, size);
	_masterPanel->SetPosition(masterPanelParams.Position);
	_masterPanel->SetSize(masterPanelParams.Size);

	auto masterSliderParams = _GetSliderParams(0, size);
	_masterSlider->SetPosition(masterSliderParams.Position);
	_masterSlider->SetSize(masterSliderParams.Size);
	_masterSlider->SetDragParams(masterSliderParams.DragControlOffset, masterSliderParams.DragControlSize, masterSliderParams.DragGap);
	if (_stationLabel)
		_stationLabel->Layout({masterSliderParams.Position.X,
			masterSliderParams.Position.Y + static_cast<int>(masterSliderParams.Size.Height) + 6}, masterSliderParams.Size.Width);
	for (auto& button : _vstControls)
		if (button)
		{
			button->SetPosition({ static_cast<int>(_SliderGap.Width), static_cast<int>(_SliderGap.Height) });
			button->SetSize({ masterSliderParams.Size.Width, GuiButtonParams::DefaultHeight });
		}

	auto channelToggleParams = _GetToggleParams(GuiRackParams::RACK_CHANNELS, size);
	_channelToggle->SetPosition(channelToggleParams.Position);
	_channelToggle->SetSize(channelToggleParams.Size);

	auto channelPanelParams = _GetPanelParams(GuiRackParams::RACK_CHANNELS, size);
	_channelPanel->SetPosition(channelPanelParams.Position);
	_channelPanel->SetSize(channelPanelParams.Size);

	auto routerToggleParams = _GetToggleParams(GuiRackParams::RACK_ROUTER, size);
	_routerToggle->SetPosition(routerToggleParams.Position);
	_routerToggle->SetSize(routerToggleParams.Size);

	auto routerPanelParams = _GetPanelParams(GuiRackParams::RACK_ROUTER, size);
	_routerPanel->SetPosition(routerPanelParams.Position);
	_routerPanel->SetSize(routerPanelParams.Size);

	auto routerParams = _GetRouterParams(size);
	_router->SetPosition(routerParams.Position);
	_router->SetSize(routerParams.Size);

	const auto midiPanelWidth = (8u * _MidiChannelToggleSize.Width) + (7u * _MidiChannelToggleGap.Width);
	const auto midiPanelHeight = (2u * _MidiChannelToggleSize.Height) + _MidiChannelToggleGap.Height;
	_midiChannelPanel->SetPosition({
		static_cast<int>(_SliderGap.Width),
		static_cast<int>(channelPanelParams.Size.Height + _MidiChannelPanelPadding)
	});
	_midiChannelPanel->SetSize({ midiPanelWidth, midiPanelHeight });

	for (auto channel = 0u; channel < _midiChannelToggles.size(); ++channel)
	{
		const auto row = channel / 8u;
		const auto col = channel % 8u;
		_midiChannelToggles[channel]->SetPosition({
			static_cast<int>(col * (_MidiChannelToggleSize.Width + _MidiChannelToggleGap.Width)),
			static_cast<int>(row * (_MidiChannelToggleSize.Height + _MidiChannelToggleGap.Height))
		});
		_midiChannelToggles[channel]->SetSize(_MidiChannelToggleSize);
	}
	if (_vstRack)
	{
		_vstRack->SetPosition({masterSliderParams.Position.X, 0});
		// Share one clearance around the fader, so resizing keeps both expanders mirrored.
		const int rightGap = channelToggleParams.Position.X -
			(masterSliderParams.Position.X + static_cast<int>(masterSliderParams.Size.Width));
		_vstRack->Layout(masterSliderParams.Size.Width, masterSliderParams.Position.Y,
			{-rightGap - static_cast<int>(channelToggleParams.Size.Width), channelToggleParams.Position.Y},
			channelToggleParams.Size);
	}

	for (auto i = 0u; i < _channelSliders.size(); ++i)
	{
		auto chanSliderParams = _GetSliderParams(i + 1, size);
		_channelSliders[i]->SetPosition(chanSliderParams.Position);
		_channelSliders[i]->SetSize(chanSliderParams.Size);
		_channelSliders[i]->SetDragParams(chanSliderParams.DragControlOffset, chanSliderParams.DragControlSize, chanSliderParams.DragGap);
	}

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
	if (action.ElementType == GuiAction::ACTIONELEMENT_BUTTON && action.Index == VstControlActionIndex)
	{
		auto control = GetVstControl();
		if (!control || !control->IsEnabled()) return ActionResult::NoAction();
		if (_rackParams.OnVstControl) _rackParams.OnVstControl(action.InputModifiers);
		else if (_receiver)
		{
			action.ElementType = GuiAction::ACTIONELEMENT_RACK;
			_receiver->OnAction(action);
		}
		return ActionResult::NoAction();
	}

	if (_receiver)
	{
		switch (action.ElementType)
		{
		case GuiAction::ACTIONELEMENT_TOGGLE:
			if (action.Index >= MidiChannelToggleBaseIndex &&
				action.Index < (MidiChannelToggleBaseIndex + 16u))
			{
				const auto channel = static_cast<int>(action.Index - MidiChannelToggleBaseIndex + 1u);
				const auto wasActive = _vstRack && _vstRack->ChannelShownActive(static_cast<unsigned int>(channel - 1));
				auto it = std::find(_rackParams.AllowedMidiChannels.begin(),
					_rackParams.AllowedMidiChannels.end(),
					channel);
				bool membershipChanged = false;
				if (it == _rackParams.AllowedMidiChannels.end() && !wasActive)
				{
					_rackParams.AllowedMidiChannels.push_back(channel);
					std::sort(_rackParams.AllowedMidiChannels.begin(), _rackParams.AllowedMidiChannels.end());
					membershipChanged = true;
				}
				else if (it != _rackParams.AllowedMidiChannels.end())
				{ _rackParams.AllowedMidiChannels.erase(it); membershipChanged = true; }

				if (membershipChanged && _rackParams.OnAllowedMidiChannelsChanged)
					_rackParams.OnAllowedMidiChannelsChanged(_rackParams.AllowedMidiChannels);
				if (_vstRack) _vstRack->SetAllowedMidiChannels(_rackParams.AllowedMidiChannels);

				break;
			}

			toggleOn = false;
			if (auto i = std::get_if<GuiAction::GuiInt>(&action.Data))
				toggleOn = (1 == i->Value);

			newRackState = action.Index <= GuiRackParams::RACK_ROUTER ?
				(GuiRackParams::RackState)action.Index :
				GuiRackParams::RACK_MASTER;

			{
				GuiRackParams::RackState computedState = _rackState;
				if (newRackState == GuiRackParams::RACK_ROUTER)
					computedState = toggleOn ? GuiRackParams::RACK_ROUTER : GuiRackParams::RACK_CHANNELS;
				else if (newRackState == GuiRackParams::RACK_CHANNELS)
					computedState = (routerVisible && toggleOn) ? GuiRackParams::RACK_ROUTER :
						(toggleOn ? GuiRackParams::RACK_CHANNELS : GuiRackParams::RACK_MASTER);

				// Pre-notify receiver before state change so it can collapse
				// other takes appropriately based on the new state.
				if (_receiver)
				{
					if (computedState == GuiRackParams::RACK_ROUTER && _rackState != GuiRackParams::RACK_ROUTER)
					{
						GuiAction preNotify;
						preNotify.ElementType = GuiAction::ACTIONELEMENT_RACK;
						preNotify.Index = RackStateNotificationIndex;
						preNotify.Data = GuiAction::GuiInt{(int)GuiRackParams::RACK_ROUTER};
						_receiver->OnAction(preNotify);
					}
					else if (computedState == GuiRackParams::RACK_CHANNELS && _rackState == GuiRackParams::RACK_MASTER)
					{
						GuiAction preNotify;
						preNotify.ElementType = GuiAction::ACTIONELEMENT_RACK;
						preNotify.Index = RackStateNotificationIndex;
						preNotify.Data = GuiAction::GuiInt{(int)GuiRackParams::RACK_CHANNELS};
						_receiver->OnAction(preNotify);
					}
				}

				_rackState = computedState;
			}
			_OnRackChange(action.Index, true);
			break;
		case GuiAction::ACTIONELEMENT_SLIDER:
			action.ElementType = GuiAction::ACTIONELEMENT_RACK;
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

void GuiRack::SetVisible(bool visible)
{
	if (!visible)
	{
		if (_vstRack) { _vstRack->ClearPointerState(); _vstRack->SetRevealHeld(false); }
		if (_stationLabel) _stationLabel->Cancel();
		SetRackState(GuiRackParams::RACK_MASTER, true);
	}
	GuiElement::SetVisible(visible);
}

void GuiRack::SetMasterControlsVisible(bool visible)
{
	if (!visible && _vstRack) { _vstRack->ClearPointerState(); _vstRack->SetRevealHeld(false); }
	if (!visible && _stationLabel) _stationLabel->Cancel();
	_masterControlsVisible = visible;
	// Suppression must not overwrite selection-depth visibility or collapse
	// the rack being edited.
	_masterPanel->SetVisible(visible || _rackState != GuiRackParams::RACK_MASTER);
}

bool GuiRack::IsInView(utils::Size2d viewport) const
{
	if (!IsVisible())
		return false;
	for (auto ancestor = Parent(); ancestor; ancestor = ancestor->Parent())
		if (!ancestor->IsVisible()) return false;
	if (_vstRack && _vstRack->IsInView(viewport)) return true;

	// Expanded panels may extend into view after their owner has left it.
	// Only the persistent master rectangle determines whether the rack is in view.
	const auto position = GlobalPosition();
	const auto size = GetSize();
	return position.X < static_cast<int>(viewport.Width) &&
		position.Y < static_cast<int>(viewport.Height) &&
		static_cast<std::int64_t>(position.X) + size.Width > 0 &&
		static_cast<std::int64_t>(position.Y) + size.Height > 0;
}

void GuiRack::_InitReceivers()
{
	_receiversInitialized = true;

	_masterSlider->SetReceiver(ActionReceiver::shared_from_this());
	for (auto& button : _vstControls)
		if (button) button->SetReceiver(ActionReceiver::shared_from_this());
	_masterSlider->SetValue(_masterSlider->Value(), true);

	_channelToggle->SetReceiver(ActionReceiver::shared_from_this());

	for (auto& slider : _channelSliders)
	{
		slider->SetReceiver(ActionReceiver::shared_from_this());
	}

	_routerToggle->SetReceiver(ActionReceiver::shared_from_this());
	_router->SetReceiver(ActionReceiver::shared_from_this());
	for (auto& toggle : _midiChannelToggles)
		toggle->SetReceiver(ActionReceiver::shared_from_this());
}

void GuiRack::_AddChannel(unsigned int index, utils::Size2d size)
{
	auto slider = std::make_shared<gui::GuiSlider>(_GetSliderParams(index, size));
	_channelSliders.push_back(slider);
	_channelPanel->AddChild(slider);

	if (_receiversInitialized)
		slider->SetReceiver(ActionReceiver::shared_from_this());
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
		if (!_rackParams.EnableVstRackMidiRouting) _midiChannelPanel->SetVisible(false);
		break;
	case GuiRackParams::RACK_CHANNELS:
		_masterPanel->SetVisible(true);
		_channelToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, 1 == index ? true : bypassUpdates);
		_channelPanel->SetVisible(true);
		_routerToggle->SetToggleState(GuiToggleParams::TOGGLE_OFF, 2 == index ? true : bypassUpdates);
		_routerPanel->SetVisible(false);
		if (!_rackParams.EnableVstRackMidiRouting) _midiChannelPanel->SetVisible(true);
		break;
	case GuiRackParams::RACK_ROUTER:
		_masterPanel->SetVisible(true);
		_channelToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, 1 == index ? true : bypassUpdates);
		_channelPanel->SetVisible(true);
		_routerToggle->SetToggleState(GuiToggleParams::TOGGLE_ON, 2 == index ? true : bypassUpdates);
		_routerPanel->SetVisible(true);
		if (!_rackParams.EnableVstRackMidiRouting) _midiChannelPanel->SetVisible(true);
		break;
	}

	SetMasterControlsVisible(_masterControlsVisible);

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
		params.Position = { static_cast<int>(size.Width - _ChannelExpanderInset), 0 };
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
	sliderParams.Max = std::pow(10.0, GuiSliderParams::RackMaxDecibels / 20.0);
	sliderParams.Scale = GuiSliderParams::SliderScale::Decibels;
	sliderParams.MinDecibels = GuiSliderParams::RackMinDecibels;
	sliderParams.ScaleMarksEnabled = true;
	sliderParams.ResetToUnityOnDoubleClick = true;
	sliderParams.InitValue = _rackParams.InitLevel;
	sliderParams.Orientation = GuiSliderParams::SLIDER_VERTICAL;

	sliderParams.Size = sliderSize;
	sliderParams.MinSize = { std::max(40u,sliderParams.Size.Width), std::max(47u, sliderParams.Size.Height) };

	if (0 == index)
	{
		sliderParams.Position = { (int)_SliderGap.Width, (int)_SliderGap.Height };
		if (_rackParams.EnableVstControl)
		{
			const auto reserved = GuiButtonParams::DefaultHeight + _SliderGap.Height;
			sliderSize.Height -= reserved;
			sliderParams.Size.Height = sliderSize.Height;
			sliderParams.MinSize.Height = std::max(47u, sliderSize.Height);
			sliderParams.Position.Y += static_cast<int>(reserved);
		}
	}
	else
	{
		sliderParams.Position = {
			((int)_SliderGap.Width + (int)sliderSize.Width) * (int)(index-1),
			(int)_SliderGap.Height
		};
	}

	// Small overhangs leave clearance within the eight-pixel channel gap.
	utils::Size2d dragSize = {
		sliderSize.Width + (2u * _DragOverhang),
		std::min(_DragHeight, sliderSize.Height)
	};
	sliderParams.DragControlOffset = { -static_cast<int>(_DragOverhang), (int)_DragGap.Height };
	sliderParams.DragControlSize = dragSize;
	sliderParams.DragGap = _DragGap;
	sliderParams.Texture = "fader_back_panel";
	sliderParams.TextureOpacity = 0.55f;
	sliderParams.OverTexture = "fader_back_panel_over";
	sliderParams.DownTexture = "fader_back_panel_down";
	sliderParams.DragTexture = "fader_button";
	sliderParams.DragOverTexture = "fader_button_over";
	sliderParams.DragDownTexture = "fader_button_down";

	return sliderParams;
}

gui::GuiToggleParams GuiRack::_GetToggleParams(GuiRackParams::RackState state, utils::Size2d size)
{
	GuiToggleParams toggleParams;

	auto sliderSize = _CalcSliderSize(size);

	switch (state)
	{
	case GuiRackParams::RACK_CHANNELS:
		toggleParams = GuiToggleParams::RackExpander(false);
		toggleParams.ToggleIndex = 1;
		toggleParams.Position = {
			static_cast<int>(size.Width - _ChannelToggleSize.Width - _ChannelExpanderInset),
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
	if (state != GuiRackParams::RACK_CHANNELS)
	{
		toggleParams.Texture = "arrow";
		toggleParams.OverTexture = "arrow_over";
		toggleParams.DownTexture = "arrow_down";
		toggleParams.ToggledTexture = "arrowup2";
		toggleParams.ToggledOverTexture = "arrowup2_over";
		toggleParams.ToggledDownTexture = "arrowup2_down";
	}

	return toggleParams;
}

gui::GuiRouterParams GuiRack::_GetRouterParams(utils::Size2d size)
{
	auto sliderSize = _CalcSliderSize(size);
	auto routerParams = GuiRouterParams::RackRouter(
		_CalcChannelPannelWidth(sliderSize),
		_CalcRouterHeight(size),
		sliderSize.Width + _SliderGap.Width,
		sliderSize.Width);

	routerParams.Position = { 0, 0 };

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

	_rackParams.NumInputChannels = channels;
	_router->SetNumInputs(channels);
	SetSize(_rackParams.Size);
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

void GuiRack::SetAllowedMidiChannels(const std::vector<int>& channels, bool bypassUpdates)
{
	std::vector<int> filtered;
	filtered.reserve(channels.size());
	for (auto channel : channels)
	{
		if (channel < 1 || channel > 16)
			continue;

		if (std::find(filtered.begin(), filtered.end(), channel) == filtered.end())
			filtered.push_back(channel);
	}

	std::sort(filtered.begin(), filtered.end());
	_rackParams.AllowedMidiChannels = filtered;

	for (auto channel = 0u; channel < _midiChannelToggles.size(); ++channel)
	{
		const auto channelNumber = static_cast<int>(channel + 1u);
		const auto on = std::find(filtered.begin(), filtered.end(), channelNumber) != filtered.end();
		_midiChannelToggles[channel]->SetToggleState(on ? GuiToggleParams::TOGGLE_ON : GuiToggleParams::TOGGLE_OFF,
			true);
	}
	if (_vstRack) _vstRack->SetAllowedMidiChannels(filtered);

	if (!bypassUpdates && _rackParams.OnAllowedMidiChannelsChanged)
		_rackParams.OnAllowedMidiChannelsChanged(_rackParams.AllowedMidiChannels);
}

std::shared_ptr<gui::GuiSlider> GuiRack::GetMasterSlider() const
{
	return _masterSlider;
}

std::shared_ptr<GuiButton> GuiRack::GetVstControl() const
{
	if (_vstRack) return _vstRack->AppendButton();
	const auto index = _vstControlState == VstControlState::Loading ?
		static_cast<size_t>(VstControlState::Closed) : static_cast<size_t>(_vstControlState);
	return _vstControls[index];
}

void GuiRack::SetVstControlState(VstControlState state, bool enabled)
{
	const auto previousState = _vstControlState;
	_vstControlState = state;
	const auto active = GetVstControl();
	for (auto& button : _vstControls)
		if (button)
		{
			button->SetVisible(button == active);
			button->SetEnabled(enabled && state != VstControlState::Loading);
		}
	if (previousState != state && active)
		active->SetText(state == VstControlState::Loading ? "Loading..." : _vstControlText);
}

void GuiRack::SetVstControlCallback(std::function<void(base::Action::Modifiers)> callback)
{
	_rackParams.OnVstControl = std::move(callback);
}

void GuiRack::Init()
{
	GuiElement::Init();
	// Tree initialization assigns child ordinals; this command has a stable index.
	for (auto& button : _vstControls)
		if (button) button->SetIndex(VstControlActionIndex);
}

void GuiRack::SetVstControlText(const std::string& text)
{
	if (_vstControlText == text) return;
	_vstControlText = text;
	for (auto& button : _vstControls)
		if (button) button->SetText(text);
	if (_vstControlState == VstControlState::Loading && GetVstControl())
		GetVstControl()->SetText("Loading...");
}

std::shared_ptr<gui::GuiSlider> GuiRack::GetChannelSlider(unsigned int index) const
{
	if (index < _channelSliders.size())
		return _channelSliders[index];
	return nullptr;
}
