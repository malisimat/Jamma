#include "Station.h"

using namespace engine;
using base::MultiAudioSource;
using base::AudioSink;
using base::GuiElement;
using base::GuiElementParams;
using base::DrawContext;
using resources::ResourceLib;
using actions::ActionResult;
using actions::KeyAction;
using actions::TouchAction;
using actions::TriggerAction;
using actions::JobAction;
using gui::GuiSliderParams;
using audio::AudioMixer;
using audio::AudioMixerParams;
using audio::WireMixBehaviourParams;
using utils::Size2d;

const utils::Size2d Station::_Gap = { 5, 5 };

Station::Station(StationParams params) :
	GuiElement(params),
	MultiAudioSource(),
	_name(params.Name),
	_fadeSamps(params.FadeSamps),
	_clock(std::shared_ptr<Timer>()),
	_loopTakes(),
	_triggers({})
{
}

Station::~Station()
{
}

std::optional<std::shared_ptr<Station>> Station::FromFile(StationParams stationParams,
	io::JamFile::Station stationStruct,
	std::wstring dir)
{
	stationParams.Name = stationStruct.Name;
	auto station = std::make_shared<Station>(stationParams);

	auto numTakes = (unsigned int)stationStruct.LoopTakes.size();
	Size2d gap = { 4, 4 };
	auto takeHeight = numTakes > 0 ?
		std::max(1u, stationParams.Size.Height - ((2 + numTakes - 1) * gap.Height) / numTakes) :
		std::max(1u, stationParams.Size.Height - (2 * gap.Height));
	Size2d takeSize = { stationParams.Size.Width - (2 * gap.Width), takeHeight };
	LoopTakeParams takeParams;
	takeParams.Size = { 80, 80 };
	
	auto takeCount = 0u;
	for (auto takeStruct : stationStruct.LoopTakes)
	{
		takeParams.ModelPosition = { (float)gap.Width, (float)(takeCount * takeHeight + gap.Height), 0.0 };
		auto take = LoopTake::FromFile(takeParams, takeStruct, dir);
		
		if (take.has_value())
			station->AddTake(take.value());

		takeCount++;
	}
		
	return station;
}

void Station::SetSize(utils::Size2d size)
{
	GuiElement::SetSize(size);

	ArrangeTakes();
}


utils::Position2d Station::Position() const
{
	return _modelScreenPos;
}

void Station::OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int indexOffset,
	unsigned int numSamps)
{
	for (auto& take : _loopTakes)
		take->OnPlay(dest, trigger, indexOffset, numSamps);
}

void Station::EndMultiPlay(unsigned int numSamps)
{
	for (auto& take : _loopTakes)
		take->EndMultiPlay(numSamps);
}

void Station::OnWriteChannel(unsigned int channel,
	const std::shared_ptr<base::AudioSource> src,
	int indexOffset,
	unsigned int numSamps)
{
	for (auto& take : _loopTakes)
		take->OnWriteChannel(channel, src, indexOffset, numSamps);
}

// TODO: Remove method
void Station::OnWrite(const std::shared_ptr<base::MultiAudioSource> src,
	int indexOffset,
	unsigned int numSamps)
{
	for (auto& take : _loopTakes)
		take->OnWrite(src, indexOffset, numSamps);
}

void Station::EndMultiWrite(unsigned int numSamps,
	bool updateIndex)
{
	for (auto& take : _loopTakes)
		take->EndMultiWrite(numSamps, updateIndex);
}

ActionResult Station::OnAction(KeyAction action)
{
	ActionResult res;
	res.IsEaten = false;
	res.ResultType = actions::ACTIONRESULT_DEFAULT;

	for (auto& trig : _triggers)
	{
		auto trigResult = trig->OnAction(action);
		if (trigResult.IsEaten)
			return trigResult;
	}

	res.IsEaten = false;
	return res;
}

ActionResult Station::OnAction(TouchAction action)
{
	return GuiElement::OnAction(action);
}

ActionResult Station::OnAction(TriggerAction action)
{
	ActionResult res;
	res.IsEaten = false;

	auto loopCount = 0u;
	auto loopTake = TryGetTake(action.TargetId);

	switch (action.ActionType)
	{
	case TriggerAction::TRIGGER_REC_START:
	{
		auto newLoopTake = AddTake();
		newLoopTake->Record(action.InputChannels, Name());

		res.SourceId = "";
		res.TargetId = newLoopTake->Id();
		res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		res.IsEaten = true;
		break;
	}
	case TriggerAction::TRIGGER_REC_END:
	{
		auto loopLength = action.SampleCount;
		auto errorSamps = 0;

		if (_clock)
		{
			if (_clock->IsQuantisable())
			{
				auto [quantisedLength, err] = _clock->QuantiseLength(action.SampleCount);
				loopLength = quantisedLength;
				errorSamps = err;
				std::cout << "Quantised loop to " << loopLength << " with error " << errorSamps << std::endl;
			}
			else
			{
				_clock->SetQuantisation(action.SampleCount / 4, Timer::QUANTISE_MULTIPLE);
				std::cout << "Set clock to " << (action.SampleCount / 4) << std::endl;
			}
		}

		auto cfg = action.GetUserConfig();
		auto streamParams = action.GetAudioParams();
		auto outLatency = streamParams.has_value() ?
			streamParams.value().OutputLatency :
			0u;

		if (0u == outLatency)
		{
			outLatency = cfg.has_value() ?
				cfg.value().Audio.LatencyOut :
				0u;
		}

		auto playPos = cfg.has_value() ?
			cfg.value().LoopPlayPos(errorSamps, loopLength, outLatency) :
			0;
		auto endRecordSamps = cfg.has_value() ?
			cfg.value().EndRecordingSamps(errorSamps) :
			0;

		std::cout << "Playing loop from " << playPos << " with loop length " << loopLength << " (out latency = " << outLatency << ")" << std::endl;

		if (loopTake.has_value())
			loopTake.value()->Play(playPos, loopLength, endRecordSamps);

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		break;
	}
	case TriggerAction::TRIGGER_OVERDUB_START:
	{
		auto sourceId = _backLoopTakes.empty() ? "" : _backLoopTakes.back()->Id();

		auto newLoopTake = AddTake();
		newLoopTake->Overdub(action.InputChannels, Name());

		res.SourceId = sourceId;
		res.TargetId = newLoopTake->Id();
		res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		res.IsEaten = true;
		break;
	}
	case TriggerAction::TRIGGER_OVERDUB_END:
	{
		auto loopLength = action.SampleCount;
		auto errorSamps = 0;

		if (_clock)
		{
			if (_clock->IsQuantisable())
			{
				auto [quantisedLength, err] = _clock->QuantiseLength(action.SampleCount);
				loopLength = quantisedLength;
				errorSamps = err;
				std::cout << "Quantised loop to " << loopLength << " with error " << errorSamps << std::endl;
			}
			else
			{
				_clock->SetQuantisation(action.SampleCount / 4, Timer::QUANTISE_MULTIPLE);
				std::cout << "Set clock to " << (action.SampleCount / 4) << std::endl;
			}
		}

		auto cfg = action.GetUserConfig();
		auto streamParams = action.GetAudioParams();
		auto outLatency = streamParams.has_value() ?
			streamParams.value().OutputLatency :
			0u;

		if (0u == outLatency)
		{
			outLatency = cfg.has_value() ?
				cfg.value().Audio.LatencyOut :
				0u;
		}

		auto playPos = cfg.has_value() ?
			cfg.value().LoopPlayPos(errorSamps, loopLength, outLatency) :
			0;
		auto endRecordSamps = cfg.has_value() ?
			cfg.value().EndRecordingSamps(errorSamps) :
			0;

		std::cout << "Playing loop from " << playPos << " with loop length " << loopLength << " (out latency = " << outLatency << ")" << std::endl;

		if (loopTake.has_value())
			loopTake.value()->Play(playPos, loopLength, endRecordSamps);

		auto sourceLoopTake = TryGetTake(action.SourceId);
		if (sourceLoopTake.has_value())
			sourceLoopTake.value()->Mute();

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_ACTIVATE;
		break;
	}
	case TriggerAction::TRIGGER_DITCH:
		if (loopTake.has_value())
		{
			loopTake.value()->Ditch();

			auto id = loopTake.value()->Id();
			auto match = std::find_if(_backLoopTakes.begin(),
				_backLoopTakes.end(),
				[&id](const std::shared_ptr<LoopTake>& arg) { return arg->Id() == id; });

			if (match != _backLoopTakes.end())
			{
				_backLoopTakes.erase(match);
				ArrangeTakes();
				_changesMade = true;
			}
		}

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_DITCH;
		break;
	case TriggerAction::TRIGGER_DITCH_UNMUTE:
		if (loopTake.has_value())
		{
			loopTake.value()->UnMute();
		}

		res.IsEaten = true;
		res.ResultType = actions::ActionResultType::ACTIONRESULT_DEFAULT;
		break;
	}

	return res;
}

void Station::OnTick(Time curTime,
	unsigned int samps,
	std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	for (auto& trig : _triggers)
	{
		trig->OnTick(curTime, samps, cfg, params);
	}
}

std::shared_ptr<LoopTake> Station::AddTake()
{
	LoopTakeParams takeParams;
	takeParams.Id = _name + "-TK-" + utils::GetGuid();
	takeParams.FadeSamps = _fadeSamps;

	auto take = std::make_shared<LoopTake>(takeParams);
	AddTake(take);

	return take;
}

void Station::AddTake(std::shared_ptr<LoopTake> take)
{
	_backLoopTakes.push_back(take);
	Init();

	ArrangeTakes();
	_changesMade = true;
}

void Station::AddTrigger(std::shared_ptr<Trigger> trigger)
{
	trigger->SetReceiver(ActionReceiver::shared_from_this());

	_triggers.push_back(trigger);
	_children.push_back(trigger);
}

unsigned int Station::NumTakes() const
{
	return _changesMade ?
		(unsigned int)_backLoopTakes.size() :
		(unsigned int)_loopTakes.size();
}

void Station::Reset()
{
	for (auto& take : _loopTakes)
	{
		auto child = std::find(_children.begin(), _children.end(), take);
		if (_children.end() != child)
			_children.erase(child);
	}
	_loopTakes.clear();

	for (auto& trigger : _triggers)
	{
		auto child = std::find(_children.begin(), _children.end(), trigger);
		if (_children.end() != child)
			_children.erase(child);
	}
	_triggers.clear();
}

std::string Station::Name() const
{
	return _name;
}

void Station::SetName(std::string name)
{
	_name = name;
}

void Station::SetClock(std::shared_ptr<Timer> clock)
{
	_clock = clock;
}

void Station::OnBounce(unsigned int numSamps, io::UserConfig config)
{
	for (auto& trigger : _triggers)
	{
		auto takes = trigger->GetTakes();

		for (auto& take : takes)
		{
			std::string sourceId = take.SourceTakeId;
			std::string targetId = take.TargetTakeId;
			auto sourceMatch = std::find_if(_backLoopTakes.begin(),
				_backLoopTakes.end(),
				[&sourceId](const std::shared_ptr<LoopTake>& arg) { return arg->Id() == sourceId; });
			auto targetMatch = std::find_if(_backLoopTakes.begin(),
				_backLoopTakes.end(),
				[&targetId](const std::shared_ptr<LoopTake>& arg) { return arg->Id() == targetId; });

			if ((_backLoopTakes.end() != sourceMatch) && (_backLoopTakes.end() != targetMatch))
			{
				(*sourceMatch)->OnPlay(*targetMatch, trigger, -((long)constants::MaxLoopFadeSamps), numSamps);
			}
		}
	}
}

unsigned int Station::CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes)
{
	if (0 == numTakes)
		return 0;

	int minHeight = 5;
	int totalGap = (numTakes + 1) * _Gap.Width;
	int height = (((int)stationHeight) - totalGap) / (int)numTakes;

	if (height < minHeight)
		return minHeight;

	return height;
}

std::vector<JobAction> Station::_CommitChanges()
{
	// Remove and add any children
	// (difference between back and front LoopTake buffer)
	std::vector<std::shared_ptr<LoopTake>> toAdd;
	std::vector<std::shared_ptr<LoopTake>> toRemove;
	std::copy_if(_backLoopTakes.begin(), _backLoopTakes.end(), std::back_inserter(toAdd), [&](const std::shared_ptr<LoopTake>& take) { return (std::find(_loopTakes.begin(), _loopTakes.end(), take) == _loopTakes.end()); });
	std::copy_if(_loopTakes.begin(), _loopTakes.end(), std::back_inserter(toRemove), [&](const std::shared_ptr<LoopTake>& take) { return (std::find(_backLoopTakes.begin(), _backLoopTakes.end(), take) == _backLoopTakes.end()); });
	
	for (auto& take : toAdd)
	{
		take->SetParent(GuiElement::shared_from_this());
		take->Init();
		_children.push_back(take);
	}

	for (auto& take : toRemove)
	{
		auto child = std::find(_children.begin(), _children.end(), take);
		if (_children.end() != child)
			_children.erase(child);
	}
	
	_loopTakes = _backLoopTakes; // TODO: Undo?

	GuiElement::_CommitChanges();

	return {};
}

void Station::ArrangeTakes()
{
	auto numTakes = (unsigned int)_backLoopTakes.size();

	auto takeHeight = CalcTakeHeight(_sizeParams.Size.Height, numTakes);
	utils::Size2d takeSize = { _sizeParams.Size.Width - (2 * _Gap.Width), takeHeight - (2 * _Gap.Height) };

	auto takeCount = 0;
	for (auto& take : _backLoopTakes)
	{
		take->SetPosition({ 0, (int)(_Gap.Height + (takeCount * takeHeight)) });
		take->SetSize(takeSize);
		take->SetModelPosition({0.0f, (float)(takeCount * takeHeight), 0.0f });
		take->SetModelScale(1.0);
		std::cout << "[Arranging take " << take->Id() << "] Y: " << (float)(takeCount * takeHeight) << std::endl;

		takeCount++;
	}
}

std::optional<std::shared_ptr<LoopTake>> Station::TryGetTake(std::string id)
{
	for (auto& take : _loopTakes)
	{
		if (take->Id() == id)
			return take;
	}

	return std::nullopt;
}
