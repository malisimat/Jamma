#include "Loop.h"

using namespace base;
using namespace engine;
using namespace resources;
using base::AudioSink;
using base::MultiAudioSource;
using audio::AudioMixer;
using audio::AudioMixerParams;
using audio::PanMixBehaviour;
using gui::GuiSliderParams;
using gui::GuiModel;
using gui::GuiModelParams;
using graphics::GlDrawContext;
using actions::ActionResult;
using actions::JobAction;

Loop::Loop(LoopParams loopParams,
	audio::AudioMixerParams mixerParams) :
	GuiElement(loopParams),
	AudioSink(),
	_modelNeedsUpdating(false),
	_playIndex(0),
	_lastPeak(0.0f),
	_pitch(1.0),
	_loopLength(0),
	_state(STATE_PLAYING),
	_loopParams(loopParams),
	_mixer(nullptr),
	_model(nullptr),
	_buffer({}),
	_backBuffer({})
{
	_mixer = std::make_unique<AudioMixer>(mixerParams);

	LoopModelParams modelParams;
	modelParams.Size = { 12, 14 };
	modelParams.ModelScale = 1.0f;
	modelParams.ModelTexture = "grid";
	modelParams.ModelShader = "texture_shaded";
	_model = std::make_shared<LoopModel>(modelParams);

	VuParams vuParams;
	vuParams.Size = { 12, 24 };
	vuParams.ModelScale = 1.0f;
	vuParams.ModelTexture = "blue";
	vuParams.ModelShader = "vu"; // TODO: define vu shader
	vuParams.LedHeight = 1.5f;
	_vu = std::make_shared<VU>(vuParams);

	_children.push_back(_mixer);
	_children.push_back(_model);
	_children.push_back(_vu);
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

	auto mixerParams = GetMixerParams(loopParams.Size, behaviour);

	loopParams.Wav = utils::EncodeUtf8(dir) + "/" + loopStruct.Name;
	auto loop = std::make_shared<Loop>(loopParams, mixerParams);

	loop->Load(io::WavReadWriter());
	loop->Play(loopStruct.MasterLoopCount, loopStruct.Length, 0);

	return loop;
}

audio::AudioMixerParams Loop::GetMixerParams(utils::Size2d loopSize,
	audio::BehaviourParams behaviour)
{
	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, loopSize.Height };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = behaviour;

	return mixerParams;
}

utils::Position2d Loop::Position() const
{
	auto pos = ModelPosition();
	return { (int)round(pos.X), (int)round(pos.Y) };
}

void Loop::SetSize(utils::Size2d size)
{
	auto mixerParams = GetMixerParams(size, audio::WireMixBehaviourParams());

	_mixer->SetSize(mixerParams.Size);

	GuiElement::SetSize(size);
}

void Loop::Draw3d(DrawContext& ctx)
{
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	auto pos = ModelPosition();
	auto scale = ModelScale();

	auto index = STATE_RECORDING == _state ?
		_writeIndex :
		_playIndex;
	index = index > constants::MaxLoopFadeSamps ? index - constants::MaxLoopFadeSamps : index;

	auto frac = _loopLength == 0 ? 0.0 : 1.0 - std::max(0.0, std::min(1.0, ((double)(index % _loopLength)) / ((double)_loopLength)));
	_model->SetLoopIndexFrac(frac);

	_modelScreenPos = glCtx.ProjectScreen(pos);
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0), glm::vec3(scale, scale + _mixer->Level(), scale)));
	
	for (auto& child : _children)
		child->Draw3d(ctx);

	glCtx.PopMvp();
	glCtx.PopMvp();
}

int Loop::OnWrite(float samp, int indexOffset)
{
	return OnOverwrite(samp, indexOffset);
}

int Loop::OnOverwrite(float samp, int indexOffset)
{
	if ((STATE_RECORDING != _state) &&
		(STATE_PLAYINGRECORDING != _state) &&
		(STATE_OVERDUBBING != _state) &&
		(STATE_PUNCHEDIN != _state))
		return indexOffset;

	auto peak = std::abs(samp);
	if (STATE_RECORDING == _state)
	{
		if (peak > _lastPeak)
			_lastPeak = peak;
	}

	auto bufSize = (unsigned long)_buffer.size();

	if (bufSize <= (_writeIndex + indexOffset))
	{
		if (bufSize >= constants::MaxLoopBufferSize)
			return indexOffset;
		else
		{
			auto newBufSize = bufSize * 2;
			if (newBufSize > constants::MaxLoopBufferSize)
				newBufSize = constants::MaxLoopBufferSize;

			//_buffer.resize(newBufSize);
		}
	}
	else
		_buffer[_writeIndex + indexOffset] = samp;

	return indexOffset + 1;
}

void Loop::EndWrite(unsigned int numSamps, bool updateIndex)
{
	// Only update if currently recording
	if ((STATE_RECORDING != _state) &&
		(STATE_PLAYINGRECORDING != _state) &&
		(STATE_OVERDUBBING != _state) &&
		(STATE_PUNCHEDIN != _state))
		return;

	if (!updateIndex)
		return;

	_writeIndex += numSamps;

	if (STATE_PLAYINGRECORDING == _state)
	{
		_endRecordSampCount += numSamps;

		if (_endRecordSampCount > _endRecordSamps)
			_endRecordingCompleted = true;
	}

	//_loopLength = _writeIndex > constants::MaxLoopFadeSamps ? _writeIndex - constants::MaxLoopFadeSamps : _writeIndex;

	_modelNeedsUpdating = true;
	_changesMade = true;
}

void Loop::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	unsigned int numSamps)
{
	// Mixer will stereo spread the mono wav
	// and adjust level
	if (0 == _loopLength)
		return;

	if ((STATE_PLAYING != _state) && (STATE_PLAYINGRECORDING != _state))
		return;

	auto index = _playIndex;
	auto bufSize = _loopLength + constants::MaxLoopFadeSamps;
	while (index >= bufSize)
		index -= _loopLength;

	auto peak = 0.0f;

	for (auto i = 0u; i < numSamps; i++)
	{
		if (index < _buffer.size())
		{
			_mixer->OnPlay(dest, _buffer[index], i);

			if (std::abs(_buffer[index]) > peak)
				peak = std::abs(_buffer[index]);
		}

		index++;
		if (index >= bufSize)
			index -= _loopLength;
	}

	_lastPeak = peak;
}

void Loop::EndMultiPlay(unsigned int numSamps)
{
	if ((STATE_PLAYING != _state) && (STATE_PLAYINGRECORDING != _state))
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

	_vu->SetValue(_lastPeak, numSamps);
}

void Loop::OnPlayRaw(const std::shared_ptr<base::MultiAudioSink> dest,
	unsigned int channel,
	unsigned int delaySamps,
	unsigned int numSamps)
{
	// Mixer will stereo spread the mono wav
	// and adjust level
	if (0 == _loopLength)
		return;

	auto index = _playIndex + delaySamps;
	auto bufSize = _loopLength + constants::MaxLoopFadeSamps;
	while (index >= bufSize)
		index -= _loopLength;

	for (auto i = 0u; i < numSamps; i++)
	{
		dest->OnWriteChannel(channel, _buffer[index], i);

		index++;
		if (index >= bufSize)
			index -= _loopLength;
	}
}

unsigned int Loop::InputChannel() const
{
	return _mixer->InputChannel();
}

void Loop::SetInputChannel(unsigned int channel)
{
	_mixer->SetInputChannel(channel);
}

std::string Loop::Id() const
{
	return _loopParams.Id;
}

bool Loop::Load(const io::WavReadWriter& readWriter)
{
	auto loadOpt = readWriter.Read(utils::DecodeUtf8(_loopParams.Wav), constants::MaxLoopBufferSize);

	if (!loadOpt.has_value())
		return false;

	auto [buffer, sampleRate, bitDepth] = loadOpt.value();

	_loopLength = 0;
	_buffer.clear();

	auto length = (unsigned long)buffer.size();
	_buffer = std::vector<float>(length);

	for (auto i = 0u; i < length; i++)
	{
		_buffer[i] = buffer[i];
	}

	_loopLength = length - constants::MaxLoopFadeSamps;

	UpdateLoopModel();

	return true;
}

void Loop::Record()
{
	Reset();
	_state = STATE_RECORDING;
	_buffer = std::vector<float>(constants::MaxLoopFadeSamps+90000);

	_changesMade = true;
}

void Loop::Play(unsigned long index,
	unsigned long loopLength,
	unsigned int endRecordSamps)
{
	auto bufSize = (unsigned int)_buffer.size();

	if (0 == bufSize)
	{
		Reset();
		return;
	}

	_playIndex = (index + constants::MaxLoopFadeSamps) >= bufSize ? (bufSize-1) : index + constants::MaxLoopFadeSamps;
	_loopLength = loopLength;
	_endRecordSampCount = 0;
	_endRecordSamps = endRecordSamps;

	auto playState = endRecordSamps > 0 ? STATE_PLAYINGRECORDING : STATE_PLAYING;
	_state = loopLength > 0 ? playState : STATE_INACTIVE;
}

void Loop::EndRecording()
{
	if (STATE_PLAYINGRECORDING == _state)
		_state = STATE_PLAYING;
}

void Loop::Ditch()
{
	Reset();
	_buffer = std::vector<float>(constants::MaxLoopFadeSamps); // TODO: backbuffer?
}

void Loop::Overdub()
{
	_state = STATE_OVERDUBBING;
}

void Loop::PunchIn()
{
	_state = STATE_PUNCHEDIN;
}

void Loop::PunchOut()
{
	_state = STATE_OVERDUBBING;
}

std::vector<JobAction> Loop::_CommitChanges()
{
	if (_modelNeedsUpdating)
	{
		_modelNeedsUpdating = false;

		JobAction job;
		job.JobActionType = JobAction::JOB_RENDERWAVE;
		job.SourceId = Id();
		job.Receiver = ActionReceiver::shared_from_this();
		return { job };
	}

	if (_endRecordingCompleted)
	{
		_endRecordingCompleted = false;

		JobAction job;
		job.JobActionType = JobAction::JOB_ENDRECORDING;
		job.SourceId = Id();
		job.Receiver = ActionReceiver::shared_from_this();
		return { job };
	}

	return {};
}

ActionResult Loop::OnAction(JobAction action)
{
	if (JobAction::JOB_RENDERWAVE == action.JobActionType)
	{
		UpdateLoopModel();
		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;

		return res;
	}

	if (JobAction::JOB_ENDRECORDING == action.JobActionType)
	{
		EndRecording();

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;

		return res;
	}

	return { false, "", actions::ACTIONRESULT_DEFAULT };
}

void Loop::Reset()
{
	_writeIndex = 0;
	_playIndex = 0;
	_loopLength = 0;
	_state = STATE_INACTIVE;
	_changesMade = true;
}

unsigned long Loop::LoopIndex() const
{
	if (constants::MaxLoopFadeSamps > _playIndex)
		return 0;

	return _playIndex - constants::MaxLoopFadeSamps;
}

double Loop::CalcDrawRadius(unsigned long loopLength)
{
	auto minRadius = 100.0;
	auto maxRadius = 400.0;
	auto radius = 70.0 * log(loopLength) - 600;

	return std::clamp(radius, minRadius, maxRadius);
}

void Loop::UpdateLoopModel()
{
	auto length = STATE_RECORDING == _state ? _writeIndex : _loopLength;

	auto radius = (float)CalcDrawRadius(length);
	_model->UpdateModel(_buffer, length, radius);
	_vu->UpdateModel(radius);

	_modelNeedsUpdating = false;
}
