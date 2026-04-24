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

Loop::Loop(LoopParams params,
	AudioMixerParams mixerParams) :
	Jammable(params),
	AudioSink(),
	_playIndex(0),
	_lastPeak(0.0f),
	_pitch(1.0),
	_loopLength(0),
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
	_model->SetLoopState(_GetLoopModelState(pass, _playState, IsMuted()));

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
	if ((STATE_RECORDING != _playState) &&
		(STATE_PLAYINGRECORDING != _playState) &&
		(STATE_OVERDUBBING != _playState) &&
		(STATE_PUNCHEDIN != _playState) &&
		(STATE_OVERDUBBINGRECORDING != _playState))
		return;

	if ((STATE_OVERDUBBING == _playState) &&
		(AUDIOSOURCE_BOUNCE != request.source))
		return;

	if (AUDIOSOURCE_MONITOR == request.source)
	{
		float peak = _lastPeak;

		for (unsigned int i = 0; i < request.numSamps; i++)
		{
			auto samp = request.samples[i * request.stride];
			auto idx = _writeIndex + writeOffset + i;
			_monitorBufferBank[idx] = (request.fadeNew * samp) + (request.fadeCurrent * _monitorBufferBank[idx]);

			if (STATE_RECORDING == _playState)
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
		auto fadeCurrent = request.fadeCurrent;
		if ((STATE_PUNCHEDIN == _playState) &&
			(AUDIOSOURCE_BOUNCE == request.source))
		{
			// Bounce audio must add to the live ADC already written this block
			// instead of replacing it, so preserve the existing buffer content.
			fadeCurrent = 1.0f;
		}

		for (unsigned int i = 0; i < request.numSamps; i++)
		{
			auto samp = request.samples[i * request.stride];
			auto idx = _writeIndex + writeOffset + i;
			_bufferBank[idx] = (request.fadeNew * samp) + (fadeCurrent * _bufferBank[idx]);
		}
	}
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

// Only called when outputting to DAC
unsigned int Loop::ReadBlock(float* outBuf,
	int sampOffset,
	unsigned int numSamps)
{
	auto isPlaying = (STATE_PLAYING == _playState) ||
		(STATE_PLAYINGRECORDING == _playState) ||
		(STATE_OVERDUBBINGRECORDING == _playState);

	if (!isPlaying)
		return 0;

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
				index -= _loopLength;
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
				index -= _loopLength;
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
	if (0 == _loopLength)
		return;

	if (STATE_RECORDING == _playState)
		_mixer->Offset(numSamps);

	// Read source data from BufferBank into stack-allocated temp buffer
	float tempBuf[constants::MaxBlockSize];
	auto sampsToWrite = ReadBlock(tempBuf, sampOffset, numSamps);

	if (sampsToWrite > 0)
	{
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
	while (_playIndex >= bufSize)
		_playIndex -= _loopLength;

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

void Loop::Update()
{
	_UpdateLoopModel();

	_bufferBank.UpdateCapacity();
	_monitorBufferBank.UpdateCapacity();
}

void Loop::SetMixerLevel(double level)
{
	_mixer->SetUnmutedLevel(level);
}

std::vector<float> Loop::ExportSamples() const
{
	if (_loopLength == 0 || _playState == STATE_INACTIVE)
		return {};

	std::vector<float> out(_loopLength);
	for (unsigned long i = 0; i < _loopLength; ++i)
		out[i] = _bufferBank[constants::MaxLoopFadeSamps + i];
	return out;
}

io::JamFile::Loop Loop::ToJamFile(const std::string& wavFilename) const
{
	io::JamFile::Loop s;
	s.Name   = wavFilename;
	s.Length = _loopLength;
	s.Index  = (_playIndex >= constants::MaxLoopFadeSamps)
	               ? _playIndex - constants::MaxLoopFadeSamps : 0ul;
	s.MasterLoopCount = 0;
	s.Level  = _mixer->UnmutedLevel();
	s.Speed  = _pitch;
	s.Muted  = IsMuted();
	s.MuteGroups   = 0;
	s.SelectGroups = 0;

	auto params = _mixer->GetBehaviourParams();
	if (auto* wire = std::get_if<audio::WireMixBehaviourParams>(&params))
	{
		s.Mix.Mix = io::JamFile::LoopMix::MIX_WIRE;
		std::vector<unsigned long> chans;
		for (auto c : wire->Channels) chans.push_back(static_cast<unsigned long>(c));
		s.Mix.Params = chans;
	}
	else if (auto* pan = std::get_if<audio::PanMixBehaviourParams>(&params))
	{
		s.Mix.Mix = io::JamFile::LoopMix::MIX_PAN;
		std::vector<double> levels;
		for (auto l : pan->ChannelLevels) levels.push_back(static_cast<double>(l));
		s.Mix.Params = levels;
	}
	else
	{
		s.Mix.Mix = io::JamFile::LoopMix::MIX_PAN;
		s.Mix.Params = std::vector<double>{ 0.5, 0.5 };
	}

	return s;
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
	_bufferBank.Resize(length);

	for (auto i = 0u; i < length; i++)
	{
		_bufferBank[i] = buffer[i];
	}

	_loopLength = length - constants::MaxLoopFadeSamps;

	_UpdateLoopModel();

	std::cout << "-=-=- Loop LOADED " << _loopParams.Wav << std::endl;

	return true;
}

void Loop::Record()
{
	if (STATE_INACTIVE != _playState)
		return;

	Reset();

	_playState = STATE_RECORDING;
	_bufferBank.Resize(constants::MaxLoopFadeSamps);

	_monitorBufferBank.Resize(constants::MaxLoopFadeSamps);

	std::cout << "-=-=- Loop " << _playState << " - " << _loopParams.Id << std::endl;
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
	// recorded physical size. This prevents reads past the current BufferBank
	// length while still ignoring any physical tail beyond the logical loop.
	auto logicalBufSize = loopLength + constants::MaxLoopFadeSamps;
	auto effectiveBufSize = std::min(logicalBufSize, physBufSize);
	_playIndex = (effectiveBufSize > 0 && index >= effectiveBufSize) ? (effectiveBufSize - 1) : index;
	_loopLength = loopLength;

	auto isOverdubbing = (STATE_OVERDUBBING == _playState) || (STATE_PUNCHEDIN == _playState);
	auto recordState = isOverdubbing ? STATE_OVERDUBBINGRECORDING : STATE_PLAYINGRECORDING;
	auto playState = continueRecording ? recordState : STATE_PLAYING;
	_playState = loopLength > 0 ? playState : STATE_INACTIVE;

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
	if (STATE_INACTIVE != _playState)
		return;

	Reset();

	_playState = STATE_OVERDUBBING;
	_bufferBank.Resize(constants::MaxLoopFadeSamps);

	_monitorBufferBank.Resize(constants::MaxLoopFadeSamps);

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

unsigned long Loop::_LoopIndex() const
{
	if (constants::MaxLoopFadeSamps > _playIndex)
		return 0;

	return _playIndex - constants::MaxLoopFadeSamps;
}

void Loop::_UpdateLoopModel()
{
	auto isRecording = (STATE_RECORDING == _playState) ||
		(STATE_OVERDUBBING == _playState) ||
		(STATE_PUNCHEDIN == _playState);
	auto length = isRecording ? _writeIndex : _loopLength;
	auto offset = isRecording ? 0ul : constants::MaxLoopFadeSamps;

	auto radius = (float)_CalcDrawRadius(length);
	auto& bufBank = isRecording ? _monitorBufferBank : _bufferBank;
	_model->UpdateModel(bufBank, length, offset, radius);
	_vu->UpdateModel(radius);
}
