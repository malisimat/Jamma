#pragma once

#include "GuiSlider.h"
#include "GuiToggle.h"
#include "GuiRouter.h"
#include "ActionReceiver.h"

namespace gui
{
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
			Receiver(std::weak_ptr<base::ActionReceiver>())
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
			Receiver(receiver)
		{}

	public:
		unsigned int NumInputChannels;
		unsigned int NumOutputChannels;
		double InitLevel;
		RackState InitState;
		std::weak_ptr<base::ActionReceiver> Receiver;
	};

	class GuiRack :
		public base::GuiElement
	{
	public:
		GuiRack(GuiRackParams params);

	public:
		static constexpr unsigned int RackStateNotificationIndex = ~0u;

		virtual void SetSize(utils::Size2d size) override;

		virtual actions::ActionResult OnAction(actions::GuiAction action) override;

		GuiRackParams::RackState GetRackState() const;
		void SetRackState(GuiRackParams::RackState state, bool bypassUpdates);
		unsigned int NumInputChannels() const;
		unsigned int NumOutputChannels() const;
		void SetNumInputChannels(unsigned int channels);
		void SetNumOutputChannels(unsigned int channels);
		void AddRoute(unsigned int inputChan, unsigned int outputChan);
		void ClearRoutes();

	protected:
		static const utils::Size2d _SliderGap;
		static const unsigned int _ChannelTogglePaddingLeft;
		static const unsigned int _RouterSpacingY;
		static const utils::Size2d _ChannelToggleSize;
		static const utils::Size2d _RouterToggleSize;
		static const unsigned int _RouterTogglePaddingBottom;
		static const utils::Size2d _DragGap;
		static const utils::Size2d _DragSize;

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
		GuiRackParams::RackState _rackState;
		std::shared_ptr<base::GuiElement> _masterPanel;
		std::shared_ptr<gui::GuiSlider> _masterSlider;
		std::shared_ptr<gui::GuiToggle> _channelToggle;
		std::shared_ptr<base::GuiElement> _channelPanel;
		std::vector<std::shared_ptr<gui::GuiSlider>> _channelSliders;
		std::shared_ptr<gui::GuiToggle> _routerToggle;
		std::shared_ptr<base::GuiElement> _routerPanel;
		std::shared_ptr<gui::GuiRouter> _router;
		GuiRackParams _rackParams;
	};
}
