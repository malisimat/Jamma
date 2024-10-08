#include "AudioMixer.h"

using namespace audio;
using namespace actions;
using base::AudioSource;
using base::MultiAudioSink;
using base::GuiElement;
using gui::GuiSlider;
using gui::GuiSliderParams;

const utils::Size2d AudioMixer::_Gap = { 2, 4 };
const utils::Size2d AudioMixer::_DragGap = { 4, 4 };
const utils::Size2d AudioMixer::_DragSize = { 32, 32 };

AudioMixer::AudioMixer(AudioMixerParams params) :
	GuiElement(params),
	_inputChannel(params.InputChannel),
	_outputChannel(params.OutputChannel),
	_behaviour(std::unique_ptr<MixBehaviour>()),
	_slider(std::make_shared<GuiSlider>(GetSliderParams(params.Size, params.OutputChannel))),
	_fade(std::make_unique<InterpolatedValueExp>())
{
	_behaviour = std::visit(MixerBehaviourFactory{}, params.Behaviour);

	InterpolatedValueExp::ExponentialParams interpParams;
	interpParams.Damping = 100.0f;

	_fade = std::make_unique<InterpolatedValueExp>(interpParams);
	_fade->SetTarget(1.0);

	_children.push_back(_slider);
}

void AudioMixer::InitReceivers()
{
	_slider->SetReceiver(ActionReceiver::shared_from_this());
	_slider->SetValue(0.2);
}

void AudioMixer::SetSize(utils::Size2d size)
{
	auto sliderParams = GetSliderParams(size, _outputChannel);
	_slider->SetSize(sliderParams.Size);

	GuiElement::SetSize(size);
}

ActionResult AudioMixer::OnAction(DoubleAction val)
{
	_fade->SetTarget(val.Value());
	return { true, "", ACTIONRESULT_DEFAULT, nullptr };
}

double AudioMixer::Level() const
{
	return _fade->Current();
}

void AudioMixer::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	unsigned int index)
{
	_behaviour->Apply(dest, samp * (float)_fade->Next(), index);
}

void AudioMixer::Offset(unsigned int numSamps)
{
	// TODO: Update fader state
}

unsigned int AudioMixer::InputChannel() const
{
	return _inputChannel;
}

void AudioMixer::SetInputChannel(unsigned int channel)
{
	_inputChannel = channel;
}

unsigned int AudioMixer::OutputChannel() const
{
	return _outputChannel;
}

void AudioMixer::SetOutputChannel(unsigned int channel)
{
	_outputChannel = channel;
}

void WireMixBehaviour::Apply(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	unsigned int index) const
{
	auto numChans = dest->NumInputChannels();

	for (auto chan = 0u; chan < numChans; chan++)
	{
		if (std::find(_mixParams.Channels.begin(), _mixParams.Channels.end(), chan) != _mixParams.Channels.end())
			dest->OnWriteChannel(chan, samp, index);
	}
}

void PanMixBehaviour::Apply(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	unsigned int index) const
{
	auto numChans = dest->NumInputChannels();

	for (auto chan = 0u; chan < numChans; chan++)
	{
		if (chan < _mixParams.ChannelLevels.size())
			dest->OnWriteChannel(chan, samp * _mixParams.ChannelLevels.at(chan), index);
	}
}

gui::GuiSliderParams AudioMixer::GetSliderParams(utils::Size2d mixerSize, unsigned int outputChannel)
{
	GuiSliderParams sliderParams;
	sliderParams.Min = 0.0;
	sliderParams.Max = 6.0;
	sliderParams.InitValue = 1.0;
	sliderParams.Orientation = GuiSliderParams::SLIDER_VERTICAL;
	sliderParams.Position = { (int)_Gap.Width, (int)_Gap.Height};
	sliderParams.Size = { mixerSize.Width - (2u * _Gap.Width), mixerSize.Height - (2 * _Gap.Height) };
	sliderParams.MinSize = { std::max(40u,mixerSize.Width), std::max(40u, mixerSize.Height) };
	sliderParams.DragControlOffset = { (int)_DragGap.Width, (int)_DragGap.Height };
	sliderParams.DragControlSize = _DragSize;
	sliderParams.DragGap = _DragGap;
	sliderParams.Texture = "fader_back";
	sliderParams.DragTexture = "fader";
	sliderParams.DragOverTexture = "fader_over";

	return sliderParams;
}