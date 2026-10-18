#pragma once

#include <functional>
#include <array>
#include "GuiButton.h"
#include "GuiSlider.h"
#include "GuiToggle.h"
#include "GuiRouter.h"
#include "GuiStationLabel.h"
#include "ActionReceiver.h"

namespace gui
{
	class GuiVstRack;
	class GuiRackParams :
		public base::GuiElementParams
	{
	public:
		enum RackState
		{
			RACK_MASTER = 0,
			RACK_CHANNELS = 1,
			RACK_ROUTER = 2
		};

	public:
		GuiRackParams() :
			GuiElementParams(),
			NumInputChannels(0),
			NumOutputChannels(0),
			InitLevel(1.0),
			InitState(RACK_MASTER),
			Receiver(std::weak_ptr<base::ActionReceiver>()),
			AllowedMidiChannels(),
			OnAllowedMidiChannelsChanged()
		{}

		GuiRackParams(base::GuiElementParams params,
			unsigned int numInputs,
			unsigned int numOutputs,
			std::weak_ptr<base::ActionReceiver> receiver) :
			GuiElementParams(params),
			NumInputChannels(numInputs),
			NumOutputChannels(numOutputs),
			InitLevel(1.0),
			InitState(RACK_MASTER),
			Receiver(receiver),
			AllowedMidiChannels(),
			OnAllowedMidiChannelsChanged()
		{}

	public:
		unsigned int NumInputChannels;
		unsigned int NumOutputChannels;
		double InitLevel;
		RackState InitState;
		std::weak_ptr<base::ActionReceiver> Receiver;
		std::vector<int> AllowedMidiChannels;
		std::function<void(const std::vector<int>&)> OnAllowedMidiChannelsChanged;
		bool EnableVstControl = false;
		bool EnableStationLabel = false;
		bool EnableVstRack = false;
		bool EnableVstRackMidiRouting = false;
		std::function<void(base::Action::Modifiers)> OnVstControl;
	};

	class GuiRack :
		public base::GuiElement
	{
	public:
		GuiRack(GuiRackParams params);

	public:
		static constexpr unsigned int RackStateNotificationIndex = ~0u;
		static constexpr unsigned int MidiChannelToggleBaseIndex = 100u;
		static constexpr unsigned int VstControlActionIndex = ~0u - 1u;
		void Init() override;
		enum class VstControlState { Empty, Closed, Open, Failed, Loading };
		void SetVstControlState(VstControlState state, bool enabled = true);
		void SetVstControlCallback(std::function<void(base::Action::Modifiers)> callback);
		void SetVstControlText(const std::string& text);
		std::shared_ptr<GuiButton> GetVstControl() const;
		std::shared_ptr<GuiStationLabel> StationLabel() const { return _stationLabel; }
		std::shared_ptr<GuiVstRack> VstRack() const { return _vstRack; }

		virtual void SetSize(utils::Size2d size) override;
		virtual void SetVisible(bool visible) override;
		void SetMasterControlsVisible(bool visible);
		bool IsInView(utils::Size2d viewport) const;

		virtual actions::ActionResult OnAction(actions::GuiAction action) override;

		GuiRackParams::RackState GetRackState() const;
		void SetRackState(GuiRackParams::RackState state, bool bypassUpdates);
		unsigned int NumInputChannels() const;
		unsigned int NumOutputChannels() const;
		void SetNumInputChannels(unsigned int channels);
		void SetNumOutputChannels(unsigned int channels);
		void AddRoute(unsigned int inputChan, unsigned int outputChan);
		void ClearRoutes();
		const std::vector<std::pair<unsigned int, unsigned int>>& Routes() const { return _router->Routes(); }
		void SetAllowedMidiChannels(const std::vector<int>& channels, bool bypassUpdates);

		std::shared_ptr<gui::GuiSlider> GetMasterSlider() const;
		std::shared_ptr<gui::GuiSlider> GetChannelSlider(unsigned int index) const;

	protected:
		static const utils::Size2d _SliderGap;
		static const unsigned int _ChannelTogglePaddingLeft;
		static const unsigned int _RouterSpacingY;
		static const utils::Size2d _ChannelToggleSize;
		static const utils::Size2d _RouterToggleSize;
		static const unsigned int _RouterTogglePaddingBottom;
		static const utils::Size2d _DragGap;
		static constexpr unsigned int _MinimumScaleLayoutHeight = 55u;
		static constexpr unsigned int _DragHeight = 28u;
		static constexpr unsigned int _DragOverhang = 2u;
		static const utils::Size2d _MidiChannelToggleSize;
		static const utils::Size2d _MidiChannelToggleGap;
		static const unsigned int _MidiChannelPanelPadding;

		virtual void _InitReceivers() override;

		virtual void _AddChannel(unsigned int channel, utils::Size2d size);
		virtual void _OnRackChange(unsigned int index, bool bypassUpdates);

		utils::Size2d _CalcSliderSize(utils::Size2d size);
		unsigned int _CalcChannelPannelWidth(utils::Size2d sliderSize);
		unsigned int _CalcRouterHeight(utils::Size2d size);
		base::GuiElementParams _GetPanelParams(GuiRackParams::RackState state, utils::Size2d);
		gui::GuiSliderParams _GetSliderParams(unsigned int index, utils::Size2d size);
		gui::GuiToggleParams _GetToggleParams(GuiRackParams::RackState state, utils::Size2d);
		gui::GuiRouterParams _GetRouterParams(utils::Size2d size);

	private:
		bool _receiversInitialized;
		bool _masterControlsVisible = true;
		GuiRackParams::RackState _rackState;
		std::shared_ptr<base::GuiElement> _masterPanel;
		std::shared_ptr<gui::GuiSlider> _masterSlider;
		std::array<std::shared_ptr<GuiButton>, 4> _vstControls;
		VstControlState _vstControlState = VstControlState::Empty;
		std::string _vstControlText = "VST";
		std::shared_ptr<gui::GuiToggle> _channelToggle;
		std::shared_ptr<base::GuiElement> _channelPanel;
		std::vector<std::shared_ptr<gui::GuiSlider>> _channelSliders;
		std::shared_ptr<gui::GuiToggle> _routerToggle;
		std::shared_ptr<base::GuiElement> _routerPanel;
		std::shared_ptr<gui::GuiRouter> _router;
		std::shared_ptr<base::GuiElement> _midiChannelPanel;
		std::vector<std::shared_ptr<gui::GuiToggle>> _midiChannelToggles;
		GuiRackParams _rackParams;
		std::shared_ptr<GuiStationLabel> _stationLabel;
		std::shared_ptr<GuiVstRack> _vstRack;
	};
}
