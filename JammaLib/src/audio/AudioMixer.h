#pragma once

#include <vector>
#include <memory>
#include <variant>
#include "AudioSource.h"
#include "MultiAudioSink.h"
#include "InterpolatedValue.h"
#include "GuiElement.h"
#include "Tweakable.h"
#include "../actions/DoubleAction.h"
#include "../gui/GuiSlider.h"

namespace audio
{
	class MixBehaviour
	{
	public:
		virtual void Apply(const std::shared_ptr<base::MultiAudioSink> dest,
			float samp,
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

	public:
		virtual void Apply(const std::shared_ptr<base::MultiAudioSink> dest,
			float samp,
			float fadeNew,
			unsigned int index) const override;
	};

	class MergeMixBehaviourParams : public WireMixBehaviourParams {};
	class MergeMixBehaviour : public WireMixBehaviour
	{
	public:
		MergeMixBehaviour(MergeMixBehaviourParams mixParams) :
			WireMixBehaviour(mixParams)
		{
			_mixParams = mixParams;
		}

	public:
		virtual void Apply(const std::shared_ptr<base::MultiAudioSink> dest,
			float samp,
			float fadeNew,
			unsigned int index) const override;
	};

	typedef std::variant<MixBehaviourParams, WireMixBehaviourParams, PanMixBehaviourParams, BounceMixBehaviourParams, MergeMixBehaviourParams> BehaviourParams;

	class AudioMixerParams :
		public base::GuiElementParams,
		public base::TweakableParams
	{
	public:
		AudioMixerParams() :
			base::GuiElementParams(0, DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			base::TweakableParams(),
			Behaviour(MixBehaviourParams())
		{
		}

		AudioMixerParams(base::GuiElementParams params,
			base::TweakableParams tweakParams,
			BehaviourParams behaviour,
			gui::GuiSliderParams sliderParams) :
			base::GuiElementParams(params),
			base::TweakableParams(tweakParams),
			Behaviour(behaviour)
		{
		}

	public:
		BehaviourParams Behaviour;
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
		std::unique_ptr<MixBehaviour> operator()(MergeMixBehaviourParams mergeParams) const {
			return std::move(std::make_unique<MergeMixBehaviour>(mergeParams));
		}
	};

	class AudioMixer :
		public base::GuiElement,
		public base::Tweakable
	{
	public:
		AudioMixer(AudioMixerParams params);

	public:
		static const double DefaultLevel;

		virtual std::string ClassName() const { return "AudioMixer"; }

		virtual actions::ActionResult OnAction(actions::DoubleAction val) override;
		virtual void SetSize(utils::Size2d size) override;
		virtual bool Mute() override;
		virtual bool UnMute() override;

		double Level() const;
		double UnmutedLevel() const;
		void SetUnmutedLevel(double level);
		void OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
			float samp,
			unsigned int index);
		void Offset(unsigned int numSamps);
		void SetBehaviour(std::unique_ptr<MixBehaviour> behaviour);

	protected:
		virtual void _InitReceivers() override;

		gui::GuiSliderParams _GetSliderParams(utils::Size2d size);

	protected:
		static const utils::Size2d _Gap;
		static const utils::Size2d _DragGap;
		static const utils::Size2d _DragSize;

		double _unmutedFadeTarget;
		std::unique_ptr<MixBehaviour> _behaviour;
		std::shared_ptr<gui::GuiSlider> _slider;
		std::unique_ptr<InterpolatedValue> _fade;
	};
}
