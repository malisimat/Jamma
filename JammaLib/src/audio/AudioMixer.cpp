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

AudioMixer::AudioMixer(AudioMixerParams params) :
	GuiElement(params),
	Tweakable(params),
	_unmutedFadeTarget(DefaultLevel),
	_behaviour(std::unique_ptr<MixBehaviour>()),
	_fade(std::make_unique<InterpolatedValueExp>())
{
	_behaviour = std::visit(MixerBehaviourFactory{}, params.Behaviour);

	InterpolatedValueExp::ExponentialParams interpParams;
	interpParams.Damping = 100.0f;

	_fade = std::make_unique<InterpolatedValueExp>(interpParams);
	_fade->Jump(DefaultLevel);
}

ActionResult AudioMixer::OnAction(GuiAction action)
{
	if (_isEnabled)
	{
		switch (action.ElementType)
		{
		case GuiAction::ACTIONELEMENT_SLIDER:
			if (auto d = std::get_if<GuiAction::GuiDouble>(&action.Data))
			{
				SetUnmutedLevel(d->Value);

				return { true, "", "", ACTIONRESULT_DEFAULT, nullptr };
			}
			break;
		}
	}
	
	return { false, "", "", ACTIONRESULT_DEFAULT, nullptr };
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

void AudioMixer::SetChannels(std::vector<std::pair<unsigned int, unsigned int>> channels)
{
	unsigned int chan = 0;

	unsigned int maxChan = 0;
	for (const auto& channel : channels)
	{
		if (channel.first > maxChan)
			maxChan = channel.first;
	}

	std::vector<unsigned int> outputChans(maxChan + 1, 0);

	// Fill the vector with the appropriate output channels
	for (const auto& channel : channels) {
		outputChans[channel.first] = channel.second;
	}

	if (auto wireBehaviour = dynamic_cast<audio::WireMixBehaviour*>(_behaviour.get())) {
		WireMixBehaviourParams wireParams;
		wireParams.Channels = outputChans;
		wireBehaviour->SetParams(wireParams);
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