#include "AudioMixer.h"

using namespace audio;
using namespace actions;
using base::AudioSource;
using base::DrawContext;
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
	_fade(std::make_unique<InterpolatedValueExp>()),
	_vu()
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
	return Tweakable::Mute();
}

bool AudioMixer::UnMute()
{
	return Tweakable::UnMute();
}

bool AudioMixer::IsMuted() const
{
	return Tweakable::IsMuted() || IsCaptureSourceMuted();
}

bool AudioMixer::AllowReplacementPlayback() noexcept
{
	if (!_captureSourceMuteControl)
		return false;
	const auto revision = _captureSourceMuteControl->ReplacementRevision();
	const auto previous = _replacementAuditionRevision.exchange(revision, std::memory_order_acq_rel);
	return previous != revision && _captureSourceMuteControl->IsReplacementMuted();
}

void AudioMixer::SetCaptureSourceMuteControl(
	const std::shared_ptr<const CaptureSourceMuteControl>& control) noexcept
{
	_captureSourceMuteControl = control;
}

void AudioMixer::_ApplyFadeTargetAtAudioBoundary() noexcept
{
	const double target = IsMuted() ? 0.0 : _unmutedFadeTarget.load(std::memory_order_acquire);
	if (target == _appliedFadeTarget)
		return;
	_appliedFadeTarget = target;
	_fade->SetTarget(target);
}

double AudioMixer::Level() const
{
	return _publishedFadeLevel.load(std::memory_order_acquire);
}

double AudioMixer::UnmutedLevel() const
{
	return _unmutedFadeTarget.load(std::memory_order_acquire);
}

void AudioMixer::SetUnmutedLevel(double level)
{
	_unmutedFadeTarget.store(level, std::memory_order_release);
}

void AudioMixer::WriteBlock(const std::shared_ptr<MultiAudioSink>& dest,
	const float* srcBuf,
	unsigned int numSamps)
{
	_ApplyFadeTargetAtAudioBoundary();
	if (!_behaviour)
		return;

	float peak = 0.0f;
	if (_fade->IsSettled())
	{
		_fade->Jump(_appliedFadeTarget);
		const auto level = static_cast<float>(_fade->Current());
		for (auto i = 0u; i < numSamps; ++i) peak = std::max(peak, std::abs(srcBuf[i] * level));
		_behaviour->ApplyBlock(dest, srcBuf, level, numSamps, 0u);
	}
	else if (dynamic_cast<const BounceMixBehaviour*>(_behaviour.get()))
	{
		// Bounce crossfades existing destination content with the source; scaling
		// only the source would discard its complementary destination envelope.
		for (auto i = 0u; i < numSamps; ++i)
		{
			const auto level = static_cast<float>(_fade->Current());
			_behaviour->ApplyBlock(dest, srcBuf + i, level, 1u, i);
			_fade->Next();
			peak = std::max(peak, std::abs(srcBuf[i] * level));
		}
	}
	else
	{
		// Fixed stack scratch preserves sample-continuous gain without allocating
		// or extending every destination's write protocol. Steady gain keeps the
		// existing direct block path. All channels share the same gain envelope.
		float scaled[256];
		for (auto offset = 0u; offset < numSamps;)
		{
			const auto count = std::min(256u, numSamps - offset);
			for (auto i = 0u; i < count; ++i)
			{
				scaled[i] = srcBuf[offset + i] * static_cast<float>(_fade->Current());
				_fade->Next();
				peak = std::max(peak, std::abs(scaled[i]));
			}
			_behaviour->ApplyBlock(dest, scaled, 1.0f, count, offset);
			offset += count;
		}
	}
	// Meter state belongs to audio; UI visibility must not gate publication.
	_vu.SetPeak(peak, numSamps);
	_publishedFadeLevel.store(_fade->Current(), std::memory_order_release);
}

void AudioMixer::Offset(unsigned int numSamps)
{
	_ApplyFadeTargetAtAudioBoundary();
	for (auto samp = 0u; samp < numSamps; samp++)
	{
		_fade->Next();
	}
	_publishedFadeLevel.store(_fade->Current(), std::memory_order_release);
}

void AudioMixer::SetChannels(std::vector<unsigned int> channels)
{
	if (auto wireBehaviour = dynamic_cast<audio::WireMixBehaviour*>(_behaviour.get())) {
		WireMixBehaviourParams wireParams;
		wireParams.Channels = channels;
		wireBehaviour->SetParams(wireParams);
	}
	else if (auto bounceBehaviour = dynamic_cast<audio::BounceMixBehaviour*>(_behaviour.get())) {
		BounceMixBehaviourParams bounceParams;
		bounceParams.Channels = channels;
		bounceBehaviour->SetParams(bounceParams);
	}
	else if (auto mergeBehaviour = dynamic_cast<audio::MergeMixBehaviour*>(_behaviour.get())) {
		MergeMixBehaviourParams mergeParams;
		mergeParams.Channels = channels;
		mergeBehaviour->SetParams(mergeParams);
	}
}

void AudioMixer::SetMaxChannels(unsigned int chans)
{
	_behaviour->SetMaxChannels(chans);
}

void AudioMixer::SetBehaviour(std::unique_ptr<MixBehaviour> behaviour)
{
	_behaviour = std::move(behaviour);
}

void AudioMixer::ExchangeBehaviour(std::unique_ptr<MixBehaviour>& behaviour) noexcept
{
	_behaviour.swap(behaviour);
}

void AudioMixer::SetVuVisible(bool visible)
{
	_vu.SetVisible(visible);
}

void AudioMixer::UpdateVu(float peak, unsigned int numSamps)
{
	_vu.SetPeak(peak, numSamps);
}

float AudioMixer::VuLevel() const noexcept
{
	return _vu.DisplayValue();
}

float AudioMixer::VuPeakLevel() const noexcept
{
	return _vu.PeakValue();
}

float AudioMixer::VuHoldLevel() const noexcept
{
	return _vu.HoldValue();
}

void AudioMixer::DrawVu(DrawContext& ctx, utils::Size2d sliderSize)
{
	// Position the VU at the right edge of the slider area, full height.
	_vu.SetPosition({ (int)sliderSize.Width - gui::GuiVu::VuWidth, 0 });
	_vu.SetSize({ (unsigned int)gui::GuiVu::VuWidth, sliderSize.Height });
	_vu.Draw(ctx);
}

void AudioMixer::_InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	_vu.InitResources(resourceLib, forceInit);
	GuiElement::_InitResources(resourceLib, forceInit);
}

void AudioMixer::_ReleaseResources()
{
	_vu.ReleaseResources();
	GuiElement::_ReleaseResources();
}

void WireMixBehaviour::_ApplyBlockToChannels(const std::shared_ptr<MultiAudioSink>& dest,
	const float* srcBuf,
	float fadeCurrent,
	float fadeNew,
	unsigned int numSamps,
	unsigned int startIndex) const
{
	if (nullptr == dest)
		return;

	base::AudioWriteRequest request;
	request.samples = srcBuf;
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = fadeCurrent;
	request.fadeNew = fadeNew;
	request.source = base::Audible::AUDIOSOURCE_MIXER;

	for (auto chan : _mixParams.Channels)
	{
		dest->OnBlockWriteChannel(chan, request, startIndex);
	}
}

void WireMixBehaviour::ApplyBlock(const std::shared_ptr<MultiAudioSink>& dest,
	const float* srcBuf,
	float fadeLevel,
	unsigned int numSamps,
	unsigned int startIndex) const
{
	_ApplyBlockToChannels(dest, srcBuf, 0.0f, fadeLevel, numSamps, startIndex);
}

void PanMixBehaviour::ApplyBlock(const std::shared_ptr<MultiAudioSink>& dest,
	const float* srcBuf,
	float fadeLevel,
	unsigned int numSamps,
	unsigned int startIndex) const
{
	if (nullptr == dest)
		return;

	auto numChans = dest->NumInputChannels(base::Audible::AUDIOSOURCE_MIXER);

	for (auto chan = 0u; chan < numChans; chan++)
	{
		if (chan < _mixParams.ChannelLevels.size())
		{
			base::AudioWriteRequest request;
			request.samples = srcBuf;
			request.numSamps = numSamps;
			request.stride = 1;
			request.fadeCurrent = 1.0f;
			request.fadeNew = fadeLevel * _mixParams.ChannelLevels.at(chan);
			request.source = base::Audible::AUDIOSOURCE_MIXER;

			dest->OnBlockWriteChannel(chan, request, startIndex);
		}
	}
}

void BounceMixBehaviour::ApplyBlock(const std::shared_ptr<MultiAudioSink>& dest,
	const float* srcBuf,
	float fadeLevel,
	unsigned int numSamps,
	unsigned int startIndex) const
{
	if (nullptr == dest)
		return;

	base::AudioWriteRequest request;
	request.samples = srcBuf;
	request.numSamps = numSamps;
	request.stride = 1;
	request.fadeCurrent = 1.0f - fadeLevel;
	request.fadeNew = fadeLevel;
	request.source = base::Audible::AUDIOSOURCE_BOUNCE;

	for (auto chan : _mixParams.Channels)
		dest->OnBlockWriteChannel(chan, request, startIndex);
}

void MergeMixBehaviour::ApplyBlock(const std::shared_ptr<MultiAudioSink>& dest,
	const float* srcBuf,
	float fadeLevel,
	unsigned int numSamps,
	unsigned int startIndex) const
{
	_ApplyBlockToChannels(dest, srcBuf, 1.0f, fadeLevel, numSamps, startIndex);
}
