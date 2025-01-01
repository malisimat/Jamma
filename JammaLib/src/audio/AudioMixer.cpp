#include "AudioMixer.h"

using namespace audio;
using namespace actions;
using base::AudioSource;
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
	_isMuted(false),
	_unmutedFadeTarget(DefaultLevel),
	_behaviour(std::unique_ptr<MixBehaviour>()),
	_slider(std::make_shared<GuiSlider>(GetSliderParams(params.Size))),
	_fade(std::make_unique<InterpolatedValueExp>())
{
	_behaviour = std::visit(MixerBehaviourFactory{}, params.Behaviour);

	InterpolatedValueExp::ExponentialParams interpParams;
	interpParams.Damping = 100.0f;

	_fade = std::make_unique<InterpolatedValueExp>(interpParams);
	_fade->Jump(DefaultLevel);

	_children.push_back(_slider);
}

void AudioMixer::InitReceivers()
{
	_slider->SetReceiver(ActionReceiver::shared_from_this());
	_slider->SetValue(DefaultLevel);
}

void AudioMixer::SetSize(utils::Size2d size)
{
	auto sliderParams = GetSliderParams(size);
	_slider->SetSize(sliderParams.Size);

	GuiElement::SetSize(size);
}

ActionResult AudioMixer::OnAction(DoubleAction val)
{
	SetUnmutedLevel(val.Value());

	return { true, "", "", ACTIONRESULT_DEFAULT, nullptr};
}

bool AudioMixer::IsMuted() const
{
	return _isMuted;
}

void AudioMixer::SetMuted(bool muted)
{
	_isMuted = muted;

	if (muted)
		_fade->SetTarget(0.0);
	else
		_fade->SetTarget(_unmutedFadeTarget);
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
	
	if (!_isMuted)
		_fade->SetTarget(_unmutedFadeTarget);
}

void AudioMixer::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	unsigned int index)
{
	if (_behaviour)
	{
		float fadeNext = (float)_fade->Next();

		if (dynamic_cast<BounceMixBehaviour*>(_behaviour.get()))
			_behaviour->Apply(dest, samp, 1.0f - fadeNext, fadeNext, index);
		else
			_behaviour->Apply(dest, samp, 1.0f, fadeNext, index);
	}
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
	float fadeCurrent,
	float fadeNew,
	unsigned int index) const
{
	if (nullptr == dest)
		return;

	for (auto chan : _mixParams.Channels)
	{
		dest->OnMixWriteChannel(chan,
			samp,
			fadeCurrent,
			fadeNew,
			index,
			base::Audible::AudioSourceType::AUDIOSOURCE_INPUT);
	}
}

void PanMixBehaviour::Apply(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	float fadeCurrent,
	float fadeNew,
	unsigned int index) const
{
	if (nullptr == dest)
		return;

	auto numChans = dest->NumInputChannels();

	for (auto chan = 0u; chan < numChans; chan++)
	{
		if (chan < _mixParams.ChannelLevels.size())
			dest->OnMixWriteChannel(chan,
				samp * _mixParams.ChannelLevels.at(chan),
				fadeCurrent,
				fadeNew,
				index,
				base::Audible::AudioSourceType::AUDIOSOURCE_INPUT);
	}
}

void BounceMixBehaviour::Apply(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	float fadeCurrent,
	float fadeNew,
	unsigned int index) const
{
	if (nullptr == dest)
		return;

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

gui::GuiSliderParams AudioMixer::GetSliderParams(utils::Size2d mixerSize)
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