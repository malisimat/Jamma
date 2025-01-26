#include "Loop.h"

using namespace base;
using namespace engine;
using namespace resources;
using base::AudioSink;
using audio::BufferBank;
using audio::AudioMixer;
using audio::Hanning;
using audio::AudioMixerParams;
using graphics::GlDrawContext;

Loop::Loop(LoopParams loopParams,
	AudioMixerParams mixerParams) :
	GuiElement(loopParams),
	Tweakable(loopParams),
	AudioSink(),
	_playIndex(0),
	_lastPeak(0.0f),
	_pitch(1.0),
	_loopLength(0),
	_playState(STATE_INACTIVE),
	_loopParams(loopParams),
	_mixer(nullptr),
	_hanning(nullptr),
	_model(nullptr),
	_bufferBank(BufferBank())
{
	_mixer = std::make_unique<AudioMixer>(mixerParams);
	_hanning = std::make_unique<Hanning>(loopParams.FadeSamps);

	LoopModelParams modelParams;
	modelParams.Size = { 12, 14 };
	modelParams.ModelScale = 1.0f;
	modelParams.ModelTextures = { "levels" };
	modelParams.ModelShaders = { "texture_shaded", "picker", "white"};
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
		if (loopStruct.Mix.Params.index() == 1)
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

	auto pos = ModelPosition();
	auto scale = ModelScale();

	auto isRecording = (STATE_RECORDING == _playState) ||
		(STATE_OVERDUBBING == _playState) ||
		(STATE_PUNCHEDIN == _playState);
	auto index = isRecording ?
		_writeIndex :
		_playIndex;

	if (!isRecording)
	{
		if (index >= constants::MaxLoopFadeSamps)
			index -= constants::MaxLoopFadeSamps;
	}

	auto frac = _loopLength == 0 ? 0.0 : 1.0 - std::max(0.0, std::min(1.0, ((double)(index % _loopLength)) / ((double)_loopLength)));
	_model->SetLoopIndexFrac(frac);
	_model->SetLoopState(GetLoopModelState(pass, _playState, IsMuted()));

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

int Loop::OnMixWrite(float samp,
	float fadeCurrent,
	float fadeNew,
	int indexOffset,
	Audible::AudioSourceType source)
{
	if ((STATE_RECORDING != _playState) &&
		(STATE_PLAYINGRECORDING != _playState) &&
		(STATE_OVERDUBBING != _playState) &&
		(STATE_PUNCHEDIN != _playState) &&
		(STATE_OVERDUBBINGRECORDING != _playState))
		return indexOffset;
	
	if (AUDIOSOURCE_MONITOR == source)
	{
		_monitorBufferBank[_writeIndex + indexOffset] = (fadeNew * samp) + (fadeCurrent * _monitorBufferBank[_writeIndex + indexOffset]);

		auto peak = std::abs(samp);
		if (STATE_RECORDING == _playState)
		{
			if (peak > _lastPeak)
				_lastPeak = peak;
		}
	}
	else
		_bufferBank[_writeIndex + indexOffset] = (fadeNew * samp) + (fadeCurrent * _bufferBank[_writeIndex + indexOffset]);

	return indexOffset + 1;
}

void Loop::EndWrite(unsigned int numSamps,
	bool updateIndex)
{
	// Only update if currently recording
	if ((STATE_RECORDING != _playState) &&
		(STATE_PLAYINGRECORDING != _playState) &&
		(STATE_OVERDUBBING != _playState) &&
		(STATE_PUNCHEDIN != _playState) &&
		(STATE_OVERDUBBINGRECORDING != _playState))
		return;

	if (!updateIndex)
		return;

	_writeIndex += numSamps;
	_bufferBank.SetLength(_writeIndex);

	if (STATE_RECORDING == _playState)
	{
		_monitorBufferBank.SetLength(_writeIndex);

		auto newValue = _lastPeak * _mixer->Level();
		_vu->SetValue(newValue, numSamps);
		_lastPeak = 0.0f;
	}
}

void Loop::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int sampOffset,
	unsigned int numSamps)
{
	// Mixer will stereo spread the mono wav
	// and adjust level
	if (0 == _loopLength)
		return;

	if (STATE_RECORDING == _playState)
		_mixer->Offset(numSamps);

	auto isPlaying = (STATE_PLAYING == _playState) ||
		(STATE_PLAYINGRECORDING == _playState) ||
		(STATE_OVERDUBBINGRECORDING == _playState);

	if (!isPlaying)
		return;

	auto peak = 0.0f;
	auto bufBankSize = _bufferBank.Length();
	auto bufSize = _loopLength + constants::MaxLoopFadeSamps;

	// _playIndex is always within range:
	// [constants::MaxLoopFadeSamps : _loopLength + constants::MaxLoopFadeSamps - 1)
	// index should apply the offset and then also stay in the same range
	auto index = _playIndex;

	if (sampOffset >= 0)
	{
		index += sampOffset;
	}
	else
	{
		while (index < (unsigned long)(-sampOffset))
			index += _loopLength;

		index += sampOffset;
	}

	while (index >= bufSize)
		index -= _loopLength;

	if (index < constants::MaxLoopFadeSamps)
		index += _loopLength;

	// Check if we are inside crossfading region at any point
	auto isXfadeRegion = (index + numSamps) >= (bufSize - _loopParams.FadeSamps);

	if (isXfadeRegion)
	{
		// Store the offset play index to avoid the effect of
		// index wrapping around, which will cause xfade region
		// calc to fail
		auto startIndex = index;

		for (auto i = 0u; i < numSamps; i++)
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

				if (nullptr == trigger)
					_mixer->OnPlay(dest, samp, i);
				else
					trigger->OnPlay(dest, samp, i);

				if (std::abs(samp) > peak)
					peak = std::abs(samp);
			}

			index++;
			if (index >= bufSize)
				index -= _loopLength;
		}
	}
	else
	{
		for (auto i = 0u; i < numSamps; i++)
		{
			if (index < bufBankSize)
			{
				auto samp = _bufferBank[index];

				if (nullptr == trigger)
					_mixer->OnPlay(dest, samp, i);
				else
					trigger->OnPlay(dest, samp, i);

				if (std::abs(samp) > peak)
					peak = std::abs(samp);
			}

			index++;
			if (index >= bufSize)
				index -= _loopLength;
		}
	}

	_lastPeak = peak;
}

void Loop::EndMultiPlay(unsigned int numSamps)
{
	auto isPlaying = (STATE_PLAYING == _playState) ||
		(STATE_PLAYINGRECORDING == _playState) ||
		(STATE_OVERDUBBING == _playState) ||
		(STATE_PUNCHEDIN == _playState) ||
		(STATE_OVERDUBBINGRECORDING == _playState);

	if (!isPlaying)
		return;

	if (0 == _loopLength)
		return;
		
	_playIndex += numSamps;

	auto bufSize = _loopLength + constants::MaxLoopFadeSamps;
	while (_playIndex > bufSize)
		_playIndex -= _loopLength;

	for (unsigned int chan = 0; chan < NumOutputChannels(); chan++)
	{
		auto channel = OutputChannel(chan);
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

void Loop::Update()
{
	UpdateLoopModel();

	_bufferBank.UpdateCapacity();
	_monitorBufferBank.UpdateCapacity();
}

bool Loop::Load(const io::WavReadWriter& readWriter)
{
	auto loadOpt = readWriter.Read(utils::DecodeUtf8(_loopParams.Wav), constants::MaxLoopBufferSize);

	if (!loadOpt.has_value())
		return false;

	auto [buffer, sampleRate, bitDepth] = loadOpt.value();

	_loopLength = 0;
	_bufferBank.Init();

	auto length = (unsigned long)buffer.size();
	_bufferBank.SetLength(length);
	_bufferBank.UpdateCapacity();

	for (auto i = 0u; i < length; i++)
	{
		_bufferBank[i] = buffer[i];
	}

	_loopLength = length - constants::MaxLoopFadeSamps;

	UpdateLoopModel();

	std::cout << "-=-=- Loop LOADED " << _loopParams.Wav << std::endl;

	return true;
}

void Loop::Record()
{
	if (STATE_INACTIVE != _playState)
		return;

	Reset();

	_playState = STATE_RECORDING;
	_bufferBank.SetLength(constants::MaxLoopFadeSamps);
	_bufferBank.UpdateCapacity();

	_monitorBufferBank.SetLength(constants::MaxLoopFadeSamps);
	_monitorBufferBank.UpdateCapacity();

	std::cout << "-=-=- Loop " << _playState << " - " << _loopParams.Id << std::endl;
}

void Loop::Play(unsigned long index,
	unsigned long loopLength,
	bool continueRecording)
{
	auto bufSize = _bufferBank.Length();

	if (0 == bufSize)
	{
		Reset();
		return;
	}

	_playIndex = index >= bufSize ? (bufSize-1) : index;
	_loopLength = loopLength;

	auto isOverdubbing = (STATE_OVERDUBBING == _playState) || (STATE_PUNCHEDIN == _playState);
	auto recordState = isOverdubbing ? STATE_OVERDUBBINGRECORDING : STATE_PLAYINGRECORDING;
	auto playState = continueRecording ? recordState : STATE_PLAYING;
	_playState = loopLength > 0 ? playState : STATE_INACTIVE;

	std::cout << "-=-=- Loop " << _playState << " - " << _loopParams.Id << std::endl;
}

bool Loop::Select()
{
	auto isNewState = GuiElement::Select();

	if (isNewState)
		_mixer->Select();

	return isNewState;
}

bool Loop::DeSelect()
{
	auto isNewState = GuiElement::DeSelect();

	if (isNewState)
		_mixer->DeSelect();

	return isNewState;
}

bool Loop::Mute()
{
	auto isNewState = Tweakable::Mute();

	if (isNewState)
		_mixer->Mute();

	return isNewState;
}

bool Loop::UnMute()
{
	auto isNewState = Tweakable::UnMute();

	if (isNewState)
		_mixer->UnMute();

	return isNewState;
}

void Loop::EndRecording()
{
	if ((STATE_PLAYINGRECORDING != _playState) &&
		(STATE_OVERDUBBINGRECORDING != _playState))
		return;
	
	_playState = STATE_PLAYING;

	std::cout << "-=-=- Loop " << _playState << " - " << _loopParams.Id << std::endl;
}

void Loop::Ditch()
{
	if (STATE_INACTIVE == _playState)
		return;

Reset();

_bufferBank.SetLength(constants::MaxLoopFadeSamps);
_bufferBank.UpdateCapacity();

std::cout << "-=-=- Loop DITCH" << std::endl;
}

void Loop::Overdub()
{
	if (STATE_INACTIVE != _playState)
		return;

	Reset();

	_playState = STATE_OVERDUBBING;
	_bufferBank.SetLength(constants::MaxLoopFadeSamps);
	_bufferBank.UpdateCapacity();

	_monitorBufferBank.SetLength(constants::MaxLoopFadeSamps);
	_monitorBufferBank.UpdateCapacity();

	std::cout << "-=-=- Loop " << _playState << " - " << _loopParams.Id << std::endl;
}

void Loop::PunchIn()
{
	if (STATE_OVERDUBBING != _playState)
		return;

	_playState = STATE_PUNCHEDIN;

	std::cout << "-=-=- Loop " << _playState << " - " << _loopParams.Id << std::endl;
}

void Loop::PunchOut()
{
	if (STATE_PUNCHEDIN != _playState)
		return;

	_playState = STATE_OVERDUBBING;

	std::cout << "-=-=- Loop " << _playState << " - " << _loopParams.Id << std::endl;
}

void Loop::Reset()
{
	_playState = STATE_INACTIVE;

	_writeIndex = 0ul;
	_playIndex = 0ul;
	_loopLength = 0ul;
	_mixer->UnMute();
	_mixer->SetUnmutedLevel(AudioMixer::DefaultLevel);

	std::cout << "-=-=- Loop RESET" << std::endl;
}

unsigned long Loop::LoopIndex() const
{
	if (constants::MaxLoopFadeSamps > _playIndex)
		return 0;

	return _playIndex - constants::MaxLoopFadeSamps;
}

double Loop::CalcDrawRadius(unsigned long loopLength)
{
	auto minRadius = 50.0;
	auto maxRadius = 400.0;
	auto radius = 70.0 * log(loopLength) - 600;

	return std::clamp(radius, minRadius, maxRadius);
}

LoopModel::LoopModelState Loop::GetLoopModelState(base::DrawPass pass, LoopPlayState state, bool isMuted)
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

void Loop::UpdateLoopModel()
{
	auto isRecording = (STATE_RECORDING == _playState) ||
		(STATE_OVERDUBBING == _playState) ||
		(STATE_PUNCHEDIN == _playState);
	auto length = isRecording ? _writeIndex : _loopLength;
	auto offset = isRecording ? 0ul : constants::MaxLoopFadeSamps;

	auto radius = (float)CalcDrawRadius(length);
	auto& bufBank = isRecording ? _monitorBufferBank : _bufferBank;
	_model->UpdateModel(bufBank, length, offset, radius);
	_vu->UpdateModel(radius);
}
