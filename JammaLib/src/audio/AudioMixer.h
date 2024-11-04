#pragma once

#include <vector>
#include <memory>
#include <variant>
#include "AudioSource.h"
#include "MultiAudioSink.h"
#include "InterpolatedValue.h"
#include "GuiElement.h"
#include "../actions/DoubleAction.h"
#include "../gui/GuiSlider.h"

namespace audio
{
	class MixBehaviour
	{
	public:
		virtual void Apply(const std::shared_ptr<base::MultiAudioSink> dest,
			float samp,
			float fadeCurrent,
			float fadeNew,
			unsigned int index) const {};
	};

	class MixBehaviourParams {};

	class WireMixBehaviourParams : public MixBehaviourParams
	{
	public:
		WireMixBehaviourParams() {};
		WireMixBehaviourParams(const std::vector<unsigned int>& vec) :
			Channels(vec) {};

	public:
		std::vector<unsigned int> Channels;
	};

	class WireMixBehaviour : public MixBehaviour
	{
	public:
		WireMixBehaviour(WireMixBehaviourParams mixParams) :
			MixBehaviour()
		{
			_mixParams = mixParams;
		}

	public:
		virtual void Apply(const std::shared_ptr<base::MultiAudioSink> dest,
			float samp,
			float fadeCurrent,
			float fadeNew,
			unsigned int index) const override;

	protected:
		WireMixBehaviourParams _mixParams;
	};

	class PanMixBehaviourParams : public MixBehaviourParams
	{
	public:
		std::vector<float> ChannelLevels;
	};

	class PanMixBehaviour : public MixBehaviour
	{
	public:
		PanMixBehaviour(PanMixBehaviourParams mixParams) :
			MixBehaviour()
		{
			_mixParams = mixParams;
		}

	public:
		virtual void Apply(const std::shared_ptr<base::MultiAudioSink> dest,
			float samp,
			float fadeCurrent,
			float fadeNew,
			unsigned int index) const override;

	protected:
		PanMixBehaviourParams _mixParams;
	};

	class BounceMixBehaviourParams : public WireMixBehaviourParams {};
	class BounceMixBehaviour : public WireMixBehaviour
	{
	public:
		BounceMixBehaviour(BounceMixBehaviourParams mixParams) :
			WireMixBehaviour(mixParams)
		{
			_mixParams = mixParams;
		}
	};

	typedef std::variant<MixBehaviourParams, WireMixBehaviourParams, PanMixBehaviourParams, BounceMixBehaviourParams> BehaviourParams;

	class AudioMixerParams :
		public base::GuiElementParams
	{
	public:
		AudioMixerParams() :
			base::GuiElementParams(DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			Behaviour(MixBehaviourParams()),
			InputChannel(0u),
			OutputChannel(0u)
		{
		}

		AudioMixerParams(base::GuiElementParams params,
			BehaviourParams behaviour,
			gui::GuiSliderParams sliderParams) :
			base::GuiElementParams(params),
			Behaviour(behaviour),
			InputChannel(0u),
			OutputChannel(0u)
		{
		}

	public:
		BehaviourParams Behaviour;
		unsigned int InputChannel;
		unsigned int OutputChannel;
	};
	
	struct MixerBehaviourFactory
	{
		std::unique_ptr<MixBehaviour> operator()(MixBehaviourParams mixParams) const {
			return std::move(std::unique_ptr<MixBehaviour>());
		}
		std::unique_ptr<MixBehaviour> operator()(PanMixBehaviourParams panParams) const {
			return std::move(std::make_unique<PanMixBehaviour>(panParams));
		}
		std::unique_ptr<MixBehaviour> operator()(WireMixBehaviourParams wireParams) const {
			return std::move(std::make_unique<WireMixBehaviour>(wireParams));
		}
		std::unique_ptr<MixBehaviour> operator()(BounceMixBehaviourParams bounceParams) const {
			return std::move(std::make_unique<BounceMixBehaviour>(bounceParams));
		}
	};

	class AudioMixer :
		public base::GuiElement
	{
	public:
		AudioMixer(AudioMixerParams params);

	public:
		static const double DefaultLevel;

		virtual std::string ClassName() const { return "AudioMixer"; }

		virtual actions::ActionResult OnAction(actions::DoubleAction val) override;
		virtual void InitReceivers() override;
		virtual void SetSize(utils::Size2d size) override;

		double Level() const;
		void SetLevel(double level);
		void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
			float samp,
			unsigned int index);
		void Offset(unsigned int numSamps);
		
		unsigned int InputChannel() const;
		void SetInputChannel(unsigned int channel);

		unsigned int OutputChannel() const;
		void SetOutputChannel(unsigned int channel);

	protected:
		gui::GuiSliderParams GetSliderParams(utils::Size2d size, unsigned int outputChannel);

	protected:
		static const utils::Size2d _Gap;
		static const utils::Size2d _DragGap;
		static const utils::Size2d _DragSize;

		unsigned int _inputChannel;
		unsigned int _outputChannel;
		std::unique_ptr<MixBehaviour> _behaviour;
		std::shared_ptr<gui::GuiSlider> _slider;
		std::unique_ptr<InterpolatedValue> _fade;
	};
}
