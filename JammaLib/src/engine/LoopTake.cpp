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
using gui::GuiRouterParams;
using utils::Size2d;

const Size2d LoopTake::_Gap = { 6, 6 };

LoopTake::LoopTake(LoopTakeParams params,
	AudioMixerParams mixerParams) :
	GuiElement(params),
	Tweakable(params),
	MultiAudioSource(),
	_flipLoopBuffer(false),
	_flipAudioBuffer(false),
	_loopsNeedUpdating(false),
	_endRecordingCompleted(false),
	_state(STATE_INACTIVE),
	_id(params.Id),
	_sourceId(""),
	_fadeSamps(params.FadeSamps),
	_sourceType(SOURCE_LOOPTAKE),
	_recordedSampCount(0),
	_endRecordSampCount(0),
	_endRecordSamps(0),
	_mixer(nullptr),
	_router(nullptr),
	_loops(),
	_backLoops(),
	_audioBuffers(),
	_backAudioBuffers()
{
	_mixer = std::make_unique<AudioMixer>(mixerParams);
	_router = std::make_unique<gui::GuiRouter>(_GetRouterParams(params.Size),
		8,
		8,
		false,
		false);

	_children.push_back(_mixer);
	_children.push_back(_router);
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

	_mixer->SetSize(mixerParams.Size);

	GuiElement::SetSize(size);

	_ArrangeLoops();
}

unsigned int LoopTake::NumInputChannels() const
{
	return (unsigned int)_backLoops.size();
}

unsigned int LoopTake::NumOutputChannels() const
{
	return _changesMade ?
		(unsigned int)_backAudioBuffers.size() :
		(unsigned int)_audioBuffers.size();
}

void LoopTake::Zero(unsigned int numSamps,
	Audible::AudioSourceType source)
{
	for (auto chan = 0u; chan < NumInputChannels(); chan++)
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

	for (auto& loop : _loops)
		loop->OnPlay(std::dynamic_pointer_cast<MultiAudioSink>(ptr),
			trigger,
			indexOffset,
			numSamps);

	for (auto& buf : _audioBuffers)
	{
		unsigned int i = 0;
		auto bufIter = buf->Delay(numSamps);

		while ((bufIter != buf->End()) && (i < numSamps))
		{
			_mixer->OnPlay(dest, *bufIter++, i++);
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

	if (res.IsEaten)
		return res;

	if (auto chans = std::get_if<GuiAction::GuiConnections>(&action.Data)) {
		_mixer->SetChannels(chans->Connections);
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
	_children.push_back(loop);
	Init();

	_ArrangeLoops();

	_flipLoopBuffer = true;
	_changesMade = true;
}

void LoopTake::SetupBuffers(unsigned int chans, unsigned int bufSize)
{
	WireMixBehaviourParams wireParams;

	_backAudioBuffers.clear();

	for (unsigned int i = 0; i < chans; i++)
	{
		_backAudioBuffers.push_back(std::make_shared<audio::AudioBuffer>(bufSize));
	}

	_flipAudioBuffer = true;
	_changesMade = true;
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

	if (!_audioBuffers.empty())
		Zero(_audioBuffers[0]->BufSize(), Audible::AUDIOSOURCE_LOOPS);

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

	//_flipLoopBuffer = true;
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
	_router->SetReceiver(ActionReceiver::shared_from_this());
}

void LoopTake::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);
	
	_ArrangeLoops();
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
	}

	if (_flipAudioBuffer)
	{
		_audioBuffers = _backAudioBuffers;
		_flipAudioBuffer = false;
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
	case Audible::AUDIOSOURCE_BOUNCE:
	case Audible::AUDIOSOURCE_MONITOR:
		if (channel < _loops.size())
			return _loops[channel];

		break;
	case Audible::AUDIOSOURCE_LOOPS:
		if (channel < _audioBuffers.size())
			return _audioBuffers[channel];

		break;
	}

	return nullptr;
}

gui::GuiRouterParams LoopTake::_GetRouterParams(utils::Size2d size)
{
	GuiRouterParams routerParams;

	routerParams.Position = { (int)_Gap.Width, (int)_Gap.Height };
	routerParams.Size = { size.Width - (2 * _Gap.Width), size.Height - (2 * _Gap.Height) };
	routerParams.MinSize = routerParams.Size;
	routerParams.Texture = "router";
	routerParams.PinTexture = "";
	routerParams.LinkTexture = "";
	routerParams.DeviceInactiveTexture = "router";
	routerParams.DeviceActiveTexture = "router_inactive";
	routerParams.ChannelInactiveTexture = "router";
	routerParams.ChannelActiveTexture = "router_inactive";
	routerParams.OverTexture = "router_over";
	routerParams.DownTexture = "router_down";
	routerParams.HighlightTexture = "router_over";
	routerParams.LineShader = "colour";

	return routerParams;
}

void LoopTake::_ArrangeLoops()
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
		loop->SetPosition({ (int)_Gap.Width + ((int)loopSize.Width * (int)loop->LoopChannel()), (int)_Gap.Height});
		loop->SetSize(loopSize);
		loop->SetModelPosition({ 0.0f, 0.0f, 0.0f });
		loop->SetModelScale(1.0 + (loopCount * dScale) - (dTotalScale * 0.5));

		std::cout << "[Arranging loop " << loop->Id() << "] Scale: " << 1.0 + (loopCount * dScale) - (dTotalScale * 0.5) << ", Position: " << loop->Position().X << std::endl;

		loopCount++;
	}
}

void LoopTake::_UpdateLoops()
{
	for (auto& loop : _loops)
	{
		loop->Update();
	}
}
