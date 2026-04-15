#pragma once

#include <vector>
#include <memory>
#include <variant>
#include "AudioSource.h"
#include "MultiAudioSink.h"
#include "InterpolatedValue.h"
#include "GuiElement.h"
#include "Tweakable.h"
#include "../actions/GuiAction.h"
#include "../gui/GuiSlider.h"
#include "../gui/GuiVu.h"

namespace audio
{
	class MixBehaviourParams {};

	class WireMixBehaviourParams : public MixBehaviourParams
	{
	public:
		WireMixBehaviourParams() {};
		WireMixBehaviourParams(const std::vector<unsigned int>& vec) :
			Channels(vec) {
		};

	public:
		std::vector<unsigned int> Channels;
	};

	class PanMixBehaviourParams : public MixBehaviourParams
	{
	public:
		std::vector<float> ChannelLevels;
	};

	class BounceMixBehaviourParams : public WireMixBehaviourParams {};

	class MergeMixBehaviourParams : public WireMixBehaviourParams {};

	typedef std::variant<MixBehaviourParams, WireMixBehaviourParams, PanMixBehaviourParams, BounceMixBehaviourParams, MergeMixBehaviourParams> BehaviourParams;

	class MixBehaviour
	{
	public:
		virtual void ApplyBlock(const std::shared_ptr<base::MultiAudioSink>& dest,
			const float* srcBuf,
			float fadeLevel,
			unsigned int numSamps,
			unsigned int startIndex) const {};

		virtual BehaviourParams GetParams() const { return BehaviourParams(); }
		virtual void SetParams(BehaviourParams params) { }
		virtual void SetMaxChannels(unsigned int channels) { }
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
		virtual void ApplyBlock(const std::shared_ptr<base::MultiAudioSink>& dest,
			const float* srcBuf,
			float fadeLevel,
			unsigned int numSamps,
			unsigned int startIndex) const override;

		virtual BehaviourParams GetParams() const { return _mixParams; }
		virtual void SetParams(BehaviourParams params)
		{
			if (auto* wireParams = std::get_if<audio::WireMixBehaviourParams>(&params)) {
				_mixParams.Channels = wireParams->Channels;
			}
		}
		virtual void SetMaxChannels(unsigned int chans)
		{
			_mixParams.Channels.erase(std::remove_if(_mixParams.Channels.begin(), _mixParams.Channels.end(),
				[chans](unsigned int val) { return val >= chans; }),
				_mixParams.Channels.end());
		}

	protected:
		// Shared helper for channel-iterated block writes (Wire, Bounce, Merge).
		void _ApplyBlockToChannels(const std::shared_ptr<base::MultiAudioSink>& dest,
			const float* srcBuf,
			float fadeCurrent,
			float fadeNew,
			unsigned int numSamps,
			unsigned int startIndex) const;

		WireMixBehaviourParams _mixParams;
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
		virtual void ApplyBlock(const std::shared_ptr<base::MultiAudioSink>& dest,
			const float* srcBuf,
			float fadeLevel,
			unsigned int numSamps,
			unsigned int startIndex) const override;

		virtual BehaviourParams GetParams() const { return _mixParams; }
		virtual void SetParams(BehaviourParams params)
		{
			if (auto* wireParams = std::get_if<audio::PanMixBehaviourParams>(&params)) {
				_mixParams.ChannelLevels = wireParams->ChannelLevels;
			}
		}
		virtual void SetMaxChannels(unsigned int chans)
		{
			if (_mixParams.ChannelLevels.size() > chans)
				_mixParams.ChannelLevels.resize(chans);
		}

	protected:
		PanMixBehaviourParams _mixParams;
	};

	class BounceMixBehaviour : public WireMixBehaviour
	{
	public:
		BounceMixBehaviour(BounceMixBehaviourParams mixParams) :
			WireMixBehaviour(mixParams)
		{
			_mixParams = mixParams;
		}

	public:
		virtual void ApplyBlock(const std::shared_ptr<base::MultiAudioSink>& dest,
			const float* srcBuf,
			float fadeLevel,
			unsigned int numSamps,
			unsigned int startIndex) const override;
	};

	class MergeMixBehaviour : public WireMixBehaviour
	{
	public:
		MergeMixBehaviour(MergeMixBehaviourParams mixParams) :
			WireMixBehaviour(mixParams)
		{
			_mixParams = mixParams;
		}

	public:
		virtual void ApplyBlock(const std::shared_ptr<base::MultiAudioSink>& dest,
			const float* srcBuf,
			float fadeLevel,
			unsigned int numSamps,
			unsigned int startIndex) const override;
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

		virtual actions::ActionResult OnAction(actions::GuiAction action) override;
		virtual bool Mute() override;
		virtual bool UnMute() override;

		double Level() const;
		double UnmutedLevel() const;
		void SetUnmutedLevel(double level);
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink>& dest,
			const float* srcBuf,
			unsigned int numSamps);
		void Offset(unsigned int numSamps);
		void SetChannels(std::vector<unsigned int> channels);
		void SetMaxChannels(unsigned int channels);
		void SetBehaviour(std::unique_ptr<MixBehaviour> behaviour);

		// VU meter (owned by this mixer; value updated in WriteBlock).
		void SetVuVisible(bool visible);
		void DrawVu(base::DrawContext& ctx, utils::Size2d sliderSize);
		// Called from hot audio path when WriteBlock is bypassed (e.g. master mixer).
		void UpdateVu(float peak, unsigned int numSamps);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

	protected:
		static const utils::Size2d _Gap;
		static const utils::Size2d _DragGap;
		static const utils::Size2d _DragSize;

		double _unmutedFadeTarget;
		std::unique_ptr<MixBehaviour> _behaviour;
		std::unique_ptr<InterpolatedValue> _fade;
		gui::GuiVu _vu;
	};
}
