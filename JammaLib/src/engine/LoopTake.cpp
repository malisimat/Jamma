#include "LoopTake.h"

using namespace engine;
using base::AudioSink;
using base::AudioSource;
using base::DrawContext;
using base::Drawable;
using base::DrawableParams;
using engine::Trigger;
using resources::ResourceLib;
using actions::ActionResult;
using actions::TriggerAction;
using actions::GuiAction;
using actions::JobAction;
using audio::AudioMixer;
using audio::AudioMixerParams;
using audio::WireMixBehaviourParams;
using gui::GuiRackParams;
using utils::Size2d;

const Size2d LoopTake::_Gap = { 6, 6 };
const Size2d LoopTake::_ToggleSize = { 64, 32 };
const Size2d LoopTake::_ToggleGap = { 6, 6 };

LoopTake::LoopTake(LoopTakeParams params,
	AudioMixerParams mixerParams) :
	Jammable(params),
	MultiAudioSink(),
	_flipLoopBuffer(false),
	_loopsNeedUpdating(false),
	_endRecordingCompleted(false),
	_state(STATE_INACTIVE),
	_id(params.Id),
	_sourceId(""),
	_lastBufSize(constants::MaxBlockSize),
	_fadeSamps(params.FadeSamps),
	_sourceType(SOURCE_LOOPTAKE),
	_recordedSampCount(0),
	_endRecordSampCount(0),
	_endRecordSamps(0),
	_guiRack(nullptr),
	_masterMixer(nullptr),
	_loops(),
	_backLoops(),
	_audioMixers(),
	_backAudioMixers(),
	_audioBuffers(),
	_backAudioBuffers()
{
	_masterMixer = std::make_unique<AudioMixer>(mixerParams);
	_guiRack = std::make_shared<gui::GuiRack>(_GetRackParams(params.Size));

	_children.push_back(_guiRack);
}

LoopTake::~LoopTake()
{
}

std::optional<std::shared_ptr<LoopTake>> LoopTake::FromFile(LoopTakeParams takeParams, io::JamFile::LoopTake takeStruct, std::wstring dir)
{
	auto mixerParams = GetMixerParams({ 100,100 }, audio::WireMixBehaviourParams());
	auto take = std::make_shared<LoopTake>(takeParams, mixerParams);

	LoopParams loopParams;
	loopParams.Wav = "hh";

	for (auto loopStruct : takeStruct.Loops)
	{
		auto loop = Loop::FromFile(loopParams, loopStruct, dir);
		
		if (loop.has_value())
			take->AddLoop(loop.value());
	}

	return take;
}

AudioMixerParams LoopTake::GetMixerParams(utils::Size2d loopSize,
	audio::BehaviourParams behaviour)
{
	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, loopSize.Height };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = behaviour;

	return mixerParams;
}

void LoopTake::SetSize(utils::Size2d size)
{
	auto mixerParams = GetMixerParams(size,
		WireMixBehaviourParams());

	for (auto& mixer : _audioMixers)
	{
		mixer->SetSize(mixerParams.Size);
	}

	GuiElement::SetSize(size);

	_ArrangeChildren();
}

unsigned int LoopTake::NumInputChannels(Audible::AudioSourceType source) const
{
	return (_changesMade && _flipLoopBuffer) ?
		(unsigned int)_backLoops.size() :
		(unsigned int)_loops.size();
}

unsigned int LoopTake::NumOutputChannels(Audible::AudioSourceType source) const
{
	switch (source)
	{
	case Audible::AUDIOSOURCE_ADC:
	case Audible::AUDIOSOURCE_MONITOR:
	case Audible::AUDIOSOURCE_BOUNCE:
		return (_changesMade && _flipLoopBuffer) ?
			(unsigned int)_backLoops.size() :
			(unsigned int)_loops.size();
	case Audible::AUDIOSOURCE_LOOPS:
	case Audible::AUDIOSOURCE_MIXER:
		return NumBusChannels();
	}

	return 0u;
}

unsigned int LoopTake::NumBusChannels() const
{
	if (nullptr != _guiRack)
		return _guiRack->NumOutputChannels();

	return 0u;
}

void LoopTake::Zero(unsigned int numSamps,
	Audible::AudioSourceType source)
{
	for (auto chan = 0u; chan < NumInputChannels(source); chan++)
	{
		auto channel = _InputChannel(chan, source);
		channel->Zero(numSamps);
	}

	for (auto& loop : _loops)
		loop->Zero(numSamps);
}

// Only called when outputting to DAC
void LoopTake::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int indexOffset,
	unsigned int numSamps)
{
	if (nullptr == dest)
		return;
	
	if (!dest->IsArmed())
		return;

	auto ptr = Sharable::shared_from_this();

	for (const auto& loop : _loops)
		loop->OnPlay(std::dynamic_pointer_cast<MultiAudioSink>(ptr),
			trigger,
			indexOffset,
			numSamps);

	for (const auto& buf : _audioBuffers)
	{
		auto playIndex = buf->Delay(numSamps);
		for (auto samp = 0u; samp < numSamps; samp++)
		{
			for (const auto& mixer : _audioMixers)
			{
				mixer->OnPlay(dest, (*buf)[samp + playIndex], samp);
			}
		}
	}
}

void LoopTake::EndMultiPlay(unsigned int numSamps)
{
	for (auto& loop : _loops)
		loop->EndMultiPlay(numSamps);

	for (auto& buffer : _audioBuffers)
	{
		buffer->EndWrite(numSamps, true);
		buffer->EndPlay(numSamps);
	}
}

bool LoopTake::IsArmed() const
{
	return (STATE_RECORDING == _state) ||
		(STATE_PLAYINGRECORDING == _state) ||
		(STATE_OVERDUBBING == _state) ||
		(STATE_PUNCHEDIN == _state) ||
		(STATE_OVERDUBBINGRECORDING == _state);
}

void LoopTake::EndMultiWrite(unsigned int numSamps,
	bool updateIndex,
	Audible::AudioSourceType source)
{
	for (auto& loop : _loops)
		 loop->EndWrite(numSamps, updateIndex);

	auto isRecording = IsArmed();
	auto isEndRecording = (STATE_PLAYINGRECORDING == _state) ||
		(STATE_OVERDUBBINGRECORDING == _state);

	if (isEndRecording)
	{
		_endRecordSampCount += numSamps;
		if (_endRecordSampCount > _endRecordSamps)
			_endRecordingCompleted = true;
	}

	if (isRecording)
	{
		_recordedSampCount += numSamps;
		_loopsNeedUpdating = true;
		_changesMade = true;
	}
}

ActionResult LoopTake::OnAction(GuiAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	if (res.IsEaten)
		return res;

	if (auto chans = std::get_if<GuiAction::GuiConnections>(&action.Data))
	{
		for (auto chan = 0u; chan < _audioMixers.size(); chan++)
		{
			std::vector<std::pair<unsigned int, unsigned int>> chanConnections;
			std::copy_if(chans->Connections.begin(), chans->Connections.end(), std::back_inserter(chanConnections),
				[chan](const std::pair<unsigned int, unsigned int>& pair) {
					return pair.first == chan;
				});

			std::vector<unsigned int> secondElements;
			std::transform(chanConnections.begin(), chanConnections.end(), std::back_inserter(secondElements),
				[](const std::pair<unsigned int, unsigned int>& pair) {
					return pair.second;
				});
			_audioMixers[0]->SetChannels(secondElements);
		}
	}
	else if (auto d = std::get_if<GuiAction::GuiDouble>(&action.Data))
	{
		if (0 == action.Index)
			_masterMixer->OnAction(action);
		else if ((action.Index - 1) < _audioMixers.size())
		{
			//_audioMixers[action.Index - 1]->OnAction(action);
			_loops[action.Index - 1]->SetMixerLevel(d->Value);
		}
	}

	return res;
}

ActionResult LoopTake::OnAction(JobAction action)
{
	switch (action.JobActionType)
	{
	case JobAction::JOB_UPDATELOOPS:
	{
		_UpdateLoops();

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;

		return res;
	}
	break;
	case JobAction::JOB_ENDRECORDING:
	{
		EndRecording();
		std::cout << "Ended recording" << std::endl;

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;

		return res;
	}
	break;
	}

	return ActionResult::NoAction();
}

std::string LoopTake::Id() const
{
	return _id;
}

std::string LoopTake::SourceId() const
{
	return _sourceId;
}

LoopTake::LoopTakeSource LoopTake::TakeSourceType() const
{
	return _sourceType;
}

LoopTake::LoopTakeState LoopTake::TakeState() const
{
	return _state;
}

unsigned long LoopTake::NumRecordedSamps() const
{
	return _recordedSampCount;
}

std::shared_ptr<Loop> LoopTake::AddLoop(unsigned int chan, std::string stationName)
{
	auto newNumLoops = (unsigned int)_loops.size() + 1;

	auto loopHeight = _CalcLoopHeight(_sizeParams.Size.Height, newNumLoops);

	audio::WireMixBehaviourParams wire;
	wire.Channels = { chan };
	auto mixerParams = Loop::GetMixerParams({ 110, loopHeight }, wire);
	
	LoopParams loopParams;
	loopParams.Wav = stationName;
	loopParams.Id = stationName + "-" + utils::GetGuid();
	loopParams.TakeId = _id;
	loopParams.Channel = chan;
	loopParams.FadeSamps = _fadeSamps;
	auto loop = std::make_shared<Loop>(loopParams, mixerParams);
	AddLoop(loop);

	return loop;
}

void LoopTake::AddLoop(std::shared_ptr<Loop> loop)
{
	_backLoops.push_back(loop);
	_backAudioBuffers.push_back(std::make_shared<audio::AudioBuffer>(_lastBufSize));
	
	WireMixBehaviourParams wireParams;
	if (_backAudioBuffers.size() <= NumBusChannels())
		wireParams.Channels.push_back((unsigned int)(_backAudioBuffers.size() - 1));

	auto mixerParams = LoopTake::GetMixerParams(_guiParams.Size, wireParams);
	auto mixer = std::make_shared<audio::AudioMixer>(mixerParams);
	mixer->SetUnmutedLevel(1.0);
	_backAudioMixers.push_back(mixer);

	_children.push_back(loop);

	Init();

	_ArrangeChildren();

	_flipLoopBuffer = true;
	_changesMade = true;

	_guiRack->SetNumInputChannels((unsigned int)_backLoops.size());
	_guiRack->AddRoute((unsigned int)_backLoops.size() - 1, (unsigned int)_backLoops.size() - 1);
}

void LoopTake::SetMixerLevel(unsigned int chan, double level)
{
	if (chan < _audioMixers.size())
		_audioMixers[chan]->SetUnmutedLevel(level);
}

void LoopTake::SetupBuffers(unsigned int bufSize)
{
	_lastBufSize = bufSize;

	auto& buffers = (_flipLoopBuffer && _changesMade) ?
		_backAudioBuffers :
		_audioBuffers;

	for (auto& buf : buffers)
	{
		buf->SetSize(bufSize);
	}
}

void LoopTake::SetNumBusChannels(unsigned int chans)
{
	for (auto& mixer : _audioMixers)
	{
		mixer->SetMaxChannels(chans);
	}

	_masterMixer->SetMaxChannels(chans);
	_guiRack->SetNumOutputChannels(chans);
}

void LoopTake::Record(std::vector<unsigned int> channels, std::string stationName)
{
	if (STATE_INACTIVE != _state)
		return;

	_state = STATE_RECORDING;

	_recordedSampCount = 0;
	_endRecordSampCount = 0;
	_endRecordSamps = 0;
	_backLoops.clear();

	for (auto chan : channels)
	{
		auto loop = AddLoop(chan, stationName);
		loop->Record();
	}

	//_flipLoopBuffer = true;
	_loopsNeedUpdating = true;
	_changesMade = true;
}

void LoopTake::Play(unsigned long index,
	unsigned long loopLength,
	unsigned int endRecordSamps)
{
	if ((STATE_RECORDING != _state) &&
		(STATE_OVERDUBBING != _state) &&
		(STATE_PUNCHEDIN != _state))
		return;

	_endRecordSampCount = 0;
	_endRecordSamps = endRecordSamps;

	for (auto& loop : _loops)
	{
		loop->Play(index, loopLength, endRecordSamps > 0);
	}

	auto isOverdubbing = (STATE_OVERDUBBING == _state) || (STATE_PUNCHEDIN == _state);
	auto recordState = isOverdubbing ? STATE_OVERDUBBINGRECORDING : STATE_PLAYINGRECORDING;
	auto playState = endRecordSamps > 0 ? recordState : STATE_PLAYING;
	_state = loopLength > 0 ? playState : STATE_INACTIVE;
}

bool LoopTake::Select()
{
	auto isNewState = GuiElement::Select();

	if (isNewState)
	{
		for (auto& loop : _loops)
		{
			loop->Select();
		}
	}

	return isNewState;
}

bool LoopTake::DeSelect()
{
	auto isNewState = GuiElement::DeSelect();

	if (isNewState)
	{
		for (auto& loop : _loops)
		{
			loop->DeSelect();
		}
	}

	return isNewState;
}

bool LoopTake::Mute()
{
	auto isNewState = Tweakable::Mute();

	if (isNewState)
	{
		for (auto& loop : _loops)
		{
			loop->Mute();
		}
	}

	return isNewState;
}

bool LoopTake::UnMute()
{
	auto isNewState = Tweakable::UnMute();

	if (isNewState)
	{
		for (auto& loop : _loops)
		{
			loop->UnMute();
		}
	}

	return isNewState;
}

void LoopTake::SetPickingFromState(EditMode mode, bool flipState)
{
	switch (mode)
	{
	case EDIT_SELECT:
		_isPicking3d = flipState ?
			!_isSelected :
			_isSelected;
		break;
	case EDIT_MUTE:
		_isPicking3d = flipState ? 
			!(TWEAKSTATE_MUTED & _tweakState)
			: (TWEAKSTATE_MUTED & _tweakState);
		break;
	}

	GuiElement::SetPicking3d(_isPicking3d);
}

void LoopTake::SetStateFromPicking(EditMode mode, bool flipState)
{
	bool isPicking = flipState ?
		!_isPicking3d :
		_isPicking3d;

	switch (mode)
	{
	case EDIT_SELECT:
		if (isPicking)
			Select();
		else
			DeSelect();
		break;
	case EDIT_MUTE:
		if (isPicking)
			Mute();
		else
			UnMute();
		break;
	}
}

void LoopTake::EndRecording()
{
	if ((STATE_PLAYINGRECORDING != _state) &&
		(STATE_OVERDUBBINGRECORDING != _state))
		return;

	_state = STATE_PLAYING;

	for (auto& loop : _loops)
	{
		loop->EndRecording();
	}
}

void LoopTake::Ditch()
{
	_recordedSampCount = 0;
	_endRecordSampCount = 0;
	_endRecordSamps = 0;

	for (auto& loop : _loops)
	{
		loop->Ditch();
	}

	Zero(_lastBufSize, Audible::AUDIOSOURCE_LOOPS);

	_loops.clear();
}

void LoopTake::Overdub(std::vector<unsigned int> channels, std::string stationName)
{
	if (STATE_INACTIVE != _state)
		return;

	_state = STATE_OVERDUBBING;

	_recordedSampCount = 0;
	_endRecordSampCount = 0;
	_endRecordSamps = 0;
	_backLoops.clear();

	for (auto chan : channels)
	{
		auto loop = AddLoop(chan, stationName);
		loop->Overdub();
	}

	_loopsNeedUpdating = true;
	_changesMade = true;
}

void LoopTake::PunchIn()
{
	if (STATE_OVERDUBBING != _state)
		return;

	_state = STATE_PUNCHEDIN;

	for (auto& loop : _loops)
	{
		loop->PunchIn();
	}
}

void LoopTake::PunchOut()
{
	if (STATE_PUNCHEDIN != _state)
		return;

	_state = STATE_OVERDUBBING;

	for (auto& loop : _loops)
	{
		loop->PunchOut();
	}
}

unsigned int LoopTake::_CalcLoopHeight(unsigned int takeHeight, unsigned int numLoops)
{
	if (0 == numLoops)
		return 0;

	auto minHeight = 5;
	int totalGap = (numLoops + 1) * (int)_Gap.Width;
	int height = (((int)takeHeight) - totalGap) / (int)numLoops;

	if (height < minHeight)
		return minHeight;

	return height;
}

void LoopTake::_InitReceivers()
{
	_guiRack->SetReceiver(ActionReceiver::shared_from_this());
}

void LoopTake::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);
	
	_ArrangeChildren();
}

std::vector<JobAction> LoopTake::_CommitChanges()
{
	if (_flipLoopBuffer)
	{
		_flipLoopBuffer = false;

		// Remove and add any children
		// (difference between back and front LoopTake buffer)
		std::vector<std::shared_ptr<Loop>> toAdd;
		std::vector<std::shared_ptr<Loop>> toRemove;
		std::copy_if(_backLoops.begin(), _backLoops.end(), std::back_inserter(toAdd), [&](const std::shared_ptr<Loop>& loop) { return (std::find(_loops.begin(), _loops.end(), loop) == _loops.end()); });
		std::copy_if(_loops.begin(), _loops.end(), std::back_inserter(toRemove), [&](const std::shared_ptr<Loop>& loop) { return (std::find(_backLoops.begin(), _backLoops.end(), loop) == _backLoops.end()); });

		for (auto& loop : toAdd)
		{
			loop->SetParent(GuiElement::shared_from_this());
			loop->Init();
			_children.push_back(loop);
		}

		for (auto& loop : toRemove)
		{
			auto child = std::find(_children.begin(), _children.end(), loop);
			if (_children.end() != child)
				_children.erase(child);
		}

		_loops = _backLoops; // TODO: Undo?
		_audioBuffers = _backAudioBuffers;
		_audioMixers = _backAudioMixers;

		_guiRack->SetNumInputChannels((unsigned int)_loops.size());
	}

	std::vector<JobAction> jobs;

	if (_loopsNeedUpdating)
	{
		_loopsNeedUpdating = false;

		JobAction job;
		job.JobActionType = JobAction::JOB_UPDATELOOPS;
		job.SourceId = Id();
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(job);
	}

	if (_endRecordingCompleted)
	{
		_endRecordingCompleted = false;

		JobAction job;
		job.JobActionType = JobAction::JOB_ENDRECORDING;
		job.SourceId = Id();
		job.Receiver = ActionReceiver::shared_from_this();
		jobs.push_back(job);
	}

	GuiElement::_CommitChanges();

	return jobs;
}

const std::shared_ptr<AudioSink> LoopTake::_InputChannel(unsigned int channel,
	Audible::AudioSourceType source)
{
	switch (source)
	{
	case Audible::AUDIOSOURCE_ADC:
	case Audible::AUDIOSOURCE_MONITOR:
	case Audible::AUDIOSOURCE_BOUNCE:
		if (channel < _loops.size())
			return _loops[channel];

		break;
	case Audible::AUDIOSOURCE_LOOPS:
	case Audible::AUDIOSOURCE_MIXER:
		if (channel < _audioBuffers.size())
			return _audioBuffers[channel];

		break;
	}

	return nullptr;
}

void LoopTake::_ArrangeChildren()
{
	auto numLoops = (unsigned int)_backLoops.size();

	if (0 == numLoops)
		return;

	utils::Size2d loopSize = { _sizeParams.Size.Width - (2 * _Gap.Width), _sizeParams.Size.Height - (2 * _Gap.Height) };

	auto loopCount = 0u;
	auto dScale = 0.1;
	auto dTotalScale = 0.4 / ((double)numLoops);

	for (auto& loop : _backLoops)
	{
		loop->SetPosition({ (int)_Gap.Width + ((int)loopSize.Width * (int)loop->LoopChannel()), (int)_Gap.Height });
		loop->SetSize(loopSize);
		loop->SetModelPosition({ 0.0f, 0.0f, 0.0f });
		loop->SetModelScale(1.0 + (loopCount * dScale) - (dTotalScale * 0.5));

		std::cout << "[Arranging loop " << loop->Id() << "] Scale: " << 1.0 + (loopCount * dScale) - (dTotalScale * 0.5) << ", Position: " << loop->Position().X << std::endl;

		loopCount++;
	}
}

GuiRackParams LoopTake::_GetRackParams(utils::Size2d size)
{
	GuiRackParams rackParams;
	rackParams.Position = { 0, 0 };
	rackParams.Size = size;
	rackParams.MinSize = rackParams.Size;
	rackParams.NumInputChannels = NumInputChannels(Audible::AUDIOSOURCE_LOOPS);
	rackParams.NumOutputChannels = NumOutputChannels(Audible::AUDIOSOURCE_LOOPS);
	rackParams.InitLevel = 1.0;
	rackParams.InitState = gui::GuiRackParams::RACK_MASTER;

	return rackParams;
}

void LoopTake::_UpdateLoops()
{
	for (auto& loop : _loops)
	{
		loop->Update();
	}
}
