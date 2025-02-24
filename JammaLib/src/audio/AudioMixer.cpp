#include "AudioMixer.h"

using namespace audio;
using namespace actions;
using base::AudioSource;
using base::Tweakable;
using base::MultiAudioSink;
using base::GuiElement;
using gui::GuiSlider;
using gui::GuiSliderParams;

const double AudioMixer::DefaultLevel = 1.0;

const utils::Size2d AudioMixer::_Gap = { 2, 4 };
const utils::Size2d AudioMixer::_DragGap = { 4, 4 };
const utils::Size2d AudioMixer::_DragSize = { 32, 32 };

AudioMixer::AudioMixer(AudioMixerParams params) :
	GuiElement(params),
	Tweakable(params),
	_unmutedFadeTarget(DefaultLevel),
	_behaviour(std::unique_ptr<MixBehaviour>()),
	_slider(std::make_shared<GuiSlider>(_GetSliderParams(params.Size))),
	_fade(std::make_unique<InterpolatedValueExp>())
{
	_behaviour = std::visit(MixerBehaviourFactory{}, params.Behaviour);

	InterpolatedValueExp::ExponentialParams interpParams;
	interpParams.Damping = 100.0f;

	_fade = std::make_unique<InterpolatedValueExp>(interpParams);
	_fade->Jump(DefaultLevel);

	_children.push_back(_slider);
}

void AudioMixer::SetSize(utils::Size2d size)
{
	auto sliderParams = _GetSliderParams(size);
	_slider->SetSize(sliderParams.Size);

	GuiElement::SetSize(size);
}

ActionResult AudioMixer::OnAction(DoubleAction val)
{
	SetUnmutedLevel(val.Value());

	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr};
}

bool AudioMixer::Mute()
{
	auto isNewState = Tweakable::Mute();

	if (isNewState)
		_fade->SetTarget(0.0);

	return isNewState;
}

bool AudioMixer::UnMute()
{
	auto isNewState = Tweakable::UnMute();

	if (isNewState)
		_fade->SetTarget(_unmutedFadeTarget);

	return isNewState;
}

double AudioMixer::Level() const
{
	return _fade->Current();
}

double AudioMixer::UnmutedLevel() const
{
	return _unmutedFadeTarget;
}

void AudioMixer::SetUnmutedLevel(double level)
{
	_unmutedFadeTarget = level;
	
	if (!IsMuted())
		_fade->SetTarget(_unmutedFadeTarget);
}

void AudioMixer::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	unsigned int index)
{
	if (_behaviour)
		_behaviour->Apply(dest, samp, (float)_fade->Next(), index);
}

void AudioMixer::Offset(unsigned int numSamps)
{
	for (auto samp = 0u; samp < numSamps; samp++)
	{
		_fade->Next();
	}
}

void AudioMixer::SetBehaviour(std::unique_ptr<MixBehaviour> behaviour)
{
	_behaviour = std::move(behaviour);
}

void WireMixBehaviour::Apply(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	float fadeNew,
	unsigned int index) const
{
	if (nullptr == dest)
		return;

	float fadeCurrent = 0.0f;

	for (auto chan : _mixParams.Channels)
	{
		dest->OnMixWriteChannel(chan,
			samp,
			fadeCurrent,
			fadeNew,
			index,
			base::Audible::AudioSourceType::AUDIOSOURCE_LOOPS);
	}
}

void PanMixBehaviour::Apply(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	float fadeNew,
	unsigned int index) const
{
	if (nullptr == dest)
		return;

	auto numChans = dest->NumInputChannels();
	float fadeCurrent = 1.0f;

	for (auto chan = 0u; chan < numChans; chan++)
	{
		if (chan < _mixParams.ChannelLevels.size())
			dest->OnMixWriteChannel(chan,
				samp * _mixParams.ChannelLevels.at(chan),
				fadeCurrent,
				fadeNew,
				index,
				base::Audible::AudioSourceType::AUDIOSOURCE_ADC);
	}
}

void BounceMixBehaviour::Apply(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	float fadeNew,
	unsigned int index) const
{
	if (nullptr == dest)
		return;

	float fadeCurrent = 1.0f - fadeNew;

	for (auto chan : _mixParams.Channels)
	{
		dest->OnMixWriteChannel(chan,
			samp,
			fadeCurrent,
			fadeNew,
			index,
			base::Audible::AudioSourceType::AUDIOSOURCE_BOUNCE);
	}
}

void MergeMixBehaviour::Apply(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	float fadeNew,
	unsigned int index) const
{
	if (nullptr == dest)
		return;

	float fadeCurrent = 1.0f;

	for (auto chan : _mixParams.Channels)
	{
		dest->OnMixWriteChannel(chan,
			samp,
			fadeCurrent,
			fadeNew,
			index,
			base::Audible::AudioSourceType::AUDIOSOURCE_MIXER);
	}
}

void AudioMixer::_InitReceivers()
{
	_slider->SetReceiver(ActionReceiver::shared_from_this());
	_slider->SetValue(DefaultLevel);
}

gui::GuiSliderParams AudioMixer::_GetSliderParams(utils::Size2d mixerSize)
{
	GuiSliderParams sliderParams;
	sliderParams.Min = 0.0;
	sliderParams.Max = 6.0;
	sliderParams.InitValue = DefaultLevel;
	sliderParams.Orientation = GuiSliderParams::SLIDER_VERTICAL;
	sliderParams.Position = { (int)_Gap.Width, (int)_Gap.Height};
	sliderParams.Size = { mixerSize.Width - (2u * _Gap.Width), mixerSize.Height - (2 * _Gap.Height) };
	sliderParams.MinSize = { std::max(40u,mixerSize.Width), std::max(40u, mixerSize.Height) };
	sliderParams.DragControlOffset = { (int)(sliderParams.Size.Width / 2) - (int)(_DragSize.Width / 2), (int)_DragGap.Height};
	sliderParams.DragControlSize = _DragSize;
	sliderParams.DragGap = _DragGap;
	sliderParams.Texture = "fader_back";
	sliderParams.DragTexture = "fader";
	sliderParams.DragOverTexture = "fader_over";

	return sliderParams;
}