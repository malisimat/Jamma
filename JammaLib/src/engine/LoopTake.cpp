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
	_state(STATE_DEFAULT),
	_id(params.Id),
	_sourceId(""),
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

void LoopTake::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	unsigned int numSamps)
{
	for (auto& loop : _loops)
		loop->OnPlay(dest, numSamps);
}

void LoopTake::EndMultiPlay(unsigned int numSamps)
{
	for (auto& loop : _loops)
		loop->EndMultiPlay(numSamps);
}

// TODO: Remove method
void LoopTake::OnWrite(const std::shared_ptr<MultiAudioSource> src,
	unsigned int numSamps)
{
	for (auto& loop : _loops)
	{
		auto inChan = loop->InputChannel();
		src->OnPlayChannel(inChan, loop, numSamps);
	}
}

void LoopTake::OnWriteChannel(unsigned int channel,
	const std::shared_ptr<base::AudioSource> src,
	unsigned int numSamps)
{
	for (auto& loop : _loops)
	{
		if (loop->InputChannel() == channel)
			src->OnPlay(loop, numSamps);
	}
}

void LoopTake::EndMultiWrite(unsigned int numSamps, bool updateIndex)
{
	for (auto& loop : _loops)
		 loop->EndWrite(numSamps, updateIndex);

	if (STATE_PLAYINGRECORDING == _state)
	{
		_endRecordSampCount += numSamps;
		if (_endRecordSampCount > _endRecordSamps)
			_endRecordingCompleted = true;
	}

	if ((STATE_RECORDING == _state) ||
		(STATE_PLAYINGRECORDING == _state))
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

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;

		return res;
	}
	break;
	}

	return { false, "", actions::ACTIONRESULT_DEFAULT };
}

void LoopTake::OnPlayRaw(const std::shared_ptr<MultiAudioSink> dest,
	unsigned int delaySamps,
	unsigned int numSamps)
{
	auto loopNum = 0u;
	for (auto& loop : _loops)
	{
		loop->OnPlayRaw(dest, loopNum, delaySamps, numSamps);
		loopNum++;
	}
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

std::shared_ptr<Loop> LoopTake::AddLoop(unsigned int chan)
{
	auto newNumLoops = (unsigned int)_loops.size() + 1;

	auto loopHeight = CalcLoopHeight(_sizeParams.Size.Height, newNumLoops);

	audio::WireMixBehaviourParams wire;
	wire.Channels = { chan };
	auto mixerParams = Loop::GetMixerParams({ 110, loopHeight }, wire, chan);
	
	LoopParams loopParams;
	loopParams.Wav = "hh";
	loopParams.Id = "LP-" + utils::GetGuid();
	loopParams.TakeId = _id;
	loopParams.Channel = chan;
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

void LoopTake::Record(std::vector<unsigned int> channels)
{
	_state = STATE_RECORDING;

	_recordedSampCount = 0;
	_endRecordSampCount = 0;
	_endRecordSamps = 0;
	_backLoops.clear();

	for (auto chan : channels)
	{
		auto loop = AddLoop(chan);
		loop->Record();
	}

	//_flipLoopBuffer = true;
	_loopsNeedUpdating = true;
	_changesMade = true;
}

unsigned long LoopTake::NumRecordedSamps() const
{
	return _recordedSampCount;
}

void LoopTake::Play(unsigned long index,
	unsigned long loopLength,
	unsigned int endRecordSamps)
{
	_endRecordSampCount = 0;
	_endRecordSamps = endRecordSamps;

	for (auto& loop : _loops)
	{
		loop->Play(index, loopLength, endRecordSamps > 0);
	}

	auto playState = endRecordSamps > 0 ? STATE_PLAYINGRECORDING : STATE_PLAYING;
	_state = loopLength > 0 ? playState : STATE_DEFAULT;
}

void LoopTake::EndRecording()
{
	if (STATE_PLAYINGRECORDING == _state)
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

void LoopTake::Overdub()
{
	_state = STATE_OVERDUBBING;

	for (auto& loop : _loops)
	{
		loop->Overdub();
	}
}

void LoopTake::PunchIn()
{
	_state = STATE_PUNCHEDIN;

	for (auto& loop : _loops)
	{
		loop->PunchIn();
	}
}

void LoopTake::PunchOut()
{
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
