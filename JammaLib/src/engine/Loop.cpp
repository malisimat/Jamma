#include "Loop.h"
#include "../vst/RackState.h"
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


void engine::Loop::SetPlaybackRateBlockAtAudioBoundary(const audio::PlaybackRateBlock& block,
    double recordingRate) noexcept
{
    if (!std::isfinite(recordingRate) || recordingRate <= 0.0) return;
    _playbackRateBlock = block;
    _recordingPlaybackRate = recordingRate;
    if (LoopLength()) { _InitialisePlaybackOffset(); _PreparePlaybackOffsetTarget(); }
}

float engine::Loop::_ReadSourceBodySample(double sourcePosition, unsigned int frame) const noexcept
{
    const auto length = LoopLength();
    if (!length) return 0.0f;
    auto body = std::fmod(sourcePosition, static_cast<double>(length));
    if (body < 0.0) body += length;
    const auto first = static_cast<unsigned long>(body);
    const auto fraction = body - first;
    const auto read = [&](unsigned long phase) noexcept
    {
        if (!TriggerCaptureReady(phase, 1u)) return 0.0f;
        const auto storage = constants::MaxLoopFadeSamps + phase;
        if (storage >= _bufferBank.Length()) return 0.0f;
        const auto sample = _ReadSpectralSample(storage, frame, AudioSourceGeneration());
        if (_loopParams.FadeSamps && phase >= length - _loopParams.FadeSamps)
        {
            const auto fade = phase - (length - _loopParams.FadeSamps);
            const auto prefix = constants::MaxLoopFadeSamps + fade - _loopParams.FadeSamps;
            return _hanning->Mix(_ReadSpectralSample(prefix, frame, AudioSourceGeneration()), sample, fade);
        }
        return sample;
    };
    const auto next = first + 1 == length ? 0ul : first + 1;
    return static_cast<float>(read(first) * (1.0 - fraction) + read(next) * fraction);
}

void engine::Loop::SetCaptureRateHistoryAtAudioBoundary(const audio::PlaybackRateHistory* history,
    std::uint64_t physicalInputStart, const audio::SourcePosition& sourceOrigin, double baseline) noexcept
{
    _captureRateHistory = history;
    _capturePhysicalInputStart = physicalInputStart;
    _captureSourceOrigin = sourceOrigin;
    _captureRecordingRate = baseline;
}

void engine::Loop::SetCaptureInputHistoryAtAudioBoundary(const audio::InputHistoryView& history,
    unsigned int channel) noexcept
{
    _captureInputHistory = history;
    _captureInputChannel = channel;
    FlushPendingCaptureAtAudioBoundary();
}

void engine::Loop::FlushPendingCaptureAtAudioBoundary() noexcept
{
    if (!_triggerCapture || !_captureRateHistory || !TriggerCaptureCoverageValid()) return;
    _FlushMappedCapture(false);
    // The source pointer is borrowed only while ProcessActiveBounce pins it.
    if (_captureSource) _FlushMappedCapture(true);
}

void engine::Loop::_QueueMappedCapture(const base::AudioWriteRequest& request, int writeOffset,
    bool residual) noexcept
{
    if (!request.numSamps) return;
    const auto signedStart = static_cast<std::int64_t>(_capturePhysicalInputStart) + writeOffset;
    if (signedStart < 0 || request.numSamps > constants::MaxBlockSize)
    { _triggerCaptureCoverageValid.store(false, std::memory_order_release); return; }
    const auto start = static_cast<std::uint64_t>(signedStart);
    auto& capture = _captureQueues[residual ? 1u : 0u];
    if (residual)
    {
        if (_captureResidualStart && start != _captureResidualEnd)
        { _triggerCaptureCoverageValid.store(false, std::memory_order_release); return; }
        if (!_captureResidualStart) _captureResidualStart = start;
        const auto end = start + request.numSamps;
        if (capture.Count)
        {
            const auto oldest = capture.Regions[capture.First].Cursor;
            if (end > oldest + _captureResidualSamples.size())
            { _triggerCaptureCoverageValid.store(false, std::memory_order_release); return; }
        }
        for (unsigned int frame = 0; frame < request.numSamps; ++frame)
            _captureResidualSamples[(start + frame) % _captureResidualSamples.size()] = request.samples[frame * request.stride];
        _captureResidualEnd = end;
    }
    if (capture.Count == capture.Regions.size())
    { _triggerCaptureCoverageValid.store(false, std::memory_order_release); return; }
    auto& region = capture.Regions[(capture.First + capture.Count++) % capture.Regions.size()];
    region = {start, start + request.numSamps, request.fadeNew, request.fadeCurrent,
        residual ? _captureSource : nullptr, residual && _captureSource ? _captureSource->AudioSourceGeneration() : 0u,
        _captureSourceBodyOrigin};
    FlushPendingCaptureAtAudioBoundary();
}

void engine::Loop::_FlushMappedCapture(bool residual) noexcept
{
    auto& capture = _captureQueues[residual ? 1u : 0u];
    auto& published = residual ? _triggerBounceThrough : _triggerAdcThrough;
    auto through = published.load(std::memory_order_relaxed);
    const auto captureEnd = _triggerCaptureEnd.load(std::memory_order_acquire);
    bool wrote = false;
    const auto publish = [&]() noexcept
    {
        if (wrote) _InvalidateAudioSource();
        published.store(through, std::memory_order_release);
        if (!residual) _triggerAdcBlockThrough = through;
    };
    unsigned int work = 0u;
    while (capture.Count && TriggerCaptureCoverageValid())
    {
        auto& region = capture.Regions[capture.First];
        if (through >= captureEnd) { capture = {}; break; }
        if (residual && (!region.SourceIdentity || region.SourceIdentity != _captureSource
            || region.SourceGeneration != _captureSource->AudioSourceGeneration()))
        { _triggerCaptureCoverageValid.store(false, std::memory_order_release); break; }
        const auto point = _captureRateHistory->Query(region.Cursor);
        if (!point) { _triggerCaptureCoverageValid.store(false, std::memory_order_release); break; }
        auto position = point->Coordinate.Difference(_captureSourceOrigin) / _captureRecordingRate + constants::MaxLoopFadeSamps;
        while (region.Cursor < region.End)
        {
            const auto block = _captureRateHistory->DescribeAt(region.Cursor);
            if (!block) { _triggerCaptureCoverageValid.store(false, std::memory_order_release); publish(); return; }
            auto rate = block->Cursor();
            const bool constantRate = block->IsConstant();
            const auto constantIncrement = block->InitialRate * block->SourceScale / _captureRecordingRate;
            const auto count = (std::min)(static_cast<std::uint64_t>(block->Frames), region.End - region.Cursor);
            for (std::uint64_t frame = 0; frame < count; ++frame)
            {
                if (++work > constants::MaxBlockSize + CaptureInterpolationSamples)
                { _triggerCaptureCoverageValid.store(false, std::memory_order_release); publish(); return; }
                const auto increment = constantRate ? constantIncrement : rate.Next() / _captureRecordingRate;
                if (!(increment >= MinimumSourceRate) || increment > MaximumSourceRate)
                { _triggerCaptureCoverageValid.store(false, std::memory_order_release); publish(); return; }
                const auto next = position + increment;
                auto first = (std::max)({static_cast<std::int64_t>(std::ceil(position)), through, _triggerCaptureBegin});
                const auto end = (std::min)(static_cast<std::int64_t>(std::ceil(next)), captureEnd);
                // ADC gaps are intentional when punching over an existing source.
                if (!residual && !_triggerCaptureRequiresAdc && first > through) through = first;
                for (auto index = first; index < end; ++index)
                {
                    if (index != through || index < 0 || index >= _bufferBank.Capacity())
                    { _triggerCaptureCoverageValid.store(false, std::memory_order_release); publish(); return; }
                    if (residual && _captureQueues[0].Count)
                    {
                        const auto& adc = _captureQueues[0].Regions[_captureQueues[0].First];
                        const auto pending = _captureRateHistory->Query(adc.Cursor);
                        if (!pending) { _triggerCaptureCoverageValid.store(false, std::memory_order_release); publish(); return; }
                        const auto pendingSource = pending->Coordinate.Difference(_captureSourceOrigin) / _captureRecordingRate + constants::MaxLoopFadeSamps;
                        if (index >= std::ceil(pendingSource)) { publish(); return; }
                    }
                    const auto fraction = (static_cast<double>(index) - position) / increment;
                    double sample = 0.0;
                    for (unsigned int neighbor = 0; neighbor < CaptureInterpolationSamples; ++neighbor)
                    {
                        const auto weight = neighbor == 0u ? 1.0 - fraction : fraction;
                        // Exact integer samples need no right-hand lookahead.
                        if (weight == 0.0) continue;
                        const auto input = static_cast<std::int64_t>(region.Cursor) + neighbor;
                        float value = 0.0f;
                        if (residual)
                        {
                            // The residual starts at this capture boundary.
                            // Original source material is composed separately below.
                            if (input >= static_cast<std::int64_t>(*_captureResidualStart))
                            {
                                if (input >= static_cast<std::int64_t>(_captureResidualEnd)) { publish(); return; }
                                if (input < 0 || static_cast<std::uint64_t>(input) + _captureResidualSamples.size() < _captureResidualEnd)
                                { _triggerCaptureCoverageValid.store(false, std::memory_order_release); publish(); return; }
                                value = _captureResidualSamples[static_cast<std::uint64_t>(input) % _captureResidualSamples.size()];
                            }
                        }
                        else if (input < 0 || !_captureInputHistory.Read(_captureInputChannel, static_cast<std::uint32_t>(input), value))
                        {
                            const auto frontier = static_cast<std::int64_t>(_captureInputHistory.IngressHead) - _captureInputHistory.InputLatency;
                            if (input < frontier) _triggerCaptureCoverageValid.store(false, std::memory_order_release);
                            publish(); return;
                        }
                        sample += weight * value;
                    }
                    if (residual) sample += _captureSource->_ReadSourceBodySample(region.SourceBodyOrigin
                        + static_cast<double>(index) - constants::MaxLoopFadeSamps, static_cast<unsigned int>(frame));
                    auto& destination = _bufferBank[static_cast<unsigned long>(index)];
                    destination = region.FadeNew * static_cast<float>(sample) + region.FadeCurrent * destination;
                    ++through;
                    wrote = true;
                }
                position = next;
                // Retry an unavailable right-hand neighbor at this same
                // physical interval; published source samples are not rewritten.
                ++region.Cursor;
            }
        }
        capture.First = (capture.First + 1u) % capture.Regions.size();
        --capture.Count;
    }
    publish();
}

void engine::Loop::_WriteRateMappedCapture(const base::AudioWriteRequest& request, int writeOffset) noexcept
{
    if (!request.numSamps || !request.samples) return;
    if (request.source == AUDIOSOURCE_ADC || request.source == AUDIOSOURCE_BOUNCE)
    {
        _QueueMappedCapture(request, writeOffset, request.source == AUDIOSOURCE_BOUNCE);
        return;
    }
    const auto physicalSigned = static_cast<std::int64_t>(_capturePhysicalInputStart) + writeOffset;
    if (physicalSigned < 0 || !std::isfinite(_captureRecordingRate) || _captureRecordingRate <= 0.0)
    {
        _triggerCaptureCoverageValid.store(false, std::memory_order_release);
        return;
    }
    auto physical = static_cast<std::uint64_t>(physicalSigned);
    const auto origin = _captureRateHistory->Query(physical);
    if (!origin)
    {
        _triggerCaptureCoverageValid.store(false, std::memory_order_release);
        return;
    }
    auto position = origin->Coordinate.Difference(_captureSourceOrigin) / _captureRecordingRate + constants::MaxLoopFadeSamps;
    const auto slot = request.source == AUDIOSOURCE_ADC ? 0u : request.source == AUDIOSOURCE_MONITOR ? 1u : 2u;
    auto& previous = _captureInterpolation[slot];
    auto& bank = request.source == AUDIOSOURCE_MONITOR ? _monitorBufferBank : _bufferBank;
    const auto captureEnd = _triggerCaptureEnd.load(std::memory_order_acquire);
    auto publishedThrough = slot == 0u ? _triggerAdcThrough.load(std::memory_order_relaxed)
        : _triggerBounceThrough.load(std::memory_order_relaxed);
    if (request.source != AUDIOSOURCE_MONITOR) _InvalidateAudioSource();
    unsigned int frame = 0u;
    while (frame < request.numSamps)
    {
        const auto block = _captureRateHistory->DescribeAt(physical);
        if (!block)
        {
            _triggerCaptureCoverageValid.store(false, std::memory_order_release);
            return;
        }
        auto cursor = block->Cursor();
        const auto count = (std::min)(block->Frames, request.numSamps - frame);
        for (unsigned int offset = 0u; offset < count; ++offset, ++frame, ++physical)
        {
            const auto sample = request.samples[frame * request.stride];
            if (previous.Valid && previous.Frame + 1u != physical) previous.Valid = false;
            const auto first = previous.Valid ? static_cast<std::int64_t>(std::floor(previous.Position)) + 1
                : static_cast<std::int64_t>(std::ceil(position));
            const auto last = static_cast<std::int64_t>(std::floor(position));
            if (last - first > static_cast<std::int64_t>(MaximumSourceRate))
            {
                _triggerCaptureCoverageValid.store(false, std::memory_order_release);
                return;
            }
            for (auto index = first; index <= last; ++index)
            {
                if (index < _triggerCaptureBegin || index < 0 || index >= captureEnd) continue;
                if (index >= bank.Capacity())
                {
                    _triggerCaptureCoverageValid.store(false, std::memory_order_release);
                    return;
                }
                const auto fraction = previous.Valid && position != previous.Position
                    ? (index - previous.Position) / (position - previous.Position) : 1.0;
                auto value = static_cast<float>(previous.Valid
                    ? previous.Sample + (sample - previous.Sample) * fraction : sample);
                if (slot == 2u && _captureSource)
                    value += _captureSource->_ReadSourceBodySample(_captureSourceBodyOrigin
                        + static_cast<double>(index) - constants::MaxLoopFadeSamps, frame);
                auto& destination = bank[static_cast<unsigned long>(index)];
                destination = request.fadeNew * value + request.fadeCurrent * destination;
                if (index == publishedThrough) ++publishedThrough;
                else if (index > publishedThrough && slot != 1u)
                    _triggerCaptureCoverageValid.store(false, std::memory_order_release);
            }
            previous = {true, physical, position, sample};
            if (slot == 1u) _lastPeak = (std::max)(_lastPeak, std::abs(sample));
            const auto increment = cursor.Next() / _captureRecordingRate;
            if (!(increment > 0.0) || increment > MaximumSourceRate)
            {
                _triggerCaptureCoverageValid.store(false, std::memory_order_release);
                return;
            }
            position += increment;
        }
    }
    if (slot == 0u)
    {
        _triggerAdcThrough.store(publishedThrough, std::memory_order_release);
        _triggerAdcBlockThrough = publishedThrough;
    }
    else if (slot == 2u) _triggerBounceThrough.store(publishedThrough, std::memory_order_release);
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
	_playbackOffsetDamping = 1.0 / -std::expm1(-1.0 / (constants::DefaultSampleRate * 0.010));
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
	loop->SetPlaybackOffsetSamps(loopStruct.PlaybackOffsetSamps);
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

void Loop::SetSpectralModel(std::shared_ptr<graphics::SpectralModel> model)
{
	if (_spectralModel == model) return;
	if (_spectralModel)
		_children.erase(std::remove(_children.begin(), _children.end(), _spectralModel), _children.end());
	_spectralModel = std::move(model);
	if (_spectralModel) _children.push_back(_spectralModel);
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

	if (!isRecording) index = PlaybackBodyIndex();
	auto frac = loopLength == 0 ? 0.0 : 1.0 - std::max(0.0, std::min(1.0, ((double)(index % loopLength)) / ((double)loopLength)));
	_model->SetLoopIndexFrac(frac);
	_model->SetLoopState(_GetLoopModelState(pass, playState, IsMuted()));
	_model->SetMuted(IsMuted());

	_modelScreenPos = glCtx.ProjectScreen(pos);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	if (PASS_SCENE == pass)
		_vu->Draw3d(ctx, 1, pass);

	const auto spectralVisible = _spectralModel && _spectralModel->IsAvailable();
	_model->SetSpectralReveal(spectralVisible ? _spectralMorph : 0.0f);
	const auto visualHeightScale = static_cast<float>(_mixer->UnmutedLevel()) * _masterVisualScale;
	_model->SetWaveformColorScale(visualHeightScale);
	glCtx.PushMvp(glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, pass == PASS_PICKER ? 1.0f : visualHeightScale, 1.0f)));
	
	GLboolean depthWrites = GL_TRUE;
	const auto fadingWaveform = PASS_SCENE == pass && spectralVisible && _spectralMorph > 0.0f;
	if (fadingWaveform)
	{
		glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrites);
		glDepthMask(GL_FALSE);
	}
	_model->Draw3d(ctx, 1, pass);
	if (fadingWaveform) glDepthMask(depthWrites);
	
	glCtx.PopMvp();
	if (_spectralModel && _spectralMorph > 0.0f)
	{
		_spectralModel->SetRadius(static_cast<float>(CalcDrawRadius(loopLength) * _DrawRadiusScale()));
		_spectralModel->SetPlayFraction(loopLength == 0ul ? 0.0
			: static_cast<double>(PlaybackBodyIndex()) / loopLength);
		_spectralModel->Draw3d(ctx, numInstances, pass);
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

	if (_triggerCapture && _captureRateHistory)
	{
		_WriteRateMappedCapture(request, writeOffset);
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
    if (_triggerCapture && request.numSamps)
    {
        const auto slot = request.source == AUDIOSOURCE_ADC ? 0u : request.source == AUDIOSOURCE_MONITOR ? 1u : 2u;
        const auto last = request.numSamps - 1u;
        const auto physical = static_cast<std::int64_t>(_capturePhysicalInputStart) + writeOffset + last;
        if (physical >= 0)
            _captureInterpolation[slot] = {true, static_cast<std::uint64_t>(physical),
                static_cast<double>(writeIndex + writeOffset + last), request.samples[last * request.stride]};
    }

}

void Loop::EndWrite(unsigned int numSamps,
	bool updateIndex)
{
	FlushPendingCaptureAtAudioBoundary();
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

	if (_triggerCapture && _captureRateHistory)
	{
		// Causal interpolation publishes only covered integer source samples.
		// The next device frame supplies the right-hand neighbor at the frontier.
		const auto end = _captureRateHistory->Query(_capturePhysicalInputStart + numSamps);
		if (!end) { _triggerCaptureCoverageValid.store(false, std::memory_order_release); return; }
		_triggerCaptureCursor = static_cast<std::int64_t>(std::floor(end->Coordinate.Difference(_captureSourceOrigin) / _captureRecordingRate + constants::MaxLoopFadeSamps));
		const auto ready = (std::min)(
			_triggerCaptureRequiresAdc ? _triggerAdcThrough.load(std::memory_order_acquire) : _triggerCaptureCursor,
			_triggerCaptureRequiresBounce ? _triggerBounceThrough.load(std::memory_order_acquire) : _triggerCaptureCursor);
		const auto length = static_cast<unsigned long>((std::clamp)(ready, std::int64_t{0}, static_cast<std::int64_t>(_bufferBank.Capacity())));
		_writeIndex.store(length, std::memory_order_relaxed);
		_bufferBank.SetLength(length);
		_monitorBufferBank.SetLength(length);
		return;
	}
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

	_InitialisePlaybackOffset(); // Cold latch once; repeated consumers share this block head.
	const auto sourceGeneration = AudioSourceGeneration();
	auto peak = 0.0f;
	auto bufSize = loopLength + constants::MaxLoopFadeSamps;

	// _playIndex is always within range:
	// [constants::MaxLoopFadeSamps : _loopLength + constants::MaxLoopFadeSamps - 1)
	// index should apply the offset and then also stay in the same range
	const auto targetOffset = TotalPlaybackOffsetSamps();
	const bool exactTargetOffset = _playbackOffsetRate == 0.0 && _playbackOffsetStart == targetOffset;
	const auto playbackOffset = exactTargetOffset ? static_cast<std::int64_t>(targetOffset)
		: static_cast<std::int64_t>(std::llround(_playbackOffsetStart));
	// Normalize on the fade-free body ruler. Storage-domain wrapping can land
	// in the prefix when a signed offset crosses several very short loops.
	const auto readOffset = playbackOffset - static_cast<std::int64_t>(sampOffset);
	auto index = constants::MaxLoopFadeSamps + OffsetBodyIndex(BodyPlayIndex(), loopLength, readOffset);

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
        if (!provisional) return _ReadSpectralSample(storage, frame, sourceGeneration);
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

	const bool fractionalSeek = !_playbackRateBlock.IsConstant()
        || _playbackRateBlock.InitialRate * _playbackRateBlock.SourceScale != _recordingPlaybackRate || _playbackFraction != 0.0
        || _playbackOffsetRate != 0.0
        || (!exactTargetOffset && _playbackOffsetStart != std::round(_playbackOffsetStart));
    if (fractionalSeek)
    {
        // A completed/normalized body owns every adjacent and seam-prefix sample.
        // In-flight captures retain their ordinary integer read path until ready.
        if (_triggerCapture && !_triggerCaptureNormalized.load(std::memory_order_acquire)) return 0u;
        const auto sampleAtBody = [&](unsigned long body, unsigned int frame) noexcept
        {
            const auto storage = constants::MaxLoopFadeSamps + body;
            if (storage >= bufBankSize) return 0.0f;
            const auto sample = readSource(storage, frame);
            if (_loopParams.FadeSamps && storage >= bufSize - _loopParams.FadeSamps)
            {
                const auto fadeIndex = storage - (bufSize - _loopParams.FadeSamps);
                const auto prefix = constants::MaxLoopFadeSamps + fadeIndex - _loopParams.FadeSamps;
                return _hanning->Mix(readSource(prefix, frame), sample, fadeIndex);
            }
            return sample;
        };
        const auto count = (std::min)(numSamps, constants::MaxBlockSize);
        auto body = std::fmod(static_cast<double>(BodyPlayIndex()) + _playbackFraction + sampOffset - _playbackOffsetStart,
            static_cast<double>(loopLength));
        if (body < 0.0) body += loopLength;
        auto rate = _playbackRateBlock.Cursor();
        auto appliedOffset = _playbackOffsetStart;
        const bool constantRate = _playbackRateBlock.IsConstant();
        const bool movingOffset = _playbackOffsetStart != _playbackOffsetEnd;
        const auto constantIncrement = _playbackRateBlock.InitialRate * _playbackRateBlock.SourceScale / _recordingPlaybackRate;
        for (auto frame = 0u; frame < count; ++frame)
        {
            auto increment = constantRate ? constantIncrement : rate.Next() / _recordingPlaybackRate;
            if (movingOffset)
            {
                const auto baseIncrement = constantRate ? constantIncrement : rate.BaseIncrement() / _recordingPlaybackRate;
                const auto correction = constantRate ? 0.0 : rate.Correction() / _recordingPlaybackRate;
                const auto nextOffset = _NextPlaybackOffset(appliedOffset, baseIncrement, correction);
                increment -= nextOffset - appliedOffset;
                appliedOffset = nextOffset;
            }
            if (!(increment > 0.0) || increment > MaximumSourceRate) return 0u;
            const auto first = static_cast<unsigned long>(body);
            const auto fraction = body - first;
            const auto next = first + 1ul == loopLength ? 0ul : first + 1ul;
            const auto linear = static_cast<double>(sampleAtBody(first, frame)) * (1.0 - fraction)
                + static_cast<double>(sampleAtBody(next, frame)) * fraction;
            outBuf[frame] = static_cast<float>(linear);
            peak = (std::max)(peak, std::abs(outBuf[frame]));
            body += increment;
            if (body >= loopLength)
            {
                body -= loopLength;
                // One subtraction covers ordinary loops. Tiny children can
                // wrap several times at the admitted source rate (at most 5).
                if (body >= loopLength) body = std::fmod(body, static_cast<double>(loopLength));
            }
        }
        _lastPeak = peak;
        return count;
    }

	// Check if we are inside crossfading region at any point
	auto isXfadeRegion = _loopParams.FadeSamps != 0u
		&& (index + numSamps) >= (bufSize - _loopParams.FadeSamps);

	auto sampsToWrite = (numSamps <= constants::MaxBlockSize) ? numSamps : constants::MaxBlockSize;

	if (isXfadeRegion)
	{
		// Re-evaluate the seam on the wrapped index, including short loops
		// that cross several seams within one device block.

		for (auto i = 0u; i < sampsToWrite; i++)
		{
			const auto isXfade = index >= (bufSize - _loopParams.FadeSamps) && index < bufSize;

			if (provisional || index < bufBankSize)
			{
				auto samp = readSource(index, i);

				if (isXfade)
				{
					auto xfadeIndex = index - (bufSize - _loopParams.FadeSamps);
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
	int sampOffset, unsigned int numSamps, const audio::DacRenderContext* dac,
	std::uint32_t trigger, std::int64_t delay, unsigned int channel,
	vst::VstChain* preparedChain, bool usePreparedChain, bool sourceResidual)
{
	base::PublishedAudioSnapshot<vst::VstChain>::Lease chainLease;
	if (!usePreparedChain) chainLease = _vstChain.borrow();
	auto* chain = usePreparedChain ? preparedChain : chainLease.get();
	const auto count = std::min(numSamps, constants::MaxBlockSize);
	if (!count) return;
	const auto loopLength = _loopLength.load(std::memory_order_relaxed);
	if (_playState.load(std::memory_order_relaxed) == STATE_RECORDING) _mixer->Offset(count);
	float tempBuf[constants::MaxBlockSize];
	auto rendered = loopLength ? ReadBlock(tempBuf, sampOffset, count,
		bounceWriter ? nullptr : dac, trigger, delay, channel) : 0u;
	if (!rendered)
	{
		if (bounceWriter || !chain || !chain->IsActive())
		{
			_mixer->UpdateVu(0.0f, count);
			ClearRecordedPlaybackDelay();
			return;
		}
		std::fill_n(tempBuf, count, 0.0f);
		rendered = count; // A generator/release/tail is independent of audio material.
	}
	// Compensate the material before instrument injection. Generated audio uses
	// only its destination MIDI delay, so it is never delayed twice.
	if (!bounceWriter && _playbackDelay)
	{
		if (IsMuted()) _playbackDelay->Clear();
		else _playbackDelay->Process(tempBuf, rendered);
	}
	float drySource[constants::MaxBlockSize];
	if (sourceResidual && bounceWriter) std::copy_n(tempBuf, rendered, drySource);
	if (chain && chain->IsActive())
	{
		float instrumentScratch[constants::MaxBlockSize];
		chain->ProcessRackMono(tempBuf, instrumentScratch, static_cast<int>(rendered), !bounceWriter);
	}
	if (sourceResidual && bounceWriter)
		for (unsigned int frame = 0; frame < rendered; ++frame) tempBuf[frame] -= drySource[frame];
	if (!bounceWriter) _mixer->WriteBlock(dest, tempBuf, rendered);
	else bounceWriter->WriteBlock(dest, tempBuf, rendered, LoopChannel());
}

void Loop::EndMultiPlay(unsigned int numSamps)
{
	_CopySpectralSourceAtAudioBoundary();
	_AdvanceSpectralPlayback(numSamps);
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
		
	_AdvancePlaybackOffset(numSamps);

    if (_playbackFraction == 0.0 && _playbackRateBlock.IsConstant()
        && _playbackRateBlock.InitialRate * _playbackRateBlock.SourceScale == _recordingPlaybackRate)
    {
        auto index = static_cast<std::uint64_t>(_playIndex.load(std::memory_order_relaxed)) + numSamps;
        const auto size = static_cast<std::uint64_t>(loopLength) + constants::MaxLoopFadeSamps;
        if (index >= size) index = constants::MaxLoopFadeSamps + (index - constants::MaxLoopFadeSamps) % loopLength;
        _playIndex.store(static_cast<unsigned long>(index), std::memory_order_relaxed);
    }
    else
    {
        const auto progress = _playbackFraction + _playbackRateBlock.Advance(numSamps, _recordingPlaybackRate);
        const auto whole = std::floor(progress);
        _playbackFraction = progress - whole;
        const auto body = (static_cast<std::uint64_t>(BodyPlayIndex()) + static_cast<std::uint64_t>(whole)) % loopLength;
        _playIndex.store(constants::MaxLoopFadeSamps + static_cast<unsigned long>(body), std::memory_order_relaxed);
    }

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
	loop.AllowedMidiChannels = _allowedMidiChannels;
	loop.MidiChannelRoutes = _midiChannelRoutes;
	loop.Id = _loopParams.Id;
	loop.Channel = _loopParams.Channel;
	loop.Length = loopLength;
	loop.Index = (playIndex >= constants::MaxLoopFadeSamps) ?
		(playIndex - constants::MaxLoopFadeSamps) :
		0ul;
	// Files store the requested SHFT. Compensate the saved base for the
	// correction still in flight so cold loading preserves the audible phase.
	if (loopLength)
	{
		auto body = std::fmod(FractionalBodyPlayIndex() + TotalPlaybackOffsetSamps()
			- AppliedPlaybackOffsetSamps(), static_cast<double>(loopLength));
		if (body < 0.0) body += loopLength;
		loop.BodyPlayIndex = static_cast<unsigned long>(std::floor(body));
	}
	else loop.BodyPlayIndex = BodyPlayIndex();
	loop.PlaybackOffsetSamps = PlaybackOffsetSamps();
	loop.MasterLoopCount = 0;
	loop.Level = _mixer->UnmutedLevel();
	loop.Speed = _pitch;
	loop.MuteGroups = 0;
	loop.SelectGroups = 0;
	loop.Muted = IsUserOrSourceMuted(); // Parent intent is saved once on Station.

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
	std::vector<io::JamFile::VstEntry> savedEntries;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		paths = _vstPluginPaths;
		savedEntries = _vstSavedEntries;
		chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	}
	// Plugin state calls may pump native messages and reenter this owner.
	{
		for (size_t i = 0; i < paths.size(); ++i)
		{
			io::JamFile::VstEntry entry = savedEntries[i];

			if (chain && i < chain->NumPlugins())
			{
				auto plugin = chain->GetPlugin(i);
				if (plugin)
				{
					entry.Id = plugin->StateId();
					entry.Bypass = plugin->IsBypassed();
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
	_vstSavedEntries.clear();
	_vstJobsPending.store(0u, std::memory_order_release);
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

bool Loop::CanSetLogicalLengthAtAudioBoundary(unsigned long length) const noexcept
{
	return length > 0ul && length <= PhysicalLoopLength()
		&& _playState.load(std::memory_order_acquire) == STATE_PLAYING
		&& !_isPunchInActive.load(std::memory_order_acquire);
}

void Loop::SetLogicalLengthAtAudioBoundary(unsigned long length) noexcept
{
	const auto phase = BodyPlayIndex();
	if (_loopLength.load(std::memory_order_relaxed) != length) _InvalidateAudioSource();
	_loopLength.store(length, std::memory_order_relaxed);
	SetBodyPlayIndex(phase % length);
	InvalidateSceneAnchor();
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
	_playbackFraction = 0.0;
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
	_captureRateHistory = nullptr;
	_captureSource = nullptr;
	_captureInputHistory = {};
	_captureQueues = {};
	_captureResidualStart.reset();
	_captureResidualEnd = 0;
	_captureInterpolation = {};
	_writeIndex.store(0ul, std::memory_order_relaxed);
	_playIndex.store(0ul, std::memory_order_relaxed);
	_loopLength.store(0ul, std::memory_order_relaxed);
	_playbackFraction = 0.0;
	_playbackOffsetInitialised = false;
	_playbackOffsetPublished.store(false, std::memory_order_release);
	_playbackRateBlock = {};
	_recordingPlaybackRate = 1.0;
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

bool Loop::CanUseAsTimingReference() const noexcept
{
    return _playState.load(std::memory_order_acquire)==STATE_PLAYING && !_isPunchInActive.load(std::memory_order_acquire) &&
        LoopLength()>0ul && LoopLength()<=PhysicalLoopLength();
}

bool Loop::CanEditAudioSource() const noexcept
{
	if (!SupportsSpectralEditing()
		|| _playState.load(std::memory_order_acquire) != STATE_PLAYING
		|| _isPunchInActive.load(std::memory_order_acquire))
		return false;

	const auto length = _loopLength.load(std::memory_order_relaxed);
	return length > 0ul && length <= PhysicalLoopLength();
}

spectral::SpectralSourceLayout Loop::_SpectralSourceLayout() const noexcept
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
	if (std::isfinite(sampleRate) && sampleRate > 0.0f)
		_playbackOffsetDamping = 1.0 / -std::expm1(-1.0 / (sampleRate * 0.010));
}

std::shared_ptr<spectral::SpectralSourceCapture> Loop::RequestSpectralSource(std::string& reason,
    const std::shared_ptr<spectral::SpectralResourceBudget>& budget, const spectral::SpectralSourceLayout* retainedLayout)
{
	reason.clear();
	CancelSpectralSourceCapture();
	if (!CanEditAudioSource())
	{
		reason = "Finish recording or punch before editing";
		return nullptr;
	}
	const auto generation = _audioSourceGeneration.load(std::memory_order_acquire);
	const auto layout = _SpectralSourceLayout();
	if (!layout.IsValid())
	{
		reason = "Audio source layout is unavailable";
		return nullptr;
	}
	if (static_cast<std::uint64_t>(layout.StoredSampleCount()) * sizeof(float)
		> spectral::SpectralSourceCapture::MaxSourceBytes)
	{
		reason = "Audio source exceeds the editor memory budget";
		return nullptr;
	}
	std::shared_ptr<const spectral::SpectralResourceBudget::Reservation> resources;
    auto admissionLayout = layout;
    if (retainedLayout)
    {
        admissionLayout.LogicalLength = (std::max)(admissionLayout.LogicalLength, retainedLayout->LogicalLength);
        admissionLayout.PhysicalLength = (std::max)(admissionLayout.PhysicalLength, retainedLayout->PhysicalLength);
        admissionLayout.FadeSamps = (std::max)(admissionLayout.FadeSamps, retainedLayout->FadeSamps);
    }
    spectral::SpectralTransformConfig transform;
    if (!spectral::SpectralDsp::SelectTransform(admissionLayout, budget, transform, resources, reason))
        return nullptr;
    std::shared_ptr<spectral::SpectralSourceCapture> request;
	try
	{
		request = std::make_shared<spectral::SpectralSourceCapture>(layout,
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

void Loop::CancelSpectralSourceCapture()
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
	const auto edit = _spectralEdit.load(std::memory_order_acquire);
	if (edit && edit->Source->Generation != AudioSourceGeneration())
		_spectralEdit.store(nullptr, std::memory_order_release);
	const auto playback = _spectralPlayback.load();
	if (playback && playback->Source().Generation != AudioSourceGeneration())
		_spectralPlayback.store(nullptr);
	return _audioSourceCapture.collect() + _spectralPlayback.collect();
}

bool Loop::PublishSpectralEdit(
	std::shared_ptr<const spectral::SpectralEditState> state, std::string& reason)
{
	reason.clear();
	if (!state || !IsCurrentSpectralSource(*state->Source))
	{
		reason = "Audio edit belongs to an obsolete or unavailable source";
		return false;
	}
	std::scoped_lock lock(_audioSourcePublicationMutex);
	const auto current = _spectralEdit.load(std::memory_order_acquire);
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
	if (!IsCurrentSpectralSource(*state->Source))
	{
		reason = "Audio source changed before edit publication";
		return false;
	}
	_spectralEdit.store(std::move(state), std::memory_order_release);
	_changesMade = true;
	return true;
}

std::shared_ptr<const spectral::SpectralEditState> Loop::SpectralEdit() const
{
	const auto state = _spectralEdit.load(std::memory_order_acquire);
	return state && IsCurrentSpectralSource(*state->Source) ? state : nullptr;
}

bool Loop::PublishSpectralPlayback(
	std::shared_ptr<const spectral::SpectralPlaybackSnapshot> snapshot, std::string& reason)
{
	reason.clear();
	if (!snapshot || !IsCurrentSpectralSource(snapshot->Source()))
	{
		reason = "Edited playback belongs to an obsolete or unavailable source";
		return false;
	}
	std::scoped_lock lock(_audioSourcePublicationMutex);
	if (snapshot->Source().Generation == _minimumSpectralGeneration.load(std::memory_order_acquire)
        && snapshot->SpectralRevision < _minimumSpectralRevision.load(std::memory_order_acquire))
	{
		reason = "Edited playback was superseded by Undo or stroke cancellation";
		return false;
	}
	const auto committed = _spectralEdit.load(std::memory_order_acquire);
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
	const auto current = _spectralPlayback.load();
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
	if (!IsCurrentSpectralSource(snapshot->Source()))
	{
		reason = "Audio source changed before edited playback publication";
		return false;
	}
	_spectralPlayback.store(std::move(snapshot));
	_changesMade = true;
	return true;
}

void Loop::InvalidateSpectralResultsBefore(std::uint64_t spectralRevision,
    std::uint64_t expectedGeneration)
{
	// Serialize against the final publication check, without involving the audio
	// callback. Callback adoption only reads the scalar floor.
	std::scoped_lock lock(_audioSourcePublicationMutex);
	if (expectedGeneration != AudioSourceGeneration()) return;
    const auto floor = expectedGeneration == _minimumSpectralGeneration.load(std::memory_order_relaxed)
        ? _minimumSpectralRevision.load(std::memory_order_relaxed) : 0u;
	_minimumSpectralRevision.store((std::max)(floor, spectralRevision), std::memory_order_release);
    // Tag protects a new source if punch invalidates during this method.
    _minimumSpectralGeneration.store(expectedGeneration, std::memory_order_release);
}

std::shared_ptr<const spectral::SpectralPlaybackSnapshot> Loop::SpectralPlayback() const
{
	const auto snapshot = _spectralPlayback.load();
	return snapshot && IsCurrentSpectralSource(snapshot->Source()) ? snapshot : nullptr;
}

std::vector<float> Loop::ExportSpectralSamples(
	const spectral::SpectralPlaybackSnapshot& snapshot, bool retainPhysicalTail)
{
	const auto& layout = snapshot.Source().Layout;
	const auto length = retainPhysicalTail ? layout.PhysicalLength : layout.LogicalLength;
	std::vector<float> output(length);
	for (unsigned long index = 0ul; index < length; ++index)
		output[index] = snapshot.SampleAt(layout.BodyOffset + index);
	return output;
}

void Loop::_AdvanceSpectralPlayback(unsigned int numSamps) noexcept
{
	const auto generation = _audioSourceGeneration.load(std::memory_order_acquire);
	if (_activeSpectral && _activeSpectral->Source().Generation != generation)
	{
		_previousSpectral.reset();
		_activeSpectral.reset();
		_spectralTransitionSamps = 0u;
		_spectralTransitionProgress = 0u;
		_appliedSpectralRevision.store(0u, std::memory_order_release);
	}
	if (_spectralTransitionSamps != 0u)
	{
		const auto remaining = _spectralTransitionSamps - _spectralTransitionProgress;
		_spectralTransitionProgress += (std::min)(remaining, numSamps);
		if (_spectralTransitionProgress < _spectralTransitionSamps) return;
		_previousSpectral.reset();
		_spectralTransitionSamps = 0u;
	}
	auto pending = _spectralPlayback.borrow();
	if (!pending || pending->Source().Generation != generation || !CanEditAudioSource()
		|| (pending->Source().Generation == _minimumSpectralGeneration.load(std::memory_order_acquire)
            && pending->SpectralRevision < _minimumSpectralRevision.load(std::memory_order_acquire))
		|| (_activeSpectral && pending->Revision <= _activeSpectral->Revision))
		return;
	_previousSpectral = std::move(_activeSpectral);
	_activeSpectral = std::move(pending);
	_spectralTransitionProgress = 0u;
	_spectralTransitionSamps = static_cast<unsigned int>(std::clamp(
		static_cast<double>(_activeSpectral->Source().Layout.SampleRate)
			* spectral::SpectralConfig::PlaybackTransitionSeconds,
		1.0, static_cast<double>(spectral::SpectralConfig::MaxPlaybackTransitionSamps)));
	_appliedSpectralRevision.store(_activeSpectral->Revision, std::memory_order_release);
	// Releasing a Lease only clears its scalar hazard. Publisher retains ownership
	// and CommitChanges collects off audio after the transition/read has ended.
}

float Loop::_ReadSpectralSample(unsigned long storageIndex,
	unsigned int blockOffset, std::uint64_t generation) const noexcept
{
	const auto original = [this, storageIndex]() noexcept
	{
		return _bufferBank[storageIndex]; // ReadBlock already bounds the body index.
	};
	if (!_activeSpectral || _activeSpectral->Source().Generation != generation)
		return original();
	const auto current = _activeSpectral->SampleAt(storageIndex);
	if (_spectralTransitionSamps == 0u
		|| (_activeSpectral->UsesOriginal()
			&& (!_previousSpectral || _previousSpectral->UsesOriginal()))) return current;
	const auto position = static_cast<std::uint64_t>(_spectralTransitionProgress) + blockOffset;
	if (position >= _spectralTransitionSamps) return current;
	const auto previous = _previousSpectral ? _previousSpectral->SampleAt(storageIndex) : original();
	const auto weight = static_cast<double>(position) / _spectralTransitionSamps;
	// Double convex sum avoids float subtraction overflow for opposite large peaks.
	return static_cast<float>(static_cast<double>(previous) * (1.0 - weight) + current * weight);
}

bool Loop::IsCurrentSpectralSource(const spectral::SpectralSource& source) const noexcept
{
	return source.Identity.get() == _audioSourceIdentity.get()
		&& source.Generation == _audioSourceGeneration.load(std::memory_order_acquire)
		&& CanEditAudioSource() && source.Layout == _SpectralSourceLayout();
}

void Loop::_CopySpectralSourceAtAudioBoundary() noexcept
{
	auto request = _audioSourceCapture.borrow();
	if (!request) return; // A bounded borrow failure simply retries on the next block.
	request->CopyAtAudioBoundary(_bufferBank, _SpectralSourceLayout(),
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
	_captureInterpolation = {};
	_captureQueues = {};
	_captureResidualStart.reset();
	_captureResidualEnd = 0;
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
        || _isPunchInActive.load(std::memory_order_acquire) || _activeSpectral || _previousSpectral)
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

bool Loop::IsUserOrSourceMuted() const noexcept
{
	return Tweakable::IsMuted() || _mixer->IsCaptureSourceMuted();
}

bool Loop::IsMuted() const
{
	return IsUserOrSourceMuted() || (_stationMuteControl && _stationMuteControl->load(std::memory_order_acquire));
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

bool Loop::AllowReplacementPlayback() noexcept
{
	return _mixer && _mixer->AllowReplacementPlayback();
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
		if (const auto edited = SpectralPlayback())
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

std::string Loop::LoadVstPlugin(std::wstring path,
	std::vector<std::uint8_t> initialState)
{
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		if (_vstSavedEntries.size() + _vstJobsPending.load(std::memory_order_acquire) >= 32u) return {};
	}
	JobAction job;
	job.JobActionType = JobAction::JOB_LOADVST;
	job.VstPath = std::move(path);
	job.VstInitialState = std::move(initialState);
	job.VstIdentity = utils::GetGuid();
	_pendingVstLoads.push_back(std::move(job));
	_vstJobsPending.fetch_add(1u, std::memory_order_release);
	++_rackRevision;
	_changesMade = true;
	return _pendingVstLoads.back().VstIdentity;
}

vst::VstRackSlot Loop::GetVstRackSlot() const
{
	vst::VstRackSlot slot;
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	slot.Pending = _vstJobsPending.load(std::memory_order_acquire) != 0u
		|| _flipVstChain.load(std::memory_order_acquire);
	if (_vstSavedEntries.empty()) return slot;
	const auto& entry = _vstSavedEntries.front();
	slot.Path = utils::DecodeUtf8(entry.Path);
	slot.Identity = entry.Id;
	const auto chain = _vstChain.load(std::memory_order_acquire);
	slot.Plugin = chain ? chain->GetPlugin(0u) : nullptr;
	slot.Failed = !slot.Plugin;
	return slot;
}

bool Loop::LoadManagedVstPlugin(std::wstring path, const std::string& identity)
{
	auto slots = GetVstRackSlots();
	const auto found = identity.empty() ? slots.begin() : std::find_if(slots.begin(), slots.end(), [&](const auto& slot) { return slot.Identity == identity; });
	if (path.empty() || (!identity.empty() && found == slots.end())) return false;
	if (found != slots.end() && (found->Pending || found->Plugin)) return false;
	if (LoadVstPlugin(std::move(path)).empty()) return false;
	_pendingVstLoads.back().VstManaged = found != slots.end();
	if (found != slots.end() && !identity.empty()) _pendingVstLoads.back().VstIdentity = found->Identity;
	return true;
}

bool Loop::UnloadManagedVstPlugin(const std::string& identity)
{
	const auto slot = GetVstRackSlot();
	if (slot.Pending || identity.empty() || slot.Identity != identity) return false;
	UnloadVstPlugin(0u);
	return true;
}

bool Loop::LoadVstPluginSynchronously(const std::wstring& path,
	const std::vector<std::uint8_t>& initialState,
	bool bypass, const std::string& stateId)
{
	// Scene::FromFile calls this before Scene::InitAudio, so no callback can
	// retain the old chain while this non-RT construction is in progress.
	auto plugin = vst::MakePluginForPath(path);
	const bool loaded = plugin->PreInit(path)
		&& plugin->Load(path, _sampleRate, _blockSize, 1u, vst::HostedLayoutMode::MonoFlexible);

	plugin->SetStateId(stateId);
	if (loaded && !initialState.empty())
		plugin->SetState(initialState);
	plugin->SetBypassed(bypass);
	const auto identity = plugin->StateId();
	if (!loaded) { vst::QueueForUiThreadDestroy(std::move(plugin)); plugin.reset(); }

	auto chain = _vstChain.load(std::memory_order_acquire);
	auto replacement = std::make_shared<vst::VstChain>();
	if (chain)
	{
		for (std::size_t index = 0u; index < chain->NumPlugins(); ++index)
			replacement->AddSlot(chain->GetPlugin(index));
	}
	replacement->AddSlot(std::move(plugin));
	_vstChain.store(std::move(replacement), std::memory_order_release);
	// Startup still has exclusive ownership; VstEntries cannot run until after
	// this object is published to the main loop.
	_vstPluginPaths.push_back(path);
	io::JamFile::VstEntry saved;
	saved.Path = utils::EncodeUtf8(path);
	saved.Id = identity;
	saved.Bypass = bypass;
	saved.State = io::JamFile::VstEntry::EncodeState(initialState);
	_vstSavedEntries.push_back(std::move(saved));
	++_rackRevision;
	return loaded;
}

void Loop::UnloadVstPlugin(size_t index)
{
	JobAction job;
	job.JobActionType = JobAction::JOB_UNLOADVST;
	job.VstRequestId = utils::GetGuid();
	job.VstIndex = index;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		if (index < _vstSavedEntries.size()) job.VstIdentity = _vstSavedEntries[index].Id;
	}
	_pendingVstUnloads.push_back(std::move(job));
	_vstJobsPending.fetch_add(1u, std::memory_order_release);
	++_rackRevision;
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
		if (const auto playback = SpectralPlayback())
		{
			const auto radius = static_cast<float>(CalcDrawRadius(playback->Source().Layout.LogicalLength) * _DrawRadiusScale());
			_model->UpdateEditedModel(playback, radius);
			_vu->UpdateModel(radius);
		}
	if (_CollectAudioSourceCaptures() != 0u)
		_changesMade = true;
	_vstChain.collect();

	bool vstTopologyChanged = false;
	// Swap in a new VST chain when the job thread has delivered one.
	if (_flipVstChain.load(std::memory_order_acquire))
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		_flipVstChain.store(false, std::memory_order_release);
		for (auto& route : _midiChannelRoutes)
			for (const auto& identity : _pendingDeletedVstRouteIds) std::erase(route.PluginIds, identity);
		_pendingDeletedVstRouteIds.clear();
		_vstChain.store(_backVstChain, std::memory_order_release);
		vstTopologyChanged = true;
	}
	if (vstTopologyChanged && _automationTopologyChanged) _automationTopologyChanged();

	std::vector<JobAction> jobs;

	// Drain pending unload indices — emit one JOB_UNLOADVST per entry so
	// rapid back-to-back UnloadVstPlugin() calls are not silently dropped.
	auto pendingUnloads = std::move(_pendingVstUnloads);
	auto pendingLoads = std::move(_pendingVstLoads);
	for (auto& job : pendingUnloads)
	{
		job.SourceId = Id();
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(std::move(job));
	}
	for (auto& job : pendingLoads)
	{
		job.SourceId = Id();
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
		auto plugin = action.PreInitPlugin ? action.PreInitPlugin : vst::MakePluginForPath(action.VstPath);
		const bool loaded = plugin->Load(action.VstPath, _sampleRate, _blockSize, 1u, vst::HostedLayoutMode::MonoFlexible);
		if (action.VstIdentity.empty()) action.VstIdentity = plugin->StateId();
		plugin->SetStateId(action.VstIdentity);
		if (loaded && !action.VstInitialState.empty()) plugin->SetState(action.VstInitialState);
		if (!loaded) { vst::QueueForUiThreadDestroy(std::move(plugin)); plugin.reset(); }
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			// Multiple serialized jobs can complete before the UI publishes. Compose
			// against the latest delivered chain so no earlier edit is discarded.
			const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
			auto replacement = std::make_shared<vst::VstChain>();
			if (chain) for (size_t i = 0u; i < chain->NumPlugins(); ++i) replacement->AddSlot(chain->GetPlugin(i));
			io::JamFile::VstEntry saved;
			saved.Id = action.VstIdentity;
			saved.Path = utils::EncodeUtf8(action.VstPath);
			saved.State = io::JamFile::VstEntry::EncodeState(action.VstInitialState);
			auto retry = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == action.VstIdentity; });
			// Legacy single-slot requests choose a replacement identity; row requests retain their exact ID.
			if (action.VstManaged && retry == _vstSavedEntries.end() && !_vstSavedEntries.empty() && !replacement->GetPlugin(0u)) retry = _vstSavedEntries.begin();
			if (action.VstManaged && retry != _vstSavedEntries.end())
			{
				const auto index = static_cast<std::size_t>(retry - _vstSavedEntries.begin());
				replacement->ReplaceSlot(index, std::move(plugin));
				_vstPluginPaths[index] = action.VstPath;
				*retry = std::move(saved);
			}
			else
			{
				replacement->AddSlot(std::move(plugin));
				_vstPluginPaths.push_back(action.VstPath);
				_vstSavedEntries.push_back(std::move(saved));
			}
			_backVstChain = std::move(replacement);
			++_rackRevision;
			_flipVstChain.store(true, std::memory_order_release);
		}
		if (_vstJobsPending.load(std::memory_order_acquire) != 0u) _vstJobsPending.fetch_sub(1u, std::memory_order_release);
		_changesMade = true;
		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	case JobAction::JOB_UNLOADVST:
	{
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
			// Identity is captured when requested. An index alone can drift while queued.
			auto removeIndex = _vstSavedEntries.size();
			for (size_t i = 0u; i < _vstSavedEntries.size(); ++i)
				if (!action.VstIdentity.empty() && _vstSavedEntries[i].Id == action.VstIdentity) { removeIndex = i; break; }
			if (removeIndex < _vstSavedEntries.size())
			{
				auto replacement = std::make_shared<vst::VstChain>();
				if (chain) for (size_t i = 0u; i < chain->NumPlugins(); ++i)
				{
					auto existing = chain->GetPlugin(i);
					if (i == removeIndex) { if (existing) vst::QueueForUiThreadDestroy(std::move(existing)); }
					else replacement->AddSlot(std::move(existing));
				}
				_vstPluginPaths.erase(_vstPluginPaths.begin() + static_cast<std::ptrdiff_t>(removeIndex));
				_pendingDeletedVstRouteIds.push_back(_vstSavedEntries[removeIndex].Id);
				_vstSavedEntries.erase(_vstSavedEntries.begin() + static_cast<std::ptrdiff_t>(removeIndex));
				_backVstChain = std::move(replacement);
			++_rackRevision;
				_flipVstChain.store(true, std::memory_order_release);
			}
		}
		if (_vstJobsPending.load(std::memory_order_acquire) != 0u) _vstJobsPending.fetch_sub(1u, std::memory_order_release);
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

void Loop::SetPickerLayoutHeight(float availableHeight) noexcept
{
	if (_model)
		_model->SetPickerHalfHeight(LoopModel::PickerLocalHalfHeight(availableHeight,
			static_cast<float>(ModelScale())));
}

std::vector<vst::VstRackSlot> Loop::GetVstRackSlots(std::size_t offset) const
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	std::vector<vst::VstRackSlot> result;
	for (auto i = std::min(offset, _vstSavedEntries.size()); i < _vstSavedEntries.size() && result.size() < 32u; ++i)
	{
		vst::VstRackSlot slot;
		slot.Identity = _vstSavedEntries[i].Id;
		slot.Path = utils::DecodeUtf8(_vstSavedEntries[i].Path);
		slot.Plugin = chain ? chain->GetPlugin(i) : nullptr;
		slot.Pending = _vstJobsPending.load(std::memory_order_acquire) != 0u || _flipVstChain.load(std::memory_order_acquire);
		slot.Failed = !slot.Plugin && !slot.Pending;
		result.push_back(std::move(slot));
	}
	return result;
}

bool Loop::MoveVstSlot(const vst::RackMove& move)
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	if (_vstJobsPending.load(std::memory_order_acquire) != 0u
		|| !move.Valid(vst::RackMove::Scope::Loop, Id(), RackRevision(), _vstSavedEntries)) return false;
	const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	auto replacement = move.Stage(chain, _vstSavedEntries, _vstPluginPaths);
	if (!replacement) return false;
	_backVstChain = std::move(replacement);
	++_rackRevision;
	_flipVstChain.store(true, std::memory_order_release);
	_changesMade = true;
	return true;
}

unsigned long engine::Loop::OffsetBodyIndex(unsigned long phase, unsigned long length, std::int64_t offset) noexcept
{
	if (!length) return 0ul;
	auto shifted = (static_cast<long long>(phase % length) - static_cast<long long>(offset)) % static_cast<long long>(length);
	if (shifted < 0) shifted += length;
	return static_cast<unsigned long>(shifted);
}
unsigned long engine::Loop::PlaybackBodyIndex() const noexcept
{
	return OffsetBodyIndex(BodyPlayIndex(), LoopLength(),
		static_cast<std::int64_t>(std::llround(AppliedPlaybackOffsetSamps())));
}

// The trajectory is prepared/consumed once at the existing audio advancement boundary.
void engine::Loop::AdoptInheritedPlaybackOffsetAtAudioBoundary() noexcept
{
    if (!LoopLength()) { SetInheritedPlaybackOffsetSamps(0); return; }
    _InitialisePlaybackOffset();
    _PreparePlaybackOffsetTarget();
    const auto inherited = _inheritedOffsetTarget;
    ShiftPlayIndex(-static_cast<std::int64_t>(inherited));
    _playbackOffsetStart -= inherited;
    _playbackOffsetEnd -= inherited;
    _inheritedOffsetTarget = 0;
    _inheritedPlaybackOffsetSamps.store(0, std::memory_order_relaxed);
    _appliedPlaybackOffsetSamps.store(_playbackOffsetStart, std::memory_order_relaxed);
}

double engine::Loop::_NextPlaybackOffset(double value, double baseIncrement, double sharedCorrection) const noexcept
{
    const auto error = _playbackOffsetEnd - value;
    const auto limit = (std::max)(0.0, 0.25 * baseIncrement - std::abs(sharedCorrection));
    if (std::abs(error) <= 1e-6 && std::abs(error) <= limit) return _playbackOffsetEnd;
    const auto next = value + std::clamp(error / _playbackOffsetDamping, -limit, limit);
    // Large persisted signed SHFT values can exhaust double precision before
    // the absolute tolerance. Finish only if the remainder fits this frame's budget.
    return next == value && std::abs(error) <= limit ? _playbackOffsetEnd : next;
}
void engine::Loop::_AdvancePlaybackOffset(unsigned int numSamps) noexcept
{
    static_assert(std::atomic<double>::is_always_lock_free);
    _InitialisePlaybackOffset();
    if (_playbackOffsetStart != _playbackOffsetEnd)
    {
        auto rate = _playbackRateBlock.Cursor();
        const bool constantRate = _playbackRateBlock.IsConstant();
        const auto constantIncrement = _playbackRateBlock.InitialRate * _playbackRateBlock.SourceScale / _recordingPlaybackRate;
        for (unsigned int frame = 0; frame < numSamps && _playbackOffsetStart != _playbackOffsetEnd; ++frame)
        {
            if (!constantRate) rate.Next();
            _playbackOffsetStart = _NextPlaybackOffset(_playbackOffsetStart,
                constantRate ? constantIncrement : rate.BaseIncrement() / _recordingPlaybackRate,
                constantRate ? 0.0 : rate.Correction() / _recordingPlaybackRate);
        }
    }
    _PreparePlaybackOffsetTarget();
    _appliedPlaybackOffsetSamps.store(_playbackOffsetStart, std::memory_order_relaxed);
}

void engine::Loop::_PreparePlaybackOffsetTarget() noexcept
{
    const auto local = PlaybackOffsetSamps();
    const auto inherited = _inheritedPlaybackOffsetSamps.load(std::memory_order_relaxed);
    const auto length = static_cast<double>(LoopLength());
    auto localChange = std::fmod(static_cast<double>(local) - _localOffsetTarget, length);
    if (localChange > length * 0.5) localChange -= length;
    if (localChange <= -length * 0.5) localChange += length;
    // Only a child-local SHFT chooses a shortest circular route. Parent-scope
    // motion has one signed route even when child lengths differ.
    _playbackOffsetEnd += localChange + static_cast<double>(inherited) - _inheritedOffsetTarget;
    _localOffsetTarget = local;
    _inheritedOffsetTarget = inherited;
    if (_playbackOffsetStart == _playbackOffsetEnd)
        _playbackOffsetStart = _playbackOffsetEnd = static_cast<double>(TotalPlaybackOffsetSamps());
    _playbackOffsetRate = _playbackOffsetStart == _playbackOffsetEnd ? 0.0 : 1.0;
}

void engine::Loop::_InitialisePlaybackOffset() noexcept
{
    const auto length = LoopLength();
    if (_playbackOffsetInitialised)
    {
        if (_playbackOffsetLength == length) return;
        // A new child length changes only modulo wrapping. Keep the pending
        // signed correction so geometry replacement cannot discard its debt.
        _playbackOffsetLength = length;
        return;
    }
    _playbackOffsetStart = _playbackOffsetEnd = static_cast<double>(TotalPlaybackOffsetSamps());
    _localOffsetTarget = PlaybackOffsetSamps();
    _inheritedOffsetTarget = _inheritedPlaybackOffsetSamps.load(std::memory_order_relaxed);
    _playbackOffsetRate = 0.0;
    _playbackOffsetInitialised = true;
    _playbackOffsetLength = length;
    _appliedPlaybackOffsetSamps.store(_playbackOffsetStart, std::memory_order_relaxed);
    _playbackOffsetPublished.store(true, std::memory_order_release);
}

void Loop::SetAllowedMidiChannels(const std::vector<int>& channels)
{
	std::vector<int> filtered;
	std::uint16_t mask = 0u;
	for (auto channel : channels)
		if (channel >= 1 && channel <= 16 && !(mask & (1u << (channel - 1))))
		{
			mask |= static_cast<std::uint16_t>(1u << (channel - 1));
			filtered.push_back(channel);
		}
	const auto removed = static_cast<std::uint16_t>(_allowedMidiChannelMask & ~mask);
	for (auto channel = 0u; channel < 16u; ++channel)
		if (removed & (1u << channel)) _midiChannelRoutes[channel] = {};
	_allowedMidiChannels = std::move(filtered);
	_allowedMidiChannelMask = mask;
	++_rackRevision;
	_changesMade = true;
	if (_automationTopologyChanged) _automationTopologyChanged();
}

bool Loop::SetMidiChannelRoutes(std::array<midi::MidiChannelRoute,16> routes, std::uint64_t revision)
{
	if (revision != RackRevision()) return false;
	std::string diagnostic;
	auto allowed = vst::RackState::ValidateChannelRoutes(routes, _midiChannelRoutes,
		GetVstRackSlots(), _allowedMidiChannels, diagnostic);
	if (!allowed) return false;
	SetAllowedMidiChannels(*allowed);
	_midiChannelRoutes = std::move(routes);
	if (_automationTopologyChanged) _automationTopologyChanged();
	return true;
}

bool Loop::RestoreMidiRouting(const std::vector<int>& channels, std::array<midi::MidiChannelRoute,16> routes)
{
	if (!io::JamFile::ValidSavedRouting(_vstSavedEntries, routes)) return false;
	SetAllowedMidiChannels(channels);
	_midiChannelRoutes = std::move(routes);
	if (_automationTopologyChanged) _automationTopologyChanged();
	return true;
}
