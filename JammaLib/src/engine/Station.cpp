#include "Station.h"
#include <memory>

using namespace engine;
using namespace audio;
using namespace actions;
using namespace base;
using resources::ResourceLib;
using gui::GuiSliderParams;
using utils::Size2d;
using gui::GuiRouterParams;

const utils::Size2d Station::_Gap = { 5, 5 };

Station::Station(StationParams params,
	AudioMixerParams mixerParams) :
	GuiElement(params),
	Tweakable(params),
	MultiAudioSource(),
	_flipTakeBuffer(false),
	_flipAudioBuffer(false),
	_name(params.Name),
	_fadeSamps(params.FadeSamps),
	_clock(std::shared_ptr<Timer>()),
	_mixer(nullptr),
	_router(nullptr),
	_loopTakes(),
	_triggers(),
	_backLoopTakes(),
	_audioBuffers(),
	_backAudioBuffers()
{
	_mixer = std::make_unique<AudioMixer>(mixerParams);
	_router = std::make_unique<gui::GuiRouter>(_GetRouterParams(params.Size),
		8,
		8);

	_children.push_back(_mixer);
	_children.push_back(_router);
}

Station::~Station()
{
}

std::optional<std::shared_ptr<Station>> Station::FromFile(StationParams stationParams,
	AudioMixerParams mixerParams,
	io::JamFile::Station stationStruct,
	std::wstring dir)
{
	stationParams.Name = stationStruct.Name;
	auto station = std::make_shared<Station>(stationParams, mixerParams);

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

AudioMixerParams Station::GetMixerParams(utils::Size2d stationSize,
	audio::BehaviourParams behaviour)
{
	AudioMixerParams mixerParams;
	mixerParams.Size = { 110, stationSize.Height };
	mixerParams.Position = { 6, 6 };
	mixerParams.Behaviour = behaviour;

	return mixerParams;
}

void Station::SetSize(utils::Size2d size)
{
	GuiElement::SetSize(size);

	_ArrangeTakes();
}

utils::Position2d Station::Position() const
{
	return _modelScreenPos;
}

unsigned int Station::NumOutputChannels() const
{
	return _changesMade ?
		(unsigned int)_backAudioBuffers.size() :
		(unsigned int)_audioBuffers.size();
}

unsigned int Station::NumInputChannels() const
{
	return _changesMade ?
		(unsigned int)_backAudioBuffers.size() :
		(unsigned int)_audioBuffers.size();
}

void Station::Zero(unsigned int numSamps,
	Audible::AudioSourceType source)
{
	for (auto chan = 0u; chan < NumInputChannels(); chan++)
	{
		auto channel = _InputChannel(chan, source);
		if (channel)
			channel->Zero(numSamps);
	}

	for (auto& take : _loopTakes)
		take->Zero(numSamps, source);
}

// Only called when outputting to DAC
void Station::OnPlay(const std::shared_ptr<base::MultiAudioSink> dest,
	const std::shared_ptr<Trigger> trigger,
	int indexOffset,
	unsigned int numSamps)
{
	auto ptr = Sharable::shared_from_this();

	for (auto& take : _loopTakes)
		take->OnPlay(std::dynamic_pointer_cast<MultiAudioSink>(ptr),
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

void Station::EndMultiPlay(unsigned int numSamps)
{
	for (auto& take : _loopTakes)
		take->EndMultiPlay(numSamps);

	for (auto& buffer : _audioBuffers)
	{
		buffer->EndWrite(numSamps, true);
		buffer->EndPlay(numSamps);
	}
}

void Station::OnWriteChannel(unsigned int channel,
	const std::shared_ptr<base::AudioSource> src,
	int indexOffset,
	unsigned int numSamps,
	Audible::AudioSourceType source)
{
	switch (source)
	{
	case Audible::AudioSourceType::AUDIOSOURCE_ADC:
	case Audible::AudioSourceType::AUDIOSOURCE_MONITOR:
	case Audible::AudioSourceType::AUDIOSOURCE_BOUNCE:
		for (auto& take : _loopTakes)
			take->OnWriteChannel(channel,
				src,
				indexOffset,
				numSamps,
				source);
		break;
	case Audible::AudioSourceType::AUDIOSOURCE_LOOPS:
		for (auto& take : _loopTakes)
			take->OnWriteChannel(channel,
				src,
				indexOffset,
				numSamps,
				source);
		break;
	}
}

void Station::EndMultiWrite(unsigned int numSamps,
	bool updateIndex,
	Audible::AudioSourceType source)
{
	for (auto& take : _loopTakes)
		take->EndMultiWrite(numSamps, updateIndex, source);
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

ActionResult Station::OnAction(GuiAction action)
{
	auto res = GuiElement::OnAction(action);

	if (res.IsEaten)
		return res;

	if (auto chans = std::get_if<GuiAction::GuiConnections>(&action.Data))
		_mixer->SetChannels(chans->Connections);

	return res;
}

ActionResult Station::OnAction(TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	if (res.IsEaten && (ACTIONRESULT_ROUTER == res.ResultType))
		std::cout << "Pause" << std::endl;

	return res;
}

ActionResult Station::OnAction(TouchMoveAction action)
{
	auto res = GuiElement::OnAction(action);

	if (res.IsEaten)
		return res;

	return res;
}


ActionResult Station::OnAction(TriggerAction action)
{
	ActionResult res;
	res.IsEaten = false;

	auto loopCount = 0u;
	auto loopTake = _TryGetTake(action.TargetId);

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

		auto sourceLoopTake = _TryGetTake(action.SourceId);
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
				_ArrangeTakes();
				_flipTakeBuffer = true;
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

	MergeMixBehaviourParams mergeParams;

	for (unsigned int i = 0; i < _audioBuffers.size(); i++)
	{
		mergeParams.Channels.push_back(i);
	}

	auto mixerParams = LoopTake::GetMixerParams({ 100,100 }, mergeParams);
	auto take = std::make_shared<LoopTake>(takeParams, mixerParams);
	AddTake(take);

	return take;
}

void Station::AddTake(std::shared_ptr<LoopTake> take)
{
	take->SetupBuffers(NumInputChannels(), BufSize());

	_backLoopTakes.push_back(take);
	Init();

	_ArrangeTakes();
	_flipTakeBuffer = true;
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

void Station::SetupBuffers(unsigned int chans, unsigned int bufSize)
{
	MergeMixBehaviourParams mergeParams;

	_backAudioBuffers.clear();

	for (unsigned int i = 0; i < chans; i++)
	{
		mergeParams.Channels.push_back(i);
		_backAudioBuffers.push_back(std::make_shared<audio::AudioBuffer>(bufSize));
	}

	_mixer->SetBehaviour(std::make_unique<MergeMixBehaviour>(mergeParams));

	_flipAudioBuffer = true;
	_changesMade = true;

	for (auto& take : _loopTakes)
		take->SetupBuffers(chans, bufSize);
}

unsigned int Station::BufSize() const
{
	auto bufs = _changesMade ? _backAudioBuffers : _audioBuffers;
	if (bufs.empty())
		return 0;

	return bufs[0]->BufSize();
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

unsigned int Station::_CalcTakeHeight(unsigned int stationHeight, unsigned int numTakes)
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

void Station::_InitReceivers()
{
	_router->SetReceiver(ActionReceiver::shared_from_this());
}

std::vector<JobAction> Station::_CommitChanges()
{
	if (_flipTakeBuffer)
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
		_flipTakeBuffer = false;
	}

	if (_flipAudioBuffer)
	{
		_audioBuffers = _backAudioBuffers;
		_flipAudioBuffer = false;
	}

	GuiElement::_CommitChanges();

	return {};
}

bool Station::_HitTest(utils::Position2d localPos)
{
	return GuiElement::_HitTest(localPos);
}

const std::shared_ptr<AudioSink> Station::_InputChannel(unsigned int channel,
	Audible::AudioSourceType source)
{
	if (channel < _audioBuffers.size())
		return _audioBuffers[channel];

	return nullptr;
}

gui::GuiRouterParams Station::_GetRouterParams(utils::Size2d size)
{
	size.Width = 300;
	size.Height = 300;
	GuiRouterParams routerParams;

	routerParams.Position = { (int)_Gap.Width, (int)_Gap.Height };
	routerParams.Size = { size.Width - (2 * _Gap.Width), size.Height - (2 * _Gap.Height) };
	routerParams.MinSize = routerParams.Size;
	routerParams.InputType = GuiRouterParams::CHANNEL_BUS;
	routerParams.OutputType = GuiRouterParams::CHANNEL_DEVICE;
	routerParams.InputSpacing = GuiRouterParams::BusWidth + GuiRouterParams::BusGap;
	routerParams.InputSize = GuiRouterParams::BusWidth;
	routerParams.OutputSpacing = GuiRouterParams::BusWidth + GuiRouterParams::BusGap;
	routerParams.OutputSize = GuiRouterParams::BusWidth;
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

void Station::_ArrangeTakes()
{
	auto numTakes = (unsigned int)_backLoopTakes.size();

	auto takeHeight = _CalcTakeHeight(_sizeParams.Size.Height, numTakes);
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

std::optional<std::shared_ptr<LoopTake>> Station::_TryGetTake(std::string id)
{
	for (auto& take : _loopTakes)
	{
		if (take->Id() == id)
			return take;
	}

	return std::nullopt;
}
