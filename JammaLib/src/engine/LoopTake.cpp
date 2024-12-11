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
using actions::JobAction;
using audio::AudioMixer;
using audio::AudioMixerParams;
using utils::Size2d;

const Size2d LoopTake::_Gap = { 6, 6 };

LoopTake::LoopTake(LoopTakeParams params) :
	GuiElement(params),
	MultiAudioSource(),
	_flipLoopBuffer(false),
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
	_loops({}),
	_backLoops({})
{
}

LoopTake::~LoopTake()
{
}

std::optional<std::shared_ptr<LoopTake>> LoopTake::FromFile(LoopTakeParams takeParams, io::JamFile::LoopTake takeStruct, std::wstring dir)
{
	auto take = std::make_shared<LoopTake>(takeParams);

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

void LoopTake::SetSize(utils::Size2d size)
{
	GuiElement::SetSize(size);

	ArrangeLoops();
}

unsigned int LoopTake::NumInputChannels() const
{
	return (unsigned int)_backLoops.size();
}

const std::shared_ptr<AudioSink> LoopTake::InputChannel(unsigned int channel)
{
	if (channel < _loops.size())
		return _loops[channel];

	return std::shared_ptr<AudioSink>();
}

void LoopTake::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int indexOffset,
	unsigned int numSamps)
{
	if (nullptr == dest)
		return;
	
	if (!dest->IsArmed())
		return;

	for (auto& loop : _loops)
		loop->OnPlay(dest, trigger, indexOffset, numSamps);
}

void LoopTake::EndMultiPlay(unsigned int numSamps)
{
	for (auto& loop : _loops)
		loop->EndMultiPlay(numSamps);
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
	bool updateIndex)
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

ActionResult LoopTake::OnAction(JobAction action)
{
	switch (action.JobActionType)
	{
	case JobAction::JOB_UPDATELOOPS:
	{
		UpdateLoops();

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

	return { false, "", "", actions::ACTIONRESULT_DEFAULT};
}

std::string LoopTake::Id() const
{
	return _id;
}

std::string LoopTake::SourceId() const
{
	return _sourceId;
}

LoopTake::LoopTakeSource LoopTake::SourceType() const
{
	return _sourceType;
}

unsigned long LoopTake::NumRecordedSamps() const
{
	return _recordedSampCount;
}

std::shared_ptr<Loop> LoopTake::AddLoop(unsigned int chan, std::string stationName)
{
	auto newNumLoops = (unsigned int)_loops.size() + 1;

	auto loopHeight = CalcLoopHeight(_sizeParams.Size.Height, newNumLoops);

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

	ArrangeLoops();

	_flipLoopBuffer = true;
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

void LoopTake::Mute()
{
	if (STATE_PLAYING != _state)
		return;

	_state = STATE_MUTED;

	for (auto& loop : _loops)
	{
		loop->Mute();
	}
}

void LoopTake::UnMute()
{
	if (STATE_MUTED != _state)
		return;

	_state = STATE_PLAYING;

	for (auto& loop : _loops)
	{
		loop->UnMute();
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

unsigned int LoopTake::CalcLoopHeight(unsigned int takeHeight, unsigned int numLoops)
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

void LoopTake::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);
	
	ArrangeLoops();
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

void LoopTake::ArrangeLoops()
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

void LoopTake::UpdateLoops()
{
	for (auto& loop : _loops)
	{
		loop->Update();
	}
}
