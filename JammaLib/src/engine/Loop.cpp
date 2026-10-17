#include "Loop.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace engine
{
	// JamFile gained bodyPlayIndex after legacy files had already written Index.
	// Keep this compatibility shim here so Loop construction remains valid while
	// both formats are accepted; no schema policy leaks into the audio path.
	struct LoopFileBodyPlayIndex
	{
		template <typename LoopFile>
		static unsigned long Value(const LoopFile& loop) noexcept
		{
			if constexpr (requires { loop.BodyPlayIndex; })
				return loop.BodyPlayIndex;
			return loop.Index;
		}
	};
}

void engine::Loop::_DrainVstChain(std::shared_ptr<vst::VstChain> chain)
{
	if (!chain)
		return;
	for (size_t i = 0; i < chain->NumPlugins(); ++i)
	{
		auto plugin = chain->GetPlugin(i);
		if (plugin)
			vst::QueueForUiThreadDestroy(std::move(plugin));
	}
	chain.reset();
}

using namespace base;
using namespace engine;
using namespace resources;
using base::AudioSink;
using audio::BufferBank;
using audio::AudioMixer;
using audio::Hanning;
using audio::AudioMixerParams;
using graphics::GlDrawContext;
using actions::JobAction;
using actions::ActionResult;

Loop::~Loop()
{
	_DrainVstChain(_vstChain.load(std::memory_order_acquire));
	_DrainVstChain(_backVstChain);
	ReleaseResources();
}

Loop::Loop(LoopParams params,
	AudioMixerParams mixerParams) :
	Jammable(params),
	AudioSink(),
	_visualUpdatesEnabled(true),
	_isPunchInActive(false),
	_playIndex(0),
	_loopLength(0),
	_lastPeak(0.0f),
	_pitch(1.0),
	_playState(STATE_INACTIVE),
	_loopParams(params),
	_mixer(nullptr),
	_hanning(nullptr),
	_model(nullptr),
	_bufferBank(BufferBank())
{
	if (_loopParams.Id.empty()) _loopParams.Id = utils::GetGuid();
	_mixer = std::make_unique<AudioMixer>(mixerParams);
	_hanning = std::make_unique<Hanning>(params.FadeSamps);

	LoopModelParams modelParams;
	modelParams.Size = { 12, 14 };
	modelParams.ModelScale = 1.0f;
	modelParams.ModelTextures = { "levels", "probe_pearl" };
	modelParams.ModelShaders = { "waveform", "picker", "waveform"};
	_model = std::make_shared<LoopModel>(modelParams);

	graphics::VuParams vuParams;
	vuParams.Size = { 12, 18 };
	vuParams.ModelScale = 1.0f;
	vuParams.ModelTextures = { "blue" };
	vuParams.ModelShaders = { "vu" };
	vuParams.LedHeight = 1.0f;
	_vu = std::make_shared<graphics::VU>(vuParams);

	_children.push_back(_model);
	_children.push_back(_vu);
	_children.push_back(_mixer);
}

std::optional<std::shared_ptr<Loop>> Loop::FromFile(LoopParams loopParams, io::JamFile::Loop loopStruct, std::wstring dir)
{
	audio::BehaviourParams behaviour;
	audio::WireMixBehaviourParams wire;
	audio::PanMixBehaviourParams pan;

	std::vector<unsigned long> chans;
	std::vector<double> levels;

	switch (loopStruct.Mix.Mix)
	{
	case io::JamFile::LoopMix::MIX_WIRE:
		if (loopStruct.Mix.Params.index() == 0)
			chans = std::get<std::vector<unsigned long>>(loopStruct.Mix.Params);

		for (auto chan : chans)
			wire.Channels.push_back(chan);

		behaviour = wire;
		break;
	case io::JamFile::LoopMix::MIX_PAN:
		if (loopStruct.Mix.Params.index() == 1)
			levels = std::get<std::vector<double>>(loopStruct.Mix.Params);

		for (auto level : levels)
			pan.ChannelLevels.push_back((float)level);

		behaviour = pan;
		break;
	}

	auto mixerParams = GetMixerParams(loopParams.Size,
		behaviour);

	loopParams.Wav = utils::EncodeUtf8(dir) + "/" + loopStruct.Name;
	loopParams.Id = loopStruct.Id.empty() ? utils::GetGuid() : loopStruct.Id;
	loopParams.Channel = loopStruct.Channel;
	auto loop = std::make_shared<Loop>(loopParams, mixerParams);

	if (!loop->Load(io::WavReadWriter()) || loopStruct.Length == 0ul
		|| loopStruct.Length > loop->LoopLength())
		return std::nullopt;

	const auto bodyPlayIndex = LoopFileBodyPlayIndex::Value(loopStruct);
	if (bodyPlayIndex >= loopStruct.Length)
		return std::nullopt;

	// Play consumes a fade-prefixed raw index. bodyPlayIndex is deliberately
	// fade-free, so restore it only after establishing the logical loop state.
	loop->Play(constants::MaxLoopFadeSamps, loopStruct.Length, false);
	loop->SetBodyPlayIndex(bodyPlayIndex);
	loop->SetMixerLevel(loopStruct.Level);
	loop->_pitch = loopStruct.Speed;
	if (loopStruct.Muted)
		loop->Mute();

	return loop;
}

AudioMixerParams Loop::GetMixerParams(utils::Size2d loopSize,
	audio::BehaviourParams behaviour)
{
	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, loopSize.Height };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = behaviour;

	return mixerParams;
}

void Loop::SetAudioSpectralModel(std::shared_ptr<graphics::AudioSpectralModel> model)
{
	if (_audioSpectralModel == model) return;
	if (_audioSpectralModel)
		_children.erase(std::remove(_children.begin(), _children.end(), _audioSpectralModel), _children.end());
	_audioSpectralModel = std::move(model);
	if (_audioSpectralModel) _children.push_back(_audioSpectralModel);
}

void Loop::SetSize(utils::Size2d size)
{
	auto mixerParams = GetMixerParams(size,
		audio::WireMixBehaviourParams());

	_mixer->SetSize(mixerParams.Size);

	GuiElement::SetSize(size);
}

graphics::CameraBounds Loop::CameraBoundsInParent(float parentVisualScale) const
{
	if (!_model)
		return {};
	auto bounds = _model->CameraLocalBounds();
	const auto heightScale = std::fabs(static_cast<float>(_mixer->UnmutedLevel()) *
		parentVisualScale);
	bounds.Min.Y *= heightScale;
	bounds.Max.Y *= heightScale;
	if (_vu)
	{
		// VU::CalcLedGeometry has radius/15 thickness; Draw3d stretches its
		// 1.5-spaced one-unit LEDs by four. Reserve the whole meter, independent of level.
		const auto vuRadius = bounds.Max.X * 1.02f;
		const auto vuHalfHeight = static_cast<float>(_vu->GetSize().Height) * 6.0f + 2.0f;
		graphics::CameraBounds vuBounds{ { -vuRadius, -vuHalfHeight, -vuRadius },
			{ vuRadius, vuHalfHeight, vuRadius } };
		auto vuPosition = _vu->ModelPosition();
		vuPosition.Y *= 4.0f;
		bounds.Include(vuBounds.Transformed(vuPosition, static_cast<float>(_vu->ModelScale())));
	}
	return bounds.Transformed(ModelPosition(), static_cast<float>(ModelScale()));
}

float Loop::UnfittedVisualHalfHeight(float parentVisualScale) const
{
	return _model ? _model->UnfittedHalfHeight() * std::fabs(static_cast<float>(_mixer->UnmutedLevel()) *
		parentVisualScale * static_cast<float>(ModelScale())) : 0.0f;
}

void Loop::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto playState = _playState.load(std::memory_order_acquire);
	auto loopLength = _loopLength.load(std::memory_order_relaxed);

	auto pos = ModelPosition();
	auto scale = ModelScale();

	auto isRecording = (STATE_RECORDING == playState) ||
		(STATE_OVERDUBBING == playState) ||
		(STATE_PUNCHEDIN == playState);
	auto index = isRecording ?
		_writeIndex.load(std::memory_order_relaxed) :
		_playIndex.load(std::memory_order_relaxed);

	if (!isRecording && index >= constants::MaxLoopFadeSamps)
		index -= constants::MaxLoopFadeSamps;

	auto frac = loopLength == 0 ? 0.0 : 1.0 - std::max(0.0, std::min(1.0, ((double)(index % loopLength)) / ((double)loopLength)));
	_model->SetLoopIndexFrac(frac);
	_model->SetLoopState(_GetLoopModelState(pass, playState, IsMuted()));
	_model->SetMuted(IsMuted());

	_modelScreenPos = glCtx.ProjectScreen(pos);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	if (PASS_SCENE == pass)
		_vu->Draw3d(ctx, 1, pass);

	const auto spectralVisible = _audioSpectralModel && _audioSpectralModel->IsAvailable();
	_model->SetSpectralReveal(spectralVisible ? _audioSpectralMorph : 0.0f);
	const auto visualHeightScale = static_cast<float>(_mixer->UnmutedLevel()) * _masterVisualScale;
	_model->SetWaveformColorScale(visualHeightScale);
	glCtx.PushMvp(glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, visualHeightScale, 1.0f)));
	
	GLboolean depthWrites = GL_TRUE;
	const auto fadingWaveform = PASS_SCENE == pass && spectralVisible && _audioSpectralMorph > 0.0f;
	if (fadingWaveform)
	{
		glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrites);
		glDepthMask(GL_FALSE);
	}
	_model->Draw3d(ctx, 1, pass);
	if (fadingWaveform) glDepthMask(depthWrites);
	
	glCtx.PopMvp();
	if (_audioSpectralModel && _audioSpectralMorph > 0.0f)
	{
		_audioSpectralModel->SetRadius(static_cast<float>(CalcDrawRadius(loopLength) * _DrawRadiusScale()));
		_audioSpectralModel->SetPlayFraction(loopLength == 0ul ? 0.0
			: static_cast<double>(BodyPlayIndex()) / loopLength);
		_audioSpectralModel->Draw3d(ctx, numInstances, pass);
	}
	glCtx.PopMvp();
	glCtx.PopMvp();
}

void Loop::OnBlockWrite(const base::AudioWriteRequest& request, int writeOffset)
{
	auto playState = _playState.load(std::memory_order_acquire);
	if ((STATE_RECORDING != playState) &&
		(STATE_PLAYINGRECORDING != playState) &&
		(STATE_OVERDUBBING != playState) &&
		(STATE_PUNCHEDIN != playState) &&
		(STATE_OVERDUBBINGRECORDING != playState) &&
		!_isPunchInActive.load(std::memory_order_acquire))
		return;

	if (AUDIOSOURCE_ADC == request.source)
	{
		auto canWriteAdc = (STATE_RECORDING == playState) ||
			(STATE_PLAYINGRECORDING == playState) ||
			(STATE_PUNCHEDIN == playState) ||
			_isPunchInActive.load(std::memory_order_acquire);
		if (!canWriteAdc)
			return;
	}
	else if ((AUDIOSOURCE_BOUNCE == request.source) &&
		(STATE_RECORDING == playState))
	{
		return;
	}
	else if ((STATE_OVERDUBBING == playState) &&
		(AUDIOSOURCE_BOUNCE != request.source))
	{
		return;
	}

	const auto writeIndex = _triggerCapture ? _triggerCaptureCursor :
		static_cast<std::int64_t>(_writeIndex.load(std::memory_order_relaxed));
	const auto captureEnd = _triggerCaptureEnd.load(std::memory_order_acquire);

	if (AUDIOSOURCE_MONITOR == request.source)
	{
		float peak = _lastPeak;

		for (unsigned int i = 0; i < request.numSamps; i++)
		{
			auto samp = request.samples[i * request.stride];
			const auto signedIndex = writeIndex + writeOffset + i;
			if (signedIndex < 0 || signedIndex >= _monitorBufferBank.Capacity()) continue;
			const auto idx = static_cast<unsigned long>(signedIndex);
			_monitorBufferBank[idx] = (request.fadeNew * samp) + (request.fadeCurrent * _monitorBufferBank[idx]);

			if (STATE_RECORDING == playState)
			{
				auto absSamp = std::abs(samp);
				if (absSamp > peak)
					peak = absSamp;
			}
		}

		_lastPeak = peak;
	}
	else
	{
		if (request.numSamps > 0u)
			_InvalidateAudioSource();
		for (unsigned int i = 0; i < request.numSamps; i++)
		{
			auto samp = request.samples[i * request.stride];
			const auto signedIndex = writeIndex + writeOffset + i;
			if (signedIndex < 0 || signedIndex >= _bufferBank.Capacity()
				|| (_triggerCapture && signedIndex >= captureEnd)) continue;
			const auto idx = static_cast<unsigned long>(signedIndex);
			_bufferBank[idx] = (request.fadeNew * samp) + (request.fadeCurrent * _bufferBank[idx]);
		}
		if (_triggerCapture)
		{
			auto& through = request.source == AUDIOSOURCE_ADC ? _triggerAdcThrough : _triggerBounceThrough;
			const auto first = (std::max)(writeIndex + writeOffset, _triggerCaptureBegin);
			const auto end = (std::min)({ writeIndex + writeOffset + request.numSamps,
				captureEnd, static_cast<std::int64_t>(_bufferBank.Capacity()) });
			if (first <= through.load(std::memory_order_relaxed) && end > first)
				through.store(end, std::memory_order_release);
			if (request.source == AUDIOSOURCE_ADC && first <= _triggerAdcBlockThrough && end > first)
				_triggerAdcBlockThrough = end;
		}
	}
}

void Loop::EndWrite(unsigned int numSamps,
	bool updateIndex)
{
	// Only update if currently recording
	auto playState = _playState.load(std::memory_order_acquire);
	if ((STATE_RECORDING != playState) &&
		(STATE_PLAYINGRECORDING != playState) &&
		(STATE_OVERDUBBING != playState) &&
		(STATE_PUNCHEDIN != playState) &&
		(STATE_OVERDUBBINGRECORDING != playState) &&
		!_isPunchInActive.load(std::memory_order_acquire))
		return;

	if (!updateIndex)
		return;

	const auto next = _triggerCapture ? _triggerCaptureCursor + numSamps :
		static_cast<std::int64_t>(_writeIndex.load(std::memory_order_relaxed)) + numSamps;
	if (_triggerCapture)
	{
		const auto requiredEnd = (std::min)(next, _triggerCaptureEnd.load(std::memory_order_acquire));
		if (requiredEnd > _triggerCaptureBegin &&
			((_triggerCaptureRequiresAdc && _triggerAdcThrough.load(std::memory_order_acquire) < requiredEnd)
			|| (_triggerCaptureRequiresBounce && _triggerBounceThrough.load(std::memory_order_acquire) < requiredEnd)
			|| (_triggerPunchRequiresAdc && _isPunchInActive.load(std::memory_order_acquire) && _triggerAdcBlockThrough < requiredEnd)))
			_triggerCaptureCoverageValid.store(false, std::memory_order_release);
		_triggerCaptureCursor = next;
	}
	const auto writeIndex = static_cast<unsigned long>((std::clamp)(next, std::int64_t{0},
		static_cast<std::int64_t>(_bufferBank.Capacity())));
	_writeIndex.store(writeIndex, std::memory_order_relaxed);
	_bufferBank.SetLength(writeIndex);

	if (STATE_RECORDING == playState)
	{
		_monitorBufferBank.SetLength(writeIndex);

		auto newValue = _lastPeak * _mixer->Level();
		_vu->SetValue(newValue, numSamps);
		_lastPeak = 0.0f;
	}
}

// Shared by DAC and recorded-only bounce readers. Only DAC supplies context.
unsigned int Loop::ReadBlock(float* outBuf,
	int sampOffset,
	unsigned int numSamps, const audio::DacRenderContext* dac,
    std::uint32_t trigger, std::int64_t delay, unsigned int channel)
{
	auto playState = _playState.load(std::memory_order_acquire);
	auto loopLength = _loopLength.load(std::memory_order_relaxed);
	auto isPlaying = (STATE_PLAYING == playState) ||
		(STATE_PLAYINGRECORDING == playState) ||
		(STATE_OVERDUBBINGRECORDING == playState) ||
		(STATE_PUNCHEDIN == playState);

	if (!isPlaying || (0 == loopLength))
		return 0;

	const auto sourceGeneration = AudioSourceGeneration();
	auto peak = 0.0f;
	auto bufSize = loopLength + constants::MaxLoopFadeSamps;

	// _playIndex is always within range:
	// [constants::MaxLoopFadeSamps : _loopLength + constants::MaxLoopFadeSamps - 1)
	// index should apply the offset and then also stay in the same range
	auto index = _playIndex.load(std::memory_order_relaxed);

	if (sampOffset >= 0)
	{
		index += sampOffset;
	}
	else
	{
		while (index < (unsigned long)(-sampOffset))
			index += loopLength;

		index += sampOffset;
	}

	while (index >= bufSize)
		index -= loopLength;

	if (index < constants::MaxLoopFadeSamps)
		index += loopLength;

    const bool ready = !_triggerCapture || TriggerCaptureReady(index - constants::MaxLoopFadeSamps, numSamps);
    const bool provisional = !ready && dac && CanUseTemporaryPlayback(index - constants::MaxLoopFadeSamps, numSamps)
        && dac->History.ContainsOrigin(trigger);
    if (!ready && !provisional) return 0u;
    // Read logical extent after the readiness acquire, including normalization.
    const auto bufBankSize = _bufferBank.Length();
    // Snapshot normalization before selecting a bank address. While false, never
    // touch its destination tail: the job thread may be copying it concurrently.
    const bool normalized = provisional && _triggerCaptureNormalized.load(std::memory_order_acquire);
    const auto through = provisional ? _triggerAdcThrough.load(std::memory_order_acquire) : 0;
    const auto readSource = [&](unsigned long storage, unsigned int frame) noexcept {
        if (!provisional) return _ReadAudioSpectralSample(storage, frame, sourceGeneration);
        const auto f = static_cast<std::int64_t>(constants::MaxLoopFadeSamps);
        auto selected = static_cast<std::int64_t>(storage);
        if (selected >= f)
        {
            // Selected pass is [begin,begin+L). Tail content lives in prefix
            // storage until normalization; it is never another captured pass.
            if (selected >= _triggerCaptureBegin + loopLength) selected -= loopLength;
            if (normalized) return _bufferBank[storage];
        }
        else if (selected < _triggerCaptureBegin) return 0.0f; // unpublished prefix excluded by eligibility
        if (selected >= _triggerCaptureBegin && selected < through)
            return _bufferBank[static_cast<unsigned long>(selected)];
        float sample = 0.0f;
        const auto physical = trigger + static_cast<std::uint32_t>(selected - f - delay);
        dac->History.Read(channel, physical, sample);
        return sample;
    };

	// Check if we are inside crossfading region at any point
	auto isXfadeRegion = (index + numSamps) >= (bufSize - _loopParams.FadeSamps);

	auto sampsToWrite = (numSamps <= constants::MaxBlockSize) ? numSamps : constants::MaxBlockSize;

	if (isXfadeRegion)
	{
		// Fill temp buffer with crossfade-mixed samples
		auto startIndex = index;

		for (auto i = 0u; i < sampsToWrite; i++)
		{
			auto isXfade = (startIndex + i) >= (bufSize - _loopParams.FadeSamps);
			isXfade &= index < bufSize;

			if (provisional || index < bufBankSize)
			{
				auto samp = readSource(index, i);

				if (isXfade)
				{
					auto xfadeIndex = (startIndex + i) - (bufSize - _loopParams.FadeSamps);
					auto xfadeBufIndex = constants::MaxLoopFadeSamps + xfadeIndex - _loopParams.FadeSamps;
					auto xfadeSamp = readSource(xfadeBufIndex, i);

					samp = _hanning->Mix(xfadeSamp, samp, xfadeIndex);
				}

				outBuf[i] = samp;

				if (std::abs(samp) > peak)
					peak = std::abs(samp);
			}
			else
			{
				outBuf[i] = 0.0f;
			}

			index++;
			if (index >= bufSize)
				index -= loopLength;
		}
	}
	else
	{
		// Non-crossfade: fill output buffer from BufferBank, handling wrap-around
		for (auto i = 0u; i < sampsToWrite; i++)
		{
			if (provisional || index < bufBankSize)
			{
				outBuf[i] = readSource(index, i);

				if (std::abs(outBuf[i]) > peak)
					peak = std::abs(outBuf[i]);
			}
			else
			{
				outBuf[i] = 0.0f;
			}

			index++;
			if (index >= bufSize)
				index -= loopLength;
		}
	}

	_lastPeak = peak;
	return sampsToWrite;
}

// Only called when outputting to DAC
void Loop::PollPlaybackPluginChanges() const noexcept
{
	const auto chain = _vstChain.load(std::memory_order_acquire);
	if (chain) chain->PollPendingChanges();
}

bool Loop::CanConfigureRecordedPlaybackDelay(unsigned int delaySamps) const
{
	return _playbackDelay && _playbackDelay->CanConfigure(delaySamps);
}

bool Loop::ConfigureRecordedPlaybackDelay(unsigned int delaySamps)
{
	return _playbackDelay && _playbackDelay->Configure(delaySamps);
}

void Loop::ClearRecordedPlaybackDelay() noexcept
{
	if (_playbackDelay) _playbackDelay->Clear();
}

void Loop::WriteBlock(const std::shared_ptr<MultiAudioSink> dest,
	const std::shared_ptr<base::BounceWriter> bounceWriter,
	int sampOffset,
	unsigned int numSamps, const audio::DacRenderContext* dac,
    std::uint32_t trigger, std::int64_t delay, unsigned int channel)
{
	// Mixer will stereo spread the mono wav
	// and adjust level
	auto loopLength = _loopLength.load(std::memory_order_relaxed);
	auto playState = _playState.load(std::memory_order_relaxed);
	if (0 == loopLength)
	{
		_mixer->UpdateVu(0.0f, numSamps);
		ClearRecordedPlaybackDelay();
		return;
	}

	if (STATE_RECORDING == playState)
		_mixer->Offset(numSamps);

	// Read source data from BufferBank into stack-allocated temp buffer
	float tempBuf[constants::MaxBlockSize];
	auto sampsToWrite = ReadBlock(tempBuf, sampOffset, numSamps, bounceWriter ? nullptr : dac, trigger, delay, channel);
	if (sampsToWrite == 0u && bounceWriter == nullptr)
		ClearRecordedPlaybackDelay();

	if (sampsToWrite > 0)
	{
		auto chain = _vstChain.borrow();
		if (chain && chain->IsActive())
			chain->ProcessBlock(tempBuf, static_cast<int>(sampsToWrite));

		// Only rendered playback is delayed; bounce and source capture keep
		// their original sample coordinates.
		if (bounceWriter == nullptr && _playbackDelay)
		{
			if (IsMuted()) _playbackDelay->Clear();
			else _playbackDelay->Process(tempBuf, sampsToWrite);
		}

		// Route to destination via mixer or trigger
		// (both ultimately call dest->OnBlockWriteChannel)
		if (nullptr == bounceWriter)
			_mixer->WriteBlock(dest, tempBuf, sampsToWrite);
		else
			bounceWriter->WriteBlock(dest, tempBuf, sampsToWrite, LoopChannel());
	}
	else
		_mixer->UpdateVu(0.0f, numSamps);
}

void Loop::EndMultiPlay(unsigned int numSamps)
{
	_CopyAudioSpectralSourceAtAudioBoundary();
	_AdvanceAudioSpectralPlayback(numSamps);
	auto playState = _playState.load(std::memory_order_relaxed);
	auto loopLength = _loopLength.load(std::memory_order_relaxed);
	auto isPlaying = (STATE_PLAYING == playState) ||
		(STATE_PLAYINGRECORDING == playState) ||
		(STATE_OVERDUBBING == playState) ||
		(STATE_PUNCHEDIN == playState) ||
		(STATE_OVERDUBBINGRECORDING == playState);

	if (!isPlaying)
		return;

	if (0 == loopLength)
		return;
		
	auto playIndex = _playIndex.load(std::memory_order_relaxed) + numSamps;

	auto bufSize = loopLength + constants::MaxLoopFadeSamps;
	while (playIndex >= bufSize)
		playIndex -= loopLength;
	_playIndex.store(playIndex, std::memory_order_relaxed);

	for (unsigned int chan = 0; chan < NumOutputChannels(Audible::AUDIOSOURCE_LOOPS); chan++)
	{
		const auto& channel = _OutputChannel(chan);

		if (channel)
			channel->EndPlay(numSamps);
	}

	auto newValue = _lastPeak * _mixer->Level();
	_vu->SetValue(newValue, numSamps);
}

void Loop::ShiftPlayIndex(long long deltaSamps) noexcept
{
	const auto loopLength = _loopLength.load(std::memory_order_relaxed);
	if (loopLength == 0ul || deltaSamps == 0)
		return;

	const auto playIndex = _playIndex.load(std::memory_order_relaxed);
	// First-play cursors may sit one body before the fade prefix. Resolve
	// them against the same playable-body origin as ReadBlock.
	const auto bodyIndex = (static_cast<unsigned long long>(playIndex % loopLength)
		+ loopLength - (constants::MaxLoopFadeSamps % loopLength)) % loopLength;
	const auto length = static_cast<long long>(loopLength);
	auto shifted = (static_cast<long long>(bodyIndex) + (deltaSamps % length)) % length;
	if (shifted < 0)
		shifted += length;
	_playIndex.store(constants::MaxLoopFadeSamps + static_cast<unsigned long>(shifted),
		std::memory_order_relaxed);
}

unsigned int Loop::LoopChannel() const
{
	return _loopParams.Channel;
}

void Loop::SetLoopChannel(unsigned int channel)
{
	_loopParams.Channel = channel;
}

std::string Loop::Id() const
{
	return _loopParams.Id;
}

std::vector<float> Loop::ExportSamples(bool retainPhysicalTail) const
{
	return ExportOriginalSamplesQuiescent(retainPhysicalTail);
}

std::vector<float> Loop::ExportOriginalSamplesQuiescent(bool retainPhysicalTail) const
{
	auto playState = _playState.load(std::memory_order_acquire);
	auto loopLength = _loopLength.load(std::memory_order_relaxed);

	if ((0 == loopLength) || (STATE_INACTIVE == playState))
		return {};
	// Session sidecars retain recorded samples beyond a rounded logical boundary.
	// The manifest still carries logical length; FromFile restores it after Load.
	if (retainPhysicalTail)
		loopLength = (std::max)(loopLength, PhysicalLoopLength());

	std::vector<float> out(loopLength);
	unsigned long copied = 0;
	while (copied < loopLength)
	{
		const auto idx = constants::MaxLoopFadeSamps + copied;
		const auto bankOffset = idx % BufferBank::_BufferBankSize;
		const auto samplesInBank = BufferBank::_BufferBankSize - bankOffset;
		const auto chunkSize = std::min(loopLength - copied, samplesInBank);
		const auto* ptr = _bufferBank.BlockPtr(idx);

		if (ptr != nullptr)
			std::copy_n(ptr, chunkSize, out.data() + copied);
		else
			std::fill(out.data() + copied, out.data() + copied + chunkSize, 0.0f);

		copied += chunkSize;
	}

	return out;
}

io::JamFile::Loop Loop::ToJamFile(const std::string& wavFilename) const
{
	auto loopLength = _loopLength.load(std::memory_order_relaxed);
	auto playIndex = _playIndex.load(std::memory_order_relaxed);

	io::JamFile::Loop loop;
	loop.Name = wavFilename;
	loop.Id = _loopParams.Id;
	loop.Channel = _loopParams.Channel;
	loop.Length = loopLength;
	loop.Index = (playIndex >= constants::MaxLoopFadeSamps) ?
		(playIndex - constants::MaxLoopFadeSamps) :
		0ul;
	loop.BodyPlayIndex = BodyPlayIndex();
	loop.MasterLoopCount = 0;
	loop.Level = _mixer->UnmutedLevel();
	loop.Speed = _pitch;
	loop.MuteGroups = 0;
	loop.SelectGroups = 0;
	loop.Muted = IsMuted();

	auto params = _mixer->GetBehaviourParams();
	if (auto* wire = std::get_if<audio::WireMixBehaviourParams>(&params))
	{
		loop.Mix.Mix = io::JamFile::LoopMix::MIX_WIRE;
		std::vector<unsigned long> channels;
		for (auto chan : wire->Channels)
			channels.push_back(chan);
		loop.Mix.Params = std::move(channels);
	}
	else if (auto* pan = std::get_if<audio::PanMixBehaviourParams>(&params))
	{
		loop.Mix.Mix = io::JamFile::LoopMix::MIX_PAN;
		std::vector<double> levels;
		for (auto level : pan->ChannelLevels)
			levels.push_back(level);
		loop.Mix.Params = std::move(levels);
	}
	else
	{
		loop.Mix.Mix = io::JamFile::LoopMix::MIX_PAN;
		loop.Mix.Params = std::vector<double>{ 0.5, 0.5 };
	}

	auto chain = _vstChain.load(std::memory_order_acquire);
	std::vector<std::wstring> paths;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		paths = _vstPluginPaths;
	}
	// Plugin state calls may pump native messages and reenter this owner.
	{
		for (size_t i = 0; i < paths.size(); ++i)
		{
			io::JamFile::VstEntry entry;
			entry.Path = utils::EncodeUtf8(paths[i]);
			entry.Bypass = false;

			if (chain && i < chain->NumPlugins())
			{
				auto plugin = chain->GetPlugin(i);
				if (plugin)
				{
					entry.Id = plugin->StateId();
					auto blob = plugin->GetState();
					if (!blob.empty())
						entry.State = io::JamFile::VstEntry::EncodeState(blob);
				}
			}

			loop.VstChain.push_back(std::move(entry));
		}
	}

	return loop;
}

void Loop::SetMixerLevel(double level)
{
	_mixer->SetUnmutedLevel(level);
}

void Loop::SetMasterVisualScale(float scale) noexcept
{
	_masterVisualScale = std::max(0.0f, scale);
}

void Loop::SetVisualUpdatesEnabled(bool enabled)
{
	_visualUpdatesEnabled = enabled;
}

void Loop::ForceUnloadAllVstPlugins()
{
	auto chain = _vstChain.exchange(nullptr, std::memory_order_acq_rel);
	_DrainVstChain(std::move(chain));
	_DrainVstChain(std::move(_backVstChain));
	_flipVstChain.store(false, std::memory_order_release);
	_pendingVstLoads.clear();
	_pendingVstUnloads.clear();
	_vstPluginPaths.clear();
}

bool Loop::Load(const io::WavReadWriter& readWriter)
{
	// Startup/exclusive operation: no audio reader may use this Loop during Load.
	_InvalidateAudioSource();
	auto loadOpt = readWriter.Read(utils::DecodeUtf8(_loopParams.Wav), constants::MaxLoopBufferSize);

	if (!loadOpt.has_value())
		return false;

	auto [buffer, sampleRate, bitDepth] = loadOpt.value();

	_loopLength.store(0, std::memory_order_relaxed);
	_bufferBank.Init();

	const auto length = static_cast<unsigned long>(buffer.size());
	if (length == 0ul || length > constants::MaxLoopBufferSize - constants::MaxLoopFadeSamps)
		return false;

	// WAV sidecars contain the fade-free logical body (ExportSamples deliberately
	// strips the internal prefix). Recreate that private prefix before publishing
	// a playable loop, rather than shortening the restored logical length.
	const auto physicalLength = length + constants::MaxLoopFadeSamps;
	_bufferBank.Resize(physicalLength);

	for (auto i = 0u; i < length; i++)
		_bufferBank[constants::MaxLoopFadeSamps + i] = buffer[i];

	_loopLength.store(length, std::memory_order_relaxed);

	_UpdateLoopModel();

	return true;
}

void Loop::Record()
{
	if (STATE_INACTIVE != _playState.load(std::memory_order_relaxed))
		return;

	Reset();
	_bufferBank.Resize(constants::MaxLoopFadeSamps);

	_monitorBufferBank.Resize(constants::MaxLoopFadeSamps);
	_playState.store(STATE_RECORDING, std::memory_order_release);
}

void Loop::Play(unsigned long index,
	unsigned long loopLength,
	bool continueRecording)
{
	InvalidateSceneAnchor();
	auto physBufSize = _bufferBank.Length();

	// A short take can close before any delayed ADC sample reaches storage.
	// Keep its capture alive; readiness silences unavailable repeats while phase advances.
	if (0 == physBufSize && !(_triggerCapture && continueRecording && loopLength > 0ul))
	{
		Reset();
		return;
	}

	// Preserve the requested musical phase; source readiness is checked on read.
	// Clamp against the smaller of the logical loop size and the currently
	// recorded physical size. Keep the MaxLoopFadeSamps logical offset in this
	// bound: playback indices are in [fadeOffset, fadeOffset + loopLength).
	auto logicalBufSize = loopLength + constants::MaxLoopFadeSamps;
	auto effectiveBufSize = std::min(logicalBufSize, physBufSize);
	_playIndex.store(_triggerCapture ? index :
		((effectiveBufSize > 0 && index >= effectiveBufSize) ? (effectiveBufSize - 1) : index), std::memory_order_relaxed);
	if (_loopLength.load(std::memory_order_relaxed) != loopLength)
		_InvalidateAudioSource();
	_loopLength.store(loopLength, std::memory_order_relaxed);
	if (_isPunchInActive.load(std::memory_order_relaxed))
		continueRecording = true;

	auto playState = _playState.load(std::memory_order_relaxed);
	auto isOverdubbing = (STATE_OVERDUBBING == playState) || (STATE_PUNCHEDIN == playState);
	if (_isPunchInActive.load(std::memory_order_relaxed))
		isOverdubbing = true;
	auto recordState = isOverdubbing ? STATE_OVERDUBBINGRECORDING : STATE_PLAYINGRECORDING;
	auto nextPlayState = continueRecording ? recordState : STATE_PLAYING;

	// Pre-allocate buffer capacity for recording state to prevent SetLength clamping.
	// This ensures the full loop length can be written during overdub/recording.
	if ((loopLength > 0) &&
		((STATE_OVERDUBBINGRECORDING == nextPlayState) || (STATE_PLAYINGRECORDING == nextPlayState)) &&
		(_bufferBank.Capacity() < logicalBufSize))
	{
		_bufferBank.Resize(logicalBufSize);
	}

	_playState.store(loopLength > 0 ? nextPlayState : STATE_INACTIVE, std::memory_order_release);
}

void Loop::Reset()
{
	_InvalidateAudioSource();
	_isPunchInActive.store(false, std::memory_order_relaxed);

	_triggerCapture = false;
	_writeIndex.store(0ul, std::memory_order_relaxed);
	_playIndex.store(0ul, std::memory_order_relaxed);
	_loopLength.store(0ul, std::memory_order_relaxed);
	_playState.store(STATE_INACTIVE, std::memory_order_release);
	_mixer->UnMute();
	_mixer->SetUnmutedLevel(AudioMixer::DefaultLevel);
}

void Loop::Update()
{
	RefreshVisualModel();
	UpdateCapacity();
}

void Loop::UpdateCapacity()
{
	_bufferBank.UpdateCapacity();
	_monitorBufferBank.UpdateCapacity();
}

void Loop::RefreshVisualModel()
{
	if (!CanRefreshVisualModel())
		return;

	_UpdateLoopModel();
}

bool Loop::CanEditAudioSource() const noexcept
{
	if (!SupportsAudioSpectralEditing()
		|| _playState.load(std::memory_order_acquire) != STATE_PLAYING
		|| _isPunchInActive.load(std::memory_order_acquire))
		return false;

	const auto length = _loopLength.load(std::memory_order_relaxed);
	return length > 0ul && length <= PhysicalLoopLength();
}

spectral::AudioSpectralSourceLayout Loop::_AudioSpectralSourceLayout() const noexcept
{
	return { _loopLength.load(std::memory_order_relaxed), PhysicalLoopLength(),
		_hanning->Size(), _sampleRate.load(std::memory_order_relaxed) };
}

void Loop::_InvalidateAudioSource() noexcept
{
	_audioSourceGeneration.fetch_add(1u, std::memory_order_acq_rel);
	_changesMade.store(true, std::memory_order_release);
}

void Loop::SetSampleRate(float sampleRate)
{
	// Device configuration is changed with its callback stopped.
	if (_sampleRate.load(std::memory_order_relaxed) != sampleRate)
		_InvalidateAudioSource();
	_sampleRate.store(sampleRate, std::memory_order_relaxed);
}

std::shared_ptr<spectral::AudioSpectralSourceCapture> Loop::RequestAudioSpectralSource(std::string& reason,
    const std::shared_ptr<spectral::AudioSpectralResourceBudget>& budget, const spectral::AudioSpectralSourceLayout* retainedLayout)
{
	reason.clear();
	CancelAudioSpectralSourceCapture();
	if (!CanEditAudioSource())
	{
		reason = "Finish recording or punch before editing";
		return nullptr;
	}
	const auto generation = _audioSourceGeneration.load(std::memory_order_acquire);
	const auto layout = _AudioSpectralSourceLayout();
	if (!layout.IsValid())
	{
		reason = "Audio source layout is unavailable";
		return nullptr;
	}
	if (static_cast<std::uint64_t>(layout.StoredSampleCount()) * sizeof(float)
		> spectral::AudioSpectralSourceCapture::MaxSourceBytes)
	{
		reason = "Audio source exceeds the editor memory budget";
		return nullptr;
	}
	std::shared_ptr<const spectral::AudioSpectralResourceBudget::Reservation> resources;
    auto admissionLayout = layout;
    if (retainedLayout)
    {
        admissionLayout.LogicalLength = (std::max)(admissionLayout.LogicalLength, retainedLayout->LogicalLength);
        admissionLayout.PhysicalLength = (std::max)(admissionLayout.PhysicalLength, retainedLayout->PhysicalLength);
        admissionLayout.FadeSamps = (std::max)(admissionLayout.FadeSamps, retainedLayout->FadeSamps);
    }
    spectral::AudioSpectralTransformConfig transform;
    if (!spectral::AudioSpectralDsp::SelectTransform(admissionLayout, budget, transform, resources, reason))
        return nullptr;
    std::shared_ptr<spectral::AudioSpectralSourceCapture> request;
	try
	{
		request = std::make_shared<spectral::AudioSpectralSourceCapture>(layout,
			generation, _audioSourceIdentity, std::move(resources), transform);
	}
	catch (const std::bad_alloc&)
	{
		reason = "Not enough memory to capture the audio source";
		return nullptr;
	}
	{
		std::scoped_lock lock(_audioSourcePublicationMutex);
		_audioSourceCapture.store(request);
	}
	return request;
}

void Loop::CancelAudioSpectralSourceCapture()
{
	std::scoped_lock lock(_audioSourcePublicationMutex);
	if (const auto request = _audioSourceCapture.load())
		request->Cancel();
	_audioSourceCapture.store(nullptr);
	_changesMade = true; // Pump retirement after the callback releases its borrow.
}

std::size_t Loop::_CollectAudioSourceCaptures()
{
	std::scoped_lock lock(_audioSourcePublicationMutex);
	const auto edit = _audioSpectralEdit.load(std::memory_order_acquire);
	if (edit && edit->Source->Generation != AudioSourceGeneration())
		_audioSpectralEdit.store(nullptr, std::memory_order_release);
	const auto playback = _audioSpectralPlayback.load();
	if (playback && playback->Source().Generation != AudioSourceGeneration())
		_audioSpectralPlayback.store(nullptr);
	return _audioSourceCapture.collect() + _audioSpectralPlayback.collect();
}

bool Loop::PublishAudioSpectralEdit(
	std::shared_ptr<const spectral::AudioSpectralEditState> state, std::string& reason)
{
	reason.clear();
	if (!state || !IsCurrentAudioSpectralSource(*state->Source))
	{
		reason = "Audio edit belongs to an obsolete or unavailable source";
		return false;
	}
	std::scoped_lock lock(_audioSourcePublicationMutex);
	const auto current = _audioSpectralEdit.load(std::memory_order_acquire);
	if (current && current->Source->Generation == state->Source->Generation)
	{
		if (state->Revision <= current->Revision || state->Transform != current->Transform
			|| state->SpectralRevision < current->SpectralRevision
			|| state->EnvelopeRevision < current->EnvelopeRevision
			|| (state->SpectralRevision == current->SpectralRevision && state->Gains != current->Gains)
			|| (state->EnvelopeRevision == current->EnvelopeRevision && state->Envelope != current->Envelope))
		{
			reason = "Audio edit would replace a newer or inconsistent component revision";
			return false;
		}
	}
	if (!IsCurrentAudioSpectralSource(*state->Source))
	{
		reason = "Audio source changed before edit publication";
		return false;
	}
	_audioSpectralEdit.store(std::move(state), std::memory_order_release);
	_changesMade = true;
	return true;
}

std::shared_ptr<const spectral::AudioSpectralEditState> Loop::AudioSpectralEdit() const
{
	const auto state = _audioSpectralEdit.load(std::memory_order_acquire);
	return state && IsCurrentAudioSpectralSource(*state->Source) ? state : nullptr;
}

bool Loop::PublishAudioSpectralPlayback(
	std::shared_ptr<const spectral::AudioSpectralPlaybackSnapshot> snapshot, std::string& reason)
{
	reason.clear();
	if (!snapshot || !IsCurrentAudioSpectralSource(snapshot->Source()))
	{
		reason = "Edited playback belongs to an obsolete or unavailable source";
		return false;
	}
	std::scoped_lock lock(_audioSourcePublicationMutex);
	if (snapshot->Source().Generation == _minimumAudioSpectralGeneration.load(std::memory_order_acquire)
        && snapshot->SpectralRevision < _minimumAudioSpectralRevision.load(std::memory_order_acquire))
	{
		reason = "Edited playback was superseded by Undo or stroke cancellation";
		return false;
	}
	const auto committed = _audioSpectralEdit.load(std::memory_order_acquire);
	if (committed && committed->Source->Generation == snapshot->Source().Generation
		&& (snapshot->EnvelopeRevision != committed->EnvelopeRevision
			|| (snapshot->Pcm->EnvelopeComposed
                ? snapshot->Pcm->ComposedEnvelope != committed->Envelope
                    || snapshot->SpectralRevision != committed->SpectralRevision
                    || snapshot->Pcm->ComposedSpectralRevision != committed->SpectralRevision
                    || snapshot->Pcm->ComposedEnvelopeRevision != committed->EnvelopeRevision
                : snapshot->Envelope != committed->Envelope)))
	{
		reason = "Edited playback must be composed with the newest broadband envelope";
		return false;
	}
	const auto current = _audioSpectralPlayback.load();
	if (current && current->Source().Generation == snapshot->Source().Generation)
	{
		if (snapshot->Revision <= current->Revision
			|| snapshot->SpectralRevision < current->SpectralRevision
			|| snapshot->EnvelopeRevision < current->EnvelopeRevision
			|| (snapshot->SpectralRevision == current->SpectralRevision && snapshot->Pcm != current->Pcm
                && (!snapshot->Pcm->EnvelopeComposed || snapshot->EnvelopeRevision == current->EnvelopeRevision))
			|| (snapshot->EnvelopeRevision == current->EnvelopeRevision
                && (snapshot->Pcm->EnvelopeComposed ? snapshot->Pcm->ComposedEnvelope : snapshot->Envelope)
                    != (current->Pcm->EnvelopeComposed ? current->Pcm->ComposedEnvelope : current->Envelope)))
		{
			reason = "Edited playback would replace a newer or inconsistent component revision";
			return false;
		}
	}
	// A writer can invalidate after this check. The callback independently checks
	// generation before using/adopting a snapshot; punch never takes this mutex.
	if (!IsCurrentAudioSpectralSource(snapshot->Source()))
	{
		reason = "Audio source changed before edited playback publication";
		return false;
	}
	_audioSpectralPlayback.store(std::move(snapshot));
	_changesMade = true;
	return true;
}

void Loop::InvalidateAudioSpectralResultsBefore(std::uint64_t spectralRevision,
    std::uint64_t expectedGeneration)
{
	// Serialize against the final publication check, without involving the audio
	// callback. Callback adoption only reads the scalar floor.
	std::scoped_lock lock(_audioSourcePublicationMutex);
	if (expectedGeneration != AudioSourceGeneration()) return;
    const auto floor = expectedGeneration == _minimumAudioSpectralGeneration.load(std::memory_order_relaxed)
        ? _minimumAudioSpectralRevision.load(std::memory_order_relaxed) : 0u;
	_minimumAudioSpectralRevision.store((std::max)(floor, spectralRevision), std::memory_order_release);
    // Tag protects a new source if punch invalidates during this method.
    _minimumAudioSpectralGeneration.store(expectedGeneration, std::memory_order_release);
}

std::shared_ptr<const spectral::AudioSpectralPlaybackSnapshot> Loop::AudioSpectralPlayback() const
{
	const auto snapshot = _audioSpectralPlayback.load();
	return snapshot && IsCurrentAudioSpectralSource(snapshot->Source()) ? snapshot : nullptr;
}

std::vector<float> Loop::ExportAudioSpectralSamples(
	const spectral::AudioSpectralPlaybackSnapshot& snapshot, bool retainPhysicalTail)
{
	const auto& layout = snapshot.Source().Layout;
	const auto length = retainPhysicalTail ? layout.PhysicalLength : layout.LogicalLength;
	std::vector<float> output(length);
	for (unsigned long index = 0ul; index < length; ++index)
		output[index] = snapshot.SampleAt(layout.BodyOffset + index);
	return output;
}

void Loop::_AdvanceAudioSpectralPlayback(unsigned int numSamps) noexcept
{
	const auto generation = _audioSourceGeneration.load(std::memory_order_acquire);
	if (_activeAudioSpectral && _activeAudioSpectral->Source().Generation != generation)
	{
		_previousAudioSpectral.reset();
		_activeAudioSpectral.reset();
		_audioSpectralTransitionSamps = 0u;
		_audioSpectralTransitionProgress = 0u;
		_appliedAudioSpectralRevision.store(0u, std::memory_order_release);
	}
	if (_audioSpectralTransitionSamps != 0u)
	{
		const auto remaining = _audioSpectralTransitionSamps - _audioSpectralTransitionProgress;
		_audioSpectralTransitionProgress += (std::min)(remaining, numSamps);
		if (_audioSpectralTransitionProgress < _audioSpectralTransitionSamps) return;
		_previousAudioSpectral.reset();
		_audioSpectralTransitionSamps = 0u;
	}
	auto pending = _audioSpectralPlayback.borrow();
	if (!pending || pending->Source().Generation != generation || !CanEditAudioSource()
		|| (pending->Source().Generation == _minimumAudioSpectralGeneration.load(std::memory_order_acquire)
            && pending->SpectralRevision < _minimumAudioSpectralRevision.load(std::memory_order_acquire))
		|| (_activeAudioSpectral && pending->Revision <= _activeAudioSpectral->Revision))
		return;
	_previousAudioSpectral = std::move(_activeAudioSpectral);
	_activeAudioSpectral = std::move(pending);
	_audioSpectralTransitionProgress = 0u;
	_audioSpectralTransitionSamps = static_cast<unsigned int>(std::clamp(
		static_cast<double>(_activeAudioSpectral->Source().Layout.SampleRate)
			* spectral::AudioSpectralConfig::PlaybackTransitionSeconds,
		1.0, static_cast<double>(spectral::AudioSpectralConfig::MaxPlaybackTransitionSamps)));
	_appliedAudioSpectralRevision.store(_activeAudioSpectral->Revision, std::memory_order_release);
	// Releasing a Lease only clears its scalar hazard. Publisher retains ownership
	// and CommitChanges collects off audio after the transition/read has ended.
}

float Loop::_ReadAudioSpectralSample(unsigned long storageIndex,
	unsigned int blockOffset, std::uint64_t generation) const noexcept
{
	const auto original = [this, storageIndex]() noexcept
	{
		return _bufferBank[storageIndex]; // ReadBlock already bounds the body index.
	};
	if (!_activeAudioSpectral || _activeAudioSpectral->Source().Generation != generation)
		return original();
	const auto current = _activeAudioSpectral->SampleAt(storageIndex);
	if (_audioSpectralTransitionSamps == 0u
		|| (_activeAudioSpectral->UsesOriginal()
			&& (!_previousAudioSpectral || _previousAudioSpectral->UsesOriginal()))) return current;
	const auto position = static_cast<std::uint64_t>(_audioSpectralTransitionProgress) + blockOffset;
	if (position >= _audioSpectralTransitionSamps) return current;
	const auto previous = _previousAudioSpectral ? _previousAudioSpectral->SampleAt(storageIndex) : original();
	const auto weight = static_cast<double>(position) / _audioSpectralTransitionSamps;
	// Double convex sum avoids float subtraction overflow for opposite large peaks.
	return static_cast<float>(static_cast<double>(previous) * (1.0 - weight) + current * weight);
}

bool Loop::IsCurrentAudioSpectralSource(const spectral::AudioSpectralSource& source) const noexcept
{
	return source.Identity.get() == _audioSourceIdentity.get()
		&& source.Generation == _audioSourceGeneration.load(std::memory_order_acquire)
		&& CanEditAudioSource() && source.Layout == _AudioSpectralSourceLayout();
}

void Loop::_CopyAudioSpectralSourceAtAudioBoundary() noexcept
{
	auto request = _audioSourceCapture.borrow();
	if (!request) return; // A bounded borrow failure simply retries on the next block.
	request->CopyAtAudioBoundary(_bufferBank, _AudioSpectralSourceLayout(),
		_audioSourceGeneration.load(std::memory_order_acquire), CanEditAudioSource());
}

bool Loop::CanRefreshVisualModel() const noexcept
{
	switch (_playState.load(std::memory_order_relaxed))
	{
	case STATE_INACTIVE:
	case STATE_RECORDING:
	case STATE_PLAYINGRECORDING:
	case STATE_PLAYING:
	case STATE_OVERDUBBING:
	case STATE_PUNCHEDIN:
	case STATE_OVERDUBBINGRECORDING:
		return true;
	default:
		return false;
	}
}


void Loop::ConfigureTriggerCapture(std::int64_t begin, bool requireAdc, bool requireBounce, bool requirePunchAdc) noexcept
{
	_triggerCaptureBegin = begin;
	_triggerCaptureCoverageValid.store(true, std::memory_order_relaxed);
	_triggerCaptureRequiresAdc = requireAdc;
	_triggerCaptureRequiresBounce = requireBounce;
	_triggerPunchRequiresAdc = requirePunchAdc;
	_triggerAdcThrough.store(begin, std::memory_order_relaxed);
	_triggerBounceThrough.store(begin, std::memory_order_relaxed);
	_triggerCaptureEnd.store((std::numeric_limits<std::int64_t>::max)(), std::memory_order_relaxed);
	_triggerCaptureNormalized.store(false, std::memory_order_relaxed);
	_triggerCapture = true;
}

void Loop::SetTriggerCaptureCursor(std::int64_t cursor) noexcept
{
	_triggerCaptureCursor = cursor;
	_triggerAdcBlockThrough = (std::max)(cursor, _triggerCaptureBegin);
}

bool Loop::SetTriggerCaptureLength(unsigned long length) noexcept
{
	const auto end = _triggerCaptureBegin + length;
	if (_triggerCaptureBegin < 0 || _triggerCaptureBegin > constants::MaxLoopFadeSamps
		|| constants::MaxLoopFadeSamps - _triggerCaptureBegin >= length
		|| end > constants::MaxLoopBufferSize) return false;
	_triggerCaptureEnd.store(end, std::memory_order_release);
	return true;
}

bool Loop::CanUseTemporaryPlayback(unsigned long bodyPhase, unsigned int count) const noexcept
{
    const auto state = _playState.load(std::memory_order_acquire);
    if (!_triggerCapture || !_triggerCaptureRequiresAdc || _triggerCaptureRequiresBounce
        || !TriggerCaptureCoverageValid() || (state != STATE_PLAYINGRECORDING && state != STATE_PLAYING)
        || _isPunchInActive.load(std::memory_order_acquire) || _activeAudioSpectral || _previousAudioSpectral)
        return false;

    const auto length = _loopLength.load(std::memory_order_relaxed);
    const auto fade = _loopParams.FadeSamps;
    if (length == 0u || bodyPhase >= length || fade > length) return false;
    const auto requestEnd = static_cast<std::uint64_t>(bodyPhase) + (std::min)(count, constants::MaxBlockSize);
    // Legacy seam addressing is only proved equivalent through one wrap.
    if (requestEnd > 2u * static_cast<std::uint64_t>(length)) return false;
    const auto selectedPrefix = constants::MaxLoopFadeSamps - _triggerCaptureBegin;
    if (fade <= selectedPrefix) return true;

    // Only the initial part of the existing seam reads below capture begin,
    // where prefix coverage is unpublished. Reject those requests, not the
    // whole loop. After ReadBlock wraps, its second seam leg is the body itself.
    const auto unprovedBegin = static_cast<std::uint64_t>(length) - fade;
    const auto unprovedEnd = static_cast<std::uint64_t>(length) - selectedPrefix;
    return bodyPhase >= unprovedEnd
        || requestEnd <= unprovedBegin;
}

bool Loop::TriggerCaptureComplete() const noexcept
{
	if (!_triggerCapture) return true;
	if (!_triggerCaptureCoverageValid.load(std::memory_order_acquire)) return false;
	const auto end = _triggerCaptureEnd.load(std::memory_order_acquire);
	return (!_triggerCaptureRequiresAdc || _triggerAdcThrough.load(std::memory_order_acquire) >= end)
		&& (!_triggerCaptureRequiresBounce || _triggerBounceThrough.load(std::memory_order_acquire) >= end);
}

bool Loop::TriggerCaptureReady(unsigned long phase, unsigned int count) const noexcept
{
	if (!_triggerCapture) return true;
	const auto length = _loopLength.load(std::memory_order_relaxed);
	if (length == 0) return false;
	const auto through = (std::min)(
		_triggerCaptureRequiresAdc ? _triggerAdcThrough.load(std::memory_order_acquire) : (std::numeric_limits<std::int64_t>::max)(),
		_triggerCaptureRequiresBounce ? _triggerBounceThrough.load(std::memory_order_acquire) : (std::numeric_limits<std::int64_t>::max)());
	for (unsigned int i = 0u; i < count; ++i)
	{
		const auto index = constants::MaxLoopFadeSamps + (phase + i) % length;
		if (index >= through && !_triggerCaptureNormalized.load(std::memory_order_acquire)) return false;
	}
	return true;
}

void Loop::NormalizeTriggerCapture()
{
	// Completion stopped destination writes at the exclusive physical-pass end.
	// Reads refuse the unprepared tail; publishing readiness is the final operation.
	if (!_triggerCapture || !TriggerCaptureComplete()) return;
	const auto length = _loopLength.load(std::memory_order_relaxed);
	const auto bodyEnd = constants::MaxLoopFadeSamps + length;
	_bufferBank.Resize((std::max)(_bufferBank.Length(), bodyEnd));
	for (auto i = _triggerCaptureBegin; i < constants::MaxLoopFadeSamps; ++i)
		_bufferBank[static_cast<unsigned long>(i + length)] = _bufferBank[static_cast<unsigned long>(i)];
	_triggerCaptureNormalized.store(true, std::memory_order_release);
}

void Loop::EndRecording()
{
	auto playState = _playState.load(std::memory_order_relaxed);
	if ((STATE_PLAYINGRECORDING != playState) &&
		(STATE_OVERDUBBINGRECORDING != playState))
		return;
	
	NormalizeTriggerCapture();
	_playState.store(STATE_PLAYING, std::memory_order_release);
}

void Loop::Ditch()
{
	if (STATE_INACTIVE == _playState.load(std::memory_order_relaxed))
		return;

Reset();

_bufferBank.Resize(constants::MaxLoopFadeSamps);
}

bool Loop::Mute()
{
	auto isNewState = Tweakable::Mute();

	if (isNewState && _mixer)
		_mixer->Mute();

	return isNewState;
}

bool Loop::IsMuted() const
{
	return Tweakable::IsMuted() || _mixer->IsCaptureSourceMuted();
}

void Loop::SetGroupMutedAudio(bool muted) noexcept
{
	// Group recall never releases recording-owned replacement suppression.
	if (muted) Tweakable::Mute(); else Tweakable::UnMute();
	if (_mixer) { if (muted) _mixer->Mute(); else _mixer->UnMute(); }
}

void Loop::SetCaptureSourceMuteControl(
	const std::shared_ptr<const audio::CaptureSourceMuteControl>& control) noexcept
{
	_mixer->SetCaptureSourceMuteControl(control);
}

bool Loop::UnMute()
{
	auto isNewState = Tweakable::UnMute();
	const bool replacementChanged = _mixer && _mixer->AllowReplacementPlayback();

	if (isNewState && _mixer)
		_mixer->UnMute();

	return isNewState || replacementChanged;
}

void Loop::Overdub()
{
	if (STATE_INACTIVE != _playState.load(std::memory_order_relaxed))
		return;

	Reset();
	_bufferBank.Resize(constants::MaxLoopFadeSamps);

	_monitorBufferBank.Resize(constants::MaxLoopFadeSamps);
	_playState.store(STATE_OVERDUBBING, std::memory_order_release);
}

void Loop::PunchIn()
{
	auto playState = _playState.load(std::memory_order_relaxed);
	if ((STATE_OVERDUBBING != playState) &&
		(STATE_OVERDUBBINGRECORDING != playState) &&
		(STATE_PLAYING != playState))
		return;

	_InvalidateAudioSource();
	_isPunchInActive.store(true, std::memory_order_release);
	if (STATE_OVERDUBBING == playState)
		_playState.store(STATE_PUNCHEDIN, std::memory_order_release);
}

void Loop::PunchOut()
{
	auto playState = _playState.load(std::memory_order_relaxed);
	if (!_isPunchInActive.load(std::memory_order_relaxed) && (STATE_PUNCHEDIN != playState))
		return;

	_isPunchInActive.store(false, std::memory_order_release);
	if (STATE_PUNCHEDIN == playState)
		_playState.store(STATE_OVERDUBBING, std::memory_order_release);
}

double Loop::LoopIndexFrac() const noexcept
{
	auto playState = _playState.load(std::memory_order_relaxed);
	auto loopLength = _loopLength.load(std::memory_order_relaxed);
	auto isRecording = (STATE_RECORDING == playState) ||
		(STATE_OVERDUBBING == playState) ||
		(STATE_PUNCHEDIN == playState);
	auto index = isRecording ?
		_writeIndex.load(std::memory_order_relaxed) :
		_playIndex.load(std::memory_order_relaxed);

	if (!isRecording && index >= constants::MaxLoopFadeSamps)
		index -= constants::MaxLoopFadeSamps;

	if (0ul == loopLength)
		return 0.0;

	return 1.0 - std::max(0.0, std::min(1.0, ((double)(index % loopLength)) / ((double)loopLength)));
}

double Loop::CalcDrawRadius(unsigned long loopLength)
{
	auto minRadius = 50.0;
	auto maxRadius = 400.0;
	if (loopLength == 0ul)
		return minRadius;

	auto radius = 70.0 * log(loopLength) - 600;

	return std::clamp(radius, minRadius, maxRadius);
}

LoopModel::LoopModelState Loop::_GetLoopModelState(base::DrawPass pass, LoopPlayState state, bool isMuted)
{
	switch (pass)
	{
	case PASS_PICKER:
		return LoopModel::LoopModelState::STATE_PICKING;
	case PASS_HIGHLIGHT:
		return LoopModel::LoopModelState::STATE_HIGHLIGHTING;
	default:
		switch (state)
		{
		case STATE_PLAYING:
			return isMuted ?
				LoopModel::LoopModelState::STATE_MUTED :
				LoopModel::LoopModelState::STATE_PLAYING;
		default:
			return LoopModel::LoopModelState::STATE_RECORDING;
		}
	}
}

unsigned long Loop::_ModelDisplayLength(bool isRecording, unsigned long actualLoopLength) const
{
	return actualLoopLength;
}

unsigned long Loop::_LoopIndex() const
{
	auto playIndex = _playIndex.load(std::memory_order_relaxed);
	if (constants::MaxLoopFadeSamps > playIndex)
		return 0;

	return playIndex - constants::MaxLoopFadeSamps;
}

void Loop::_UpdateLoopModel()
{
	if (!_visualUpdatesEnabled)
		return;

	_ForceUpdateLoopModel();
}

void Loop::_ForceUpdateLoopModel()
{
	auto playState = _playState.load(std::memory_order_relaxed);
	auto isRecording = (STATE_RECORDING == playState) ||
		(STATE_OVERDUBBING == playState) ||
		(STATE_PUNCHEDIN == playState);
	auto actualLength = isRecording ? _writeIndex.load(std::memory_order_relaxed) : _loopLength.load(std::memory_order_relaxed);
	auto displayLength = _ModelDisplayLength(isRecording, actualLength);
	auto offset = isRecording ? 0ul : constants::MaxLoopFadeSamps;

	auto radius = (float)(CalcDrawRadius(displayLength) * _DrawRadiusScale());
	if (!isRecording)
		if (const auto edited = AudioSpectralPlayback())
		{
			_model->UpdateEditedModel(edited, radius);
			_vu->UpdateModel(radius);
			return;
		}
	auto& bufBank = isRecording ? _monitorBufferBank : _bufferBank;
	_ApplyLoopVisualModel(bufBank, actualLength, displayLength, offset, radius);
}

void Loop::_ApplyLoopVisualModel(const BufferBank& buffer,
	unsigned long actualLength,
	unsigned long displayLength,
	unsigned long offset,
	float radius)
{
	const auto allowUnchangedSkip = (STATE_PLAYING == _playState.load(std::memory_order_relaxed));
	_model->UpdateModel(buffer, actualLength, displayLength, offset, radius, allowUnchangedSkip);
	_vu->UpdateModel(radius);
}

void Loop::LoadVstPlugin(std::wstring path,
	std::vector<std::uint8_t> initialState)
{
	_pendingVstLoads.push_back({ std::move(path), std::move(initialState) });
	_changesMade = true;
}

bool Loop::LoadVstPluginSynchronously(const std::wstring& path,
	const std::vector<std::uint8_t>& initialState,
	bool bypass, const std::string& stateId)
{
	// Scene::FromFile calls this before Scene::InitAudio, so no callback can
	// retain the old chain while this non-RT construction is in progress.
	auto plugin = vst::MakePluginForPath(path);
	if (!plugin->PreInit(path)
		|| !plugin->Load(path, _sampleRate, _blockSize, 1u, vst::HostedLayoutMode::MonoFlexible))
		return false;

	plugin->SetStateId(stateId);
	if (!initialState.empty())
		plugin->SetState(initialState);
	plugin->SetBypassed(bypass);

	auto chain = _vstChain.load(std::memory_order_acquire);
	auto replacement = std::make_shared<vst::VstChain>();
	if (chain)
	{
		for (std::size_t index = 0u; index < chain->NumPlugins(); ++index)
			if (auto existing = chain->GetPlugin(index))
				replacement->AddPlugin(std::move(existing));
	}
	replacement->AddPlugin(std::move(plugin));
	_vstChain.store(std::move(replacement), std::memory_order_release);
	// Startup still has exclusive ownership; VstEntries cannot run until after
	// this object is published to the main loop.
	_vstPluginPaths.push_back(path);
	return true;
}

void Loop::UnloadVstPlugin(size_t index)
{
	_pendingVstUnloads.push_back(index);
	_changesMade = true;
}

bool Loop::ReconfigureVstFormat(float sampleRate, unsigned int blockSize)
{
	const auto chain = _vstChain.load();
	if (chain && !chain->ReconfigureAudioFormat(sampleRate, blockSize)) return false;
	return !_backVstChain || _backVstChain == chain || _backVstChain->ReconfigureAudioFormat(sampleRate, blockSize);
}

std::shared_ptr<vst::IVstPlugin> Loop::GetVstPlugin(size_t index) const
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	if (!chain)
		return nullptr;

	return chain->GetPlugin(index);
}

std::vector<JobAction> Loop::_CommitChanges()
{
	// Plugin latency/controller notifications must be pumped with closed editors.
	if (const auto chain = _vstChain.load(std::memory_order_acquire))
		chain->PollPendingChanges();

	// UI commit also receives publications after editor close and closed-Loop Undo.
	// Keep the acquired immutable value through overview upload preparation; a
	// source invalidation here must never make this path fall back to bank reads.
	if (_visualUpdatesEnabled)
		if (const auto playback = AudioSpectralPlayback())
		{
			const auto radius = static_cast<float>(CalcDrawRadius(playback->Source().Layout.LogicalLength) * _DrawRadiusScale());
			_model->UpdateEditedModel(playback, radius);
			_vu->UpdateModel(radius);
		}
	if (_CollectAudioSourceCaptures() != 0u)
		_changesMade = true;
	_vstChain.collect();

	// Swap in a new VST chain when the job thread has delivered one.
	if (_flipVstChain.exchange(false, std::memory_order_acquire))
	{
		_vstChain.store(_backVstChain, std::memory_order_release);
	}

	std::vector<JobAction> jobs;

	// Drain pending unload indices — emit one JOB_UNLOADVST per entry so
	// rapid back-to-back UnloadVstPlugin() calls are not silently dropped.
	auto pendingUnloads = std::move(_pendingVstUnloads);
	for (auto idx : pendingUnloads)
	{
		JobAction job;
		job.JobActionType = JobAction::JOB_UNLOADVST;
		job.SourceId = Id();
		job.VstIndex = idx;
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(job);
	}

	auto pendingLoads = std::move(_pendingVstLoads);
	for (auto& [path, initialState] : pendingLoads)
	{
		JobAction job;
		job.JobActionType = JobAction::JOB_LOADVST;
		job.SourceId = Id();
		job.VstPath = std::move(path);
		job.VstInitialState = std::move(initialState);
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(std::move(job));
	}

	GuiElement::_CommitChanges();

	return jobs;
}

ActionResult Loop::OnAction(JobAction action)
{
	switch (action.JobActionType)
	{
	case JobAction::JOB_LOADVST:
	{
		// Take a snapshot of the current live chain under _vstChainMutex so we
		// don't race with _CommitChanges() swapping it on the main thread.
		std::shared_ptr<vst::VstChain> chainSnapshot;
		chainSnapshot = _vstChain.load(std::memory_order_acquire);

		auto newChain = std::make_shared<vst::VstChain>();
		if (chainSnapshot)
		{
			for (size_t i = 0; i < chainSnapshot->NumPlugins(); ++i)
			{
				auto existing = chainSnapshot->GetPlugin(i);
				if (existing)
					newChain->AddPlugin(existing);
			}
		}

		auto plugin = action.PreInitPlugin
			? action.PreInitPlugin
			: vst::MakePluginForPath(action.VstPath);
		if (plugin->Load(action.VstPath, _sampleRate, _blockSize, 1u, vst::HostedLayoutMode::MonoFlexible))
		{
			// Restore saved state before adding to the chain.
			if (!action.VstInitialState.empty())
				plugin->SetState(action.VstInitialState);

			newChain->AddPlugin(plugin);
			{
				std::lock_guard<std::mutex> lock(_vstPathsMutex);
				_vstPluginPaths.push_back(action.VstPath);
			}
			_backVstChain = std::move(newChain);
			_flipVstChain.store(true, std::memory_order_release);
			_changesMade = true;
		}
		else if (action.PreInitPlugin)
		{
			// Load failed but plugin was pre-initialised on the UI thread;
			// destroying it here (job thread) would violate VST3 threading.
			vst::QueueForUiThreadDestroy(std::move(plugin));
		}

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	case JobAction::JOB_UNLOADVST:
	{
		// Take a snapshot of the current live chain under _vstChainMutex.
		std::shared_ptr<vst::VstChain> chainSnapshot;
		chainSnapshot = _vstChain.load(std::memory_order_acquire);

		auto newChain = std::make_shared<vst::VstChain>();
		const auto removeIndex = action.VstIndex;
		if (chainSnapshot)
		{
			for (size_t i = 0; i < chainSnapshot->NumPlugins(); ++i)
			{
				if (i == removeIndex)
					continue;
				auto existing = chainSnapshot->GetPlugin(i);
				if (existing)
					newChain->AddPlugin(existing);
			}
		}

		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			if (removeIndex < _vstPluginPaths.size())
				_vstPluginPaths.erase(_vstPluginPaths.begin() + static_cast<std::ptrdiff_t>(removeIndex));
		}

		_backVstChain = std::move(newChain);
		_flipVstChain.store(true, std::memory_order_release);
		_changesMade = true;

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	default:
		break;
	}

	return ActionResult::NoAction();
}
