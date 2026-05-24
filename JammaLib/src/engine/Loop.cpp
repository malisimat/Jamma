#include "Loop.h"
#include <algorithm>
#include <cmath>

namespace
{
	void DrainVstChain(std::shared_ptr<vst::VstChain> chain)
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
	DrainVstChain(_vstChain.load(std::memory_order_acquire));
	DrainVstChain(_backVstChain);
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
	_mixer = std::make_unique<AudioMixer>(mixerParams);
	_hanning = std::make_unique<Hanning>(params.FadeSamps);

	LoopModelParams modelParams;
	modelParams.Size = { 12, 14 };
	modelParams.ModelScale = 1.0f;
	modelParams.ModelTextures = { "levels" };
	modelParams.ModelShaders = { "waveform", "picker", "white"};
	_model = std::make_shared<LoopModel>(modelParams);

	VuParams vuParams;
	vuParams.Size = { 12, 18 };
	vuParams.ModelScale = 1.0f;
	vuParams.ModelTextures = { "blue" };
	vuParams.ModelShaders = { "vu" };
	vuParams.LedHeight = 1.0f;
	_vu = std::make_shared<VU>(vuParams);

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
	auto loop = std::make_shared<Loop>(loopParams, mixerParams);

	loop->Load(io::WavReadWriter());
	loop->Play(loopStruct.MasterLoopCount, loopStruct.Length, false);

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

void Loop::SetSize(utils::Size2d size)
{
	auto mixerParams = GetMixerParams(size,
		audio::WireMixBehaviourParams());

	_mixer->SetSize(mixerParams.Size);

	GuiElement::SetSize(size);
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

	_modelScreenPos = glCtx.ProjectScreen(pos);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale, scale)));

	if (PASS_SCENE == pass)
		_vu->Draw3d(ctx, 1, pass);

	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(1.0f, _mixer->UnmutedLevel(), 1.0f)));
	
	_model->Draw3d(ctx, 1, pass);
	
	glCtx.PopMvp();
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

	auto writeIndex = _writeIndex.load(std::memory_order_relaxed);

	if (AUDIOSOURCE_MONITOR == request.source)
	{
		float peak = _lastPeak;

		for (unsigned int i = 0; i < request.numSamps; i++)
		{
			auto samp = request.samples[i * request.stride];
			auto idx = writeIndex + writeOffset + i;
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
		for (unsigned int i = 0; i < request.numSamps; i++)
		{
			auto samp = request.samples[i * request.stride];
			auto idx = writeIndex + writeOffset + i;
			_bufferBank[idx] = (request.fadeNew * samp) + (request.fadeCurrent * _bufferBank[idx]);
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

	auto writeIndex = _writeIndex.load(std::memory_order_relaxed) + numSamps;
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

// Only called when outputting to DAC
unsigned int Loop::ReadBlock(float* outBuf,
	int sampOffset,
	unsigned int numSamps)
{
	auto playState = _playState.load(std::memory_order_acquire);
	auto loopLength = _loopLength.load(std::memory_order_relaxed);
	auto isPlaying = (STATE_PLAYING == playState) ||
		(STATE_PLAYINGRECORDING == playState) ||
		(STATE_OVERDUBBINGRECORDING == playState) ||
		(STATE_PUNCHEDIN == playState);

	if (!isPlaying || (0 == loopLength))
		return 0;

	auto peak = 0.0f;
	auto bufBankSize = _bufferBank.Length();
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

			if (index < bufBankSize)
			{
				auto samp = _bufferBank[index];

				if (isXfade)
				{
					auto xfadeIndex = (startIndex + i) - (bufSize - _loopParams.FadeSamps);
					auto xfadeBufIndex = constants::MaxLoopFadeSamps + xfadeIndex - _loopParams.FadeSamps;
					auto xfadeSamp = _bufferBank[xfadeBufIndex];

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
			if (index < bufBankSize)
			{
				outBuf[i] = _bufferBank[index];

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
void Loop::WriteBlock(const std::shared_ptr<MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int sampOffset,
	unsigned int numSamps)
{
	// Mixer will stereo spread the mono wav
	// and adjust level
	auto loopLength = _loopLength.load(std::memory_order_relaxed);
	auto playState = _playState.load(std::memory_order_relaxed);
	if (0 == loopLength)
		return;

	if (STATE_RECORDING == playState)
		_mixer->Offset(numSamps);

	// Read source data from BufferBank into stack-allocated temp buffer
	float tempBuf[constants::MaxBlockSize];
	auto sampsToWrite = ReadBlock(tempBuf, sampOffset, numSamps);

	if (sampsToWrite > 0)
	{
		auto chain = _vstChain.load(std::memory_order_acquire);
		if (chain && chain->IsActive())
			chain->ProcessBlock(tempBuf, static_cast<int>(sampsToWrite));

		// Route to destination via mixer or trigger
		// (both ultimately call dest->OnBlockWriteChannel)
		if (nullptr == trigger)
			_mixer->WriteBlock(dest, tempBuf, sampsToWrite);
		else
			trigger->WriteBlock(dest, tempBuf, sampsToWrite);
	}
}

void Loop::EndMultiPlay(unsigned int numSamps)
{
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

std::vector<float> Loop::ExportSamples() const
{
	auto playState = _playState.load(std::memory_order_acquire);
	auto loopLength = _loopLength.load(std::memory_order_relaxed);

	if ((0 == loopLength) || (STATE_INACTIVE == playState))
		return {};

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
	loop.Length = loopLength;
	loop.Index = (playIndex >= constants::MaxLoopFadeSamps) ?
		(playIndex - constants::MaxLoopFadeSamps) :
		0ul;
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

	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		for (const auto& vstPath : _vstPluginPaths)
		{
			io::JamFile::VstEntry entry;
			entry.Path = utils::EncodeUtf8(vstPath);
			entry.Bypass = false;
			loop.VstChain.push_back(std::move(entry));
		}
	}

	return loop;
}

void Loop::SetMixerLevel(double level)
{
	_mixer->SetUnmutedLevel(level);
}

void Loop::SetVisualUpdatesEnabled(bool enabled)
{
	_visualUpdatesEnabled = enabled;
}

bool Loop::Load(const io::WavReadWriter& readWriter)
{
	auto loadOpt = readWriter.Read(utils::DecodeUtf8(_loopParams.Wav), constants::MaxLoopBufferSize);

	if (!loadOpt.has_value())
		return false;

	auto [buffer, sampleRate, bitDepth] = loadOpt.value();

	_loopLength.store(0, std::memory_order_relaxed);
	_bufferBank.Init();

	auto length = (unsigned long)buffer.size();
	_bufferBank.Resize(length);

	for (auto i = 0u; i < length; i++)
	{
		_bufferBank[i] = buffer[i];
	}

	_loopLength.store(length - constants::MaxLoopFadeSamps, std::memory_order_relaxed);

	_UpdateLoopModel();

	std::cout << "-=-=- Loop LOADED " << _loopParams.Wav << std::endl;

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

	std::cout << "-=-=- Loop " << _playState.load(std::memory_order_relaxed) << " - " << _loopParams.Id << std::endl;
}

void Loop::Play(unsigned long index,
	unsigned long loopLength,
	bool continueRecording)
{
	auto physBufSize = _bufferBank.Length();

	if (0 == physBufSize)
	{
		Reset();
		return;
	}

	// Clamp against the smaller of the logical loop size and the currently
	// recorded physical size. Keep the MaxLoopFadeSamps logical offset in this
	// bound: playback indices are in [fadeOffset, fadeOffset + loopLength).
	auto logicalBufSize = loopLength + constants::MaxLoopFadeSamps;
	auto effectiveBufSize = std::min(logicalBufSize, physBufSize);
	_playIndex.store((effectiveBufSize > 0 && index >= effectiveBufSize) ? (effectiveBufSize - 1) : index, std::memory_order_relaxed);
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

	std::cout << "-=-=- Loop " << _playState.load(std::memory_order_relaxed) << " - " << _loopParams.Id << std::endl;
}

void Loop::Reset()
{
	_isPunchInActive.store(false, std::memory_order_relaxed);

	_writeIndex.store(0ul, std::memory_order_relaxed);
	_playIndex.store(0ul, std::memory_order_relaxed);
	_loopLength.store(0ul, std::memory_order_relaxed);
	_playState.store(STATE_INACTIVE, std::memory_order_release);
	_mixer->UnMute();
	_mixer->SetUnmutedLevel(AudioMixer::DefaultLevel);
}

void Loop::Update()
{
	_UpdateLoopModel();

	_bufferBank.UpdateCapacity();
	_monitorBufferBank.UpdateCapacity();
}

void Loop::EndRecording()
{
	auto playState = _playState.load(std::memory_order_relaxed);
	if ((STATE_PLAYINGRECORDING != playState) &&
		(STATE_OVERDUBBINGRECORDING != playState))
		return;
	
	_playState.store(STATE_PLAYING, std::memory_order_release);

	std::cout << "-=-=- Loop " << _playState.load(std::memory_order_relaxed) << " - " << _loopParams.Id << std::endl;
}

void Loop::Ditch()
{
	if (STATE_INACTIVE == _playState.load(std::memory_order_relaxed))
		return;

Reset();

_bufferBank.Resize(constants::MaxLoopFadeSamps);

std::cout << "-=-=- Loop DITCH" << std::endl;
}

bool Loop::Mute()
{
	auto isNewState = Tweakable::Mute();

	if (isNewState && _mixer)
		_mixer->Mute();

	return isNewState;
}

bool Loop::UnMute()
{
	auto isNewState = Tweakable::UnMute();

	if (isNewState && _mixer)
		_mixer->UnMute();

	return isNewState;
}

void Loop::Overdub()
{
	if (STATE_INACTIVE != _playState.load(std::memory_order_relaxed))
		return;

	Reset();
	_bufferBank.Resize(constants::MaxLoopFadeSamps);

	_monitorBufferBank.Resize(constants::MaxLoopFadeSamps);
	_playState.store(STATE_OVERDUBBING, std::memory_order_release);

	std::cout << "-=-=- Loop " << _playState.load(std::memory_order_relaxed) << " - " << _loopParams.Id << std::endl;
}

void Loop::PunchIn()
{
	auto playState = _playState.load(std::memory_order_relaxed);
	if ((STATE_OVERDUBBING != playState) &&
		(STATE_OVERDUBBINGRECORDING != playState) &&
		(STATE_PLAYING != playState))
		return;

	_isPunchInActive.store(true, std::memory_order_release);
	if (STATE_OVERDUBBING == playState)
		_playState.store(STATE_PUNCHEDIN, std::memory_order_release);

	std::cout << "-=-=- Loop " << _playState.load(std::memory_order_relaxed) << " - " << _loopParams.Id << std::endl;
}

void Loop::PunchOut()
{
	auto playState = _playState.load(std::memory_order_relaxed);
	if (!_isPunchInActive.load(std::memory_order_relaxed) && (STATE_PUNCHEDIN != playState))
		return;

	_isPunchInActive.store(false, std::memory_order_release);
	if (STATE_PUNCHEDIN == playState)
		_playState.store(STATE_OVERDUBBING, std::memory_order_release);

	std::cout << "-=-=- Loop " << _playState.load(std::memory_order_relaxed) << " - " << _loopParams.Id << std::endl;
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

double Loop::_CalcDrawRadius(unsigned long loopLength)
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

	auto radius = (float)(_CalcDrawRadius(displayLength) * _DrawRadiusScale());
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

void Loop::LoadVstPlugin(std::wstring path)
{
	_pendingVstLoads.push_back(std::move(path));
	_changesMade = true;
}

void Loop::UnloadVstPlugin(size_t index)
{
	_pendingVstUnloads.push_back(index);
	_changesMade = true;
}

std::shared_ptr<vst::VstPlugin> Loop::GetVstPlugin(size_t index) const
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	if (!chain)
		return nullptr;

	return chain->GetPlugin(index);
}

std::vector<JobAction> Loop::_CommitChanges()
{
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
	for (auto& path : pendingLoads)
	{
		JobAction job;
		job.JobActionType = JobAction::JOB_LOADVST;
		job.SourceId = Id();
		job.VstPath = std::move(path);
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
			: std::make_shared<vst::VstPlugin>();
		if (plugin->Load(action.VstPath, _sampleRate, _blockSize, 1u, vst::HostedLayoutMode::MonoFlexible))
		{
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
