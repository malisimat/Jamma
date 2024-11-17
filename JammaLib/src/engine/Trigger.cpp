#include "Trigger.h"
#include "glm/glm.hpp"
#include "glm/ext.hpp"

using namespace base;
using namespace engine;
using actions::ActionResult;
using actions::KeyAction;
using actions::TriggerAction;
using actions::DelayedAction;
using graphics::GlDrawContext;
using graphics::Image;
using graphics::ImageParams;
using audio::AudioMixer;
using resources::ResourceLib;

Trigger::Trigger(TriggerParams trigParams) :
	GuiElement(trigParams),
	_name(trigParams.Name),
	_activateBindings(trigParams.Activate),
	_ditchBindings(trigParams.Ditch),
	_inputChannels(trigParams.InputChannels),
	_state(TRIGSTATE_DEFAULT),
	_debounceTimeMs(trigParams.DebounceMs),
	_lastActivateTime(),
	_lastDitchTime(),
	_isLastActivateDown(false),
	_isLastDitchDown(false),
	_isLastActivateDownRaw(false),
	_isLastDitchDownRaw(false),
	_recordSampCount(0),
	_textureRecording(ImageParams(DrawableParams{ trigParams.TextureRecording }, SizeableParams{ trigParams.Size,trigParams.MinSize }, "texture")),
	_textureDitchDown(ImageParams(DrawableParams{ trigParams.TextureDitchDown }, SizeableParams{ trigParams.Size,trigParams.MinSize }, "texture")),
	_textureOverdubbing(ImageParams(DrawableParams{ trigParams.TextureOverdubbing }, SizeableParams{ trigParams.Size,trigParams.MinSize }, "texture")),
	_texturePunchedIn(ImageParams(DrawableParams{ trigParams.TexturePunchedIn }, SizeableParams{ trigParams.Size,trigParams.MinSize }, "texture")),
	_lastLoopTakes({}),
	_overdubMixer(std::shared_ptr<audio::AudioMixer>()),
	_delayedActions({})
{
	_overdubMixer = std::make_shared<AudioMixer>(
		GetOverdubMixerParams(trigParams.InputChannels));
}

Trigger::~Trigger()
{
}

std::optional<std::shared_ptr<Trigger>> Trigger::FromFile(TriggerParams trigParams, io::RigFile::Trigger trigStruct)
{
	trigParams.Name = trigStruct.Name;

	auto trigger = std::make_shared<Trigger>(trigParams);

	for (auto trigPair : trigStruct.TriggerPairs)
	{
		auto activate = DualBinding(
			TriggerBinding{
				TriggerSource::TRIGGER_KEY,
				trigPair.ActivateDown,
				1
			},
			TriggerBinding{
				TriggerSource::TRIGGER_KEY,
				trigPair.ActivateUp,
				0
			});

		auto ditch = DualBinding(
			TriggerBinding{
				TriggerSource::TRIGGER_KEY,
				trigPair.DitchDown,
				1
			},
			TriggerBinding{
				TriggerSource::TRIGGER_KEY,
				trigPair.DitchUp,
				0
			});

		trigger->AddBinding(activate, ditch);
	}

	for (auto inChan : trigStruct.InputChannels)
		trigger->AddInputChannel(inChan);

	return trigger;
}

audio::BounceMixBehaviourParams Trigger::GetOverdubBehaviourParams(
	std::vector<unsigned int> channels)
{
	return audio::BounceMixBehaviourParams{ audio::WireMixBehaviourParams(channels) };
}

audio::AudioMixerParams Trigger::GetOverdubMixerParams(std::vector<unsigned int> channels)
{
	audio::AudioMixerParams mixerParams;
	mixerParams.Size = { 1, 1 };
	mixerParams.Position = { 0, 0 };
	mixerParams.Behaviour = GetOverdubBehaviourParams(channels);

	return mixerParams;
}

utils::Position2d Trigger::Position() const
{
	auto pos = ModelPosition();
	return { (int)round(pos.X), (int)round(pos.Y) };
}

ActionResult Trigger::OnAction(KeyAction action)
{
	ActionResult res;
	res.IsEaten = false;
	res.ResultType = actions::ACTIONRESULT_DEFAULT;

	auto keyState = KeyAction::KEY_DOWN == action.KeyActionType ? 1 : 0;

	for (auto& b : _activateBindings)
	{
		if (TryChangeState(b, true, action, keyState))
		{
			res.IsEaten = true;
			res.ResultType = actions::ACTIONRESULT_ACTIVATE;
			return res;
		}
	}
	for (auto& b : _ditchBindings)
	{
		if (TryChangeState(b, false, action, keyState))
		{
			res.IsEaten = true;
			res.ResultType = (TRIGSTATE_DEFAULT == _state) ?
				actions::ACTIONRESULT_DITCH :
				actions::ACTIONRESULT_DEFAULT;
			return res;
		}
	}

	return res;
}

void Trigger::OnTick(Time curTime,
	unsigned int samps,
	std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	if ((TriggerState::TRIGSTATE_DEFAULT != _state) &&
		(TriggerState::TRIGSTATE_DITCHDOWN != _state))
	{
		_recordSampCount += samps;

		for (auto& action : _delayedActions)
		{
			action.OnTick(curTime, samps, cfg, params);
		}
	}

	if (0 == _debounceTimeMs)
		return;

	// Eventually flick to new state
	// (if held long enough)
	auto elapsedMs = Timer::GetElapsedSeconds(_lastActivateTime, curTime) * 1000.0;

	if (_isLastActivateDownRaw != _isLastActivateDown)
	{
		if (elapsedMs > _debounceTimeMs)
		{
			_lastActivateTime = Timer::GetZero();
			_isLastActivateDown = _isLastActivateDownRaw;

			StateMachine(_isLastActivateDownRaw, true, cfg, params);
		}
	}

	elapsedMs = Timer::GetElapsedSeconds(_lastDitchTime, curTime) * 1000.0;
	
	if (_isLastDitchDownRaw != _isLastDitchDown)
	{
		if (elapsedMs > _debounceTimeMs)
		{
			_lastDitchTime = Timer::GetZero();
			_isLastDitchDown = _isLastDitchDownRaw;

			StateMachine(_isLastDitchDownRaw, false, cfg, params);
		}
	}
}

void Trigger::Draw(base::DrawContext& ctx)
{
	if (TRIGSTATE_DEFAULT == _state)
	{
		GuiElement::Draw(ctx);
		return;
	}

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)));

	switch (_state)
	{
	case TRIGSTATE_RECORDING:
		_textureRecording.Draw(ctx);
		break;
	case TRIGSTATE_DITCHDOWN:
		_textureDitchDown.Draw(ctx);
		break;
	case TRIGSTATE_OVERDUBBING:
		_textureOverdubbing.Draw(ctx);
		break;
	case TRIGSTATE_OVERDUBBINGDITCHDOWN:
		_textureDitchDown.Draw(ctx);
		break;
	case TRIGSTATE_PUNCHEDIN:
		_texturePunchedIn.Draw(ctx);
		break;
	case TRIGSTATE_PUNCHEDINDITCHDOWN:
		_textureDitchDown.Draw(ctx);
		break;
	}

	for (auto& child : _children)
		child->Draw(ctx);

	glCtx.PopMvp();
}

void Trigger::AddBinding(DualBinding activate, DualBinding ditch)
{
	_activateBindings.push_back(activate);
	_ditchBindings.push_back(ditch);
}

void Trigger::RemoveBinding(DualBinding activate, DualBinding ditch)
{
	auto foundBinding = std::find(_activateBindings.begin(), _activateBindings.end(), activate);
	if (foundBinding != _activateBindings.end())
		_activateBindings.erase(foundBinding);

	foundBinding = std::find(_ditchBindings.begin(), _ditchBindings.end(), ditch);
	if (foundBinding != _ditchBindings.end())
		_ditchBindings.erase(foundBinding);
}

void Trigger::ClearBindings()
{
	_activateBindings.clear();
	_ditchBindings.clear();
}

void Trigger::AddInputChannel(unsigned int chan)
{
	_inputChannels.push_back(chan);

	_UpdateBehaviour();
}

void Trigger::RemoveInputChannel(unsigned int chan)
{
	auto inChan = std::find(_inputChannels.begin(), _inputChannels.end(), chan);
	if (inChan != _inputChannels.end())
		_inputChannels.erase(inChan);

	_UpdateBehaviour();
}

void Trigger::ClearInputChannels()
{
	_inputChannels.clear();

	_UpdateBehaviour();
}

TriggerState Trigger::GetState() const
{
	return _state;
}

void Trigger::Reset()
{
	_state = TRIGSTATE_DEFAULT;
	
	for (auto& b : _activateBindings)
		b.Reset();

	for (auto& b : _ditchBindings)
		b.Reset();

	_state = TriggerState::TRIGSTATE_DEFAULT;
	_recordSampCount = 0;
	_lastLoopTakes.clear();
}

std::string Trigger::Name() const
{
	return _name;
}

void Trigger::SetName(std::string name)
{
	_name = name;
}

std::optional<TriggerTake> Trigger::TryGetLastTake() const
{
	if (!_lastLoopTakes.empty())
		return _lastLoopTakes.back();

	return std::nullopt;
}

void Trigger::OnPlay(const std::shared_ptr<MultiAudioSink> dest,
	float samp,
	unsigned int index)
{
	bool removeExpired = false;
	for (auto& action : _delayedActions)
	{
		if (action.SampsLeft(index) == 0)
		{
			_overdubMixer->SetLevel(action.GetTarget());
			removeExpired = true;
		}
	}

	if (removeExpired)
	{
		std::vector<DelayedAction> newActions;

		auto isNotExpired = [index](DelayedAction action) { return action.SampsLeft(index) > 0; };
		std::copy_if(_delayedActions.begin(), _delayedActions.end(),
			std::back_inserter(newActions), isNotExpired);

		_delayedActions = newActions;
	}

	_overdubMixer->OnPlay(dest, samp, index);
}

bool Trigger::IgnoreRepeats(bool isActivate, DualBinding::TestResult trigResult)
{
	bool allowedThrough = true;

	if (isActivate)
	{
		if ((DualBinding::MATCH_DOWN == trigResult) && _isLastActivateDownRaw)
			allowedThrough = false;
		else if ((DualBinding::MATCH_RELEASE == trigResult) && !_isLastActivateDownRaw)
			allowedThrough = false;
	}
	else
	{
		if ((DualBinding::MATCH_DOWN == trigResult) && _isLastDitchDownRaw)
			allowedThrough = false;
		else if ((DualBinding::MATCH_RELEASE == trigResult) && !_isLastDitchDownRaw)
			allowedThrough = false;
	}

	return allowedThrough;
}

bool Trigger::Debounce(bool isActivate,
	DualBinding::TestResult trigResult,
	Time actionTime)
{
	auto allowedThrough = false;

	if (isActivate)
	{
		auto isDebounceBypassed = Timer::IsZero(_lastActivateTime) || (0 == _debounceTimeMs);
		auto elapsedMs = Timer::GetElapsedSeconds(_lastActivateTime, actionTime) * 1000.0;

		if ((DualBinding::MATCH_DOWN == trigResult) && !_isLastActivateDownRaw)
		{
			_lastActivateTime = actionTime;
			_isLastActivateDownRaw = true;

			if (isDebounceBypassed || (elapsedMs > _debounceTimeMs))
			{
				allowedThrough = true;
				_isLastActivateDown = true;
			}
		}
		else if ((DualBinding::MATCH_RELEASE == trigResult) && _isLastActivateDownRaw)
		{
			_lastActivateTime = actionTime;
			_isLastActivateDownRaw = false;

			if (isDebounceBypassed || (elapsedMs > _debounceTimeMs))
			{
				allowedThrough = true;
				_isLastActivateDown = false;
			}
		}
	}
	else
	{
		auto isDebounceBypassed = Timer::IsZero(_lastDitchTime) || (0 == _debounceTimeMs);
		auto elapsedMs = Timer::GetElapsedSeconds(_lastDitchTime, actionTime) * 1000.0;

		if ((DualBinding::MATCH_DOWN == trigResult) && !_isLastDitchDownRaw)
		{
			_lastDitchTime = actionTime;
			_isLastDitchDownRaw = true;

			if (isDebounceBypassed || (elapsedMs > _debounceTimeMs))
			{
				allowedThrough = true;
				_isLastDitchDown = true;
			}
		}
		else if ((DualBinding::MATCH_RELEASE == trigResult) && _isLastDitchDownRaw)
		{
			_lastDitchTime = actionTime;
			_isLastDitchDownRaw = false;

			if (isDebounceBypassed || (elapsedMs > _debounceTimeMs))
			{
				allowedThrough = true;
				_isLastDitchDown = false;
			}
		}
	}

	return allowedThrough;
}

bool Trigger::TryChangeState(DualBinding& binding,
	bool isActivate,
	const actions::KeyAction& action,
	int keyState)
{
	auto trigResult = binding.OnTrigger(
		TriggerSource::TRIGGER_KEY,
		action.KeyChar,
		keyState);

	bool allowedThrough = IgnoreRepeats(isActivate, trigResult);
	if (!allowedThrough)
	{
		return false;
	}

	allowedThrough = Debounce(isActivate,
		trigResult,
		action.GetActionTime());

	if (!allowedThrough)
	{
		return false;
	}

	switch (trigResult)
	{
	case DualBinding::MATCH_DOWN:
		StateMachine(true, isActivate, action.GetUserConfig(), action.GetAudioParams());
		return true;
	case DualBinding::MATCH_RELEASE:
		StateMachine(false, isActivate, action.GetUserConfig(), action.GetAudioParams());
		return true;
	}

	return false;
}

bool Trigger::StateMachine(bool isDown,
	bool isActivate,
	std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	bool changedState = false;

	switch (_state)
	{
	case TRIGSTATE_DEFAULT:
		if (isDown)
		{
			if (isActivate)
			{
				StartRecording(cfg, params);
				changedState = true;
			}
			else
			{
				SetDitchDown(cfg, params);
				changedState = true;
			}
		}
		break;
	case TRIGSTATE_RECORDING:
		if (isActivate && isDown)
		{
			EndRecording(cfg, params);
			changedState = true;
		}
		else if (!isActivate && isDown)
		{
			Ditch(cfg, params);
			changedState = true;
		}
		break;
	case TRIGSTATE_DITCHDOWN:
		if (isActivate && isDown)
		{
			StartOverdub(cfg, params);
			changedState = true;
		}
		else if (!isActivate && !isDown)
		{
			Ditch(cfg, params);
			changedState = true;
		}
		break;
	case TRIGSTATE_OVERDUBBING:
		if (isActivate && isDown)
		{
			StartPunchIn(cfg, params);
			changedState = true;
		}
		else if (!isActivate && isDown)
		{
			SetDitchDown(cfg, params);
			changedState = true;
		}
		break;
	case TRIGSTATE_OVERDUBBINGDITCHDOWN:
		if (isActivate && isDown)
		{
			EndOverdub(cfg, params);
			changedState = true;
		}
		else if (!isActivate && !isDown)
		{
			Ditch(cfg, params);
			changedState = true;
		}
		break;
	case TRIGSTATE_PUNCHEDIN:
		if (isActivate && !isDown)
		{
			// End punch-in but maintain overdub mode (release)
			EndPunchIn(cfg, params);
			changedState = true;
		}
		break;
	case TRIGSTATE_PUNCHEDINDITCHDOWN:
		if (!isActivate && !isDown)
		{
			SetDitchUp(cfg, params);
		}
		break;
	}

	return changedState;
}

void Trigger::StartRecording(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	_state = TRIGSTATE_RECORDING;

	std::cout << "~~~~ Trigger RECORDING" << std::endl;

	_recordSampCount = 0;
	_delayedActions.clear();

	if (_receiver)
	{
		TriggerAction trigAction;
		trigAction.ActionType = TriggerAction::TRIGGER_REC_START;
		trigAction.InputChannels = _inputChannels;

		if (cfg.has_value())
			trigAction.SetUserConfig(cfg.value());

		if (params.has_value())
			trigAction.SetAudioParams(params.value());

		auto res = _receiver->OnAction(trigAction);

		// TODO: History for undo

		if (res.IsEaten)
		{
			TriggerTake newTake = { TriggerTake::SOURCE_ADC, res.SourceId, res.TargetId };
			_lastLoopTakes.push_back(newTake);
		}
	}
}

void Trigger::EndRecording(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	_state = TRIGSTATE_DEFAULT;

	std::cout << "~~~~ Trigger END RECORDING" << std::endl;

	if ((_receiver) && !_lastLoopTakes.empty())
	{
		auto lastTake = _lastLoopTakes.back();

		TriggerAction trigAction;
		trigAction.ActionType = TriggerAction::TRIGGER_REC_END;
		trigAction.TargetId = lastTake.TargetTakeId;
		trigAction.SampleCount = _recordSampCount;

		if (cfg.has_value())
			trigAction.SetUserConfig(cfg.value());

		if (params.has_value())
			trigAction.SetAudioParams(params.value());

		// TODO: History for undo

		_receiver->OnAction(trigAction);
	}
}

void Trigger::SetDitchDown(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	if (TRIGSTATE_OVERDUBBING == _state)
		_state = TRIGSTATE_OVERDUBBINGDITCHDOWN;
	else if (TRIGSTATE_PUNCHEDIN == _state)
		_state = TRIGSTATE_PUNCHEDINDITCHDOWN;
	else
		_state = TRIGSTATE_DITCHDOWN;
}

void Trigger::SetDitchUp(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	if (TRIGSTATE_PUNCHEDINDITCHDOWN == _state)
		_state = TRIGSTATE_PUNCHEDIN;
}

void Trigger::Ditch(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	_state = TRIGSTATE_DEFAULT;

	std::cout << "~~~~ Trigger DITCH" << std::endl;

	_delayedActions.clear();
	auto popBack = !_lastLoopTakes.empty();

	if ((_receiver) && popBack)
	{
		auto lastTake = _lastLoopTakes.back();

		TriggerAction trigAction;
		trigAction.ActionType = TriggerAction::TRIGGER_DITCH;
		trigAction.TargetId = lastTake.TargetTakeId;
		trigAction.SampleCount = _recordSampCount;

		if (cfg.has_value())
			trigAction.SetUserConfig(cfg.value());

		if (params.has_value())
			trigAction.SetAudioParams(params.value());

		_receiver->OnAction(trigAction);
	}

	if (popBack)
		_lastLoopTakes.pop_back();
}

void Trigger::StartOverdub(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	_state = TRIGSTATE_OVERDUBBING;

	std::cout << "~~~~ Trigger START OVERDUB" << std::endl;

	_recordSampCount = 0;
	_delayedActions.clear();
	_overdubMixer->SetLevel(1.0);

	if (_receiver)
	{
		TriggerAction trigAction;
		trigAction.ActionType = TriggerAction::TRIGGER_OVERDUB_START;
		trigAction.InputChannels = _inputChannels;

		if (cfg.has_value())
			trigAction.SetUserConfig(cfg.value());

		if (params.has_value())
			trigAction.SetAudioParams(params.value());

		auto res = _receiver->OnAction(trigAction);

		if (res.IsEaten)
		{
			TriggerTake newTake = { TriggerTake::SOURCE_ADC, res.SourceId, res.TargetId };
			_lastLoopTakes.push_back(newTake);
		}
	}
}

void Trigger::EndOverdub(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	_state = TRIGSTATE_DEFAULT;

	std::cout << "~~~~ Trigger END OVERDUB" << std::endl;

	if ((_receiver) && !_lastLoopTakes.empty())
	{
		auto lastTake = _lastLoopTakes.back();

		TriggerAction trigAction;
		trigAction.ActionType = TriggerAction::TRIGGER_OVERDUB_END;
		trigAction.TargetId = lastTake.TargetTakeId;
		trigAction.SampleCount = _recordSampCount;

		if (cfg.has_value())
			trigAction.SetUserConfig(cfg.value());

		if (params.has_value())
			trigAction.SetAudioParams(params.value());

		_receiver->OnAction(trigAction);
	}
}

void Trigger::DitchOverdub(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	_state = TRIGSTATE_DEFAULT;

	std::cout << "~~~~ Trigger DITCH OVERDUB" << std::endl;

	_delayedActions.clear();
	auto popBack = !_lastLoopTakes.empty();

	if ((_receiver) && popBack)
	{
		auto lastTake = _lastLoopTakes.back();
		TriggerAction trigAction;
		trigAction.ActionType = TriggerAction::TRIGGER_OVERDUB_DITCH;
		trigAction.TargetId = lastTake.TargetTakeId;
		trigAction.SampleCount = _recordSampCount;

		if (cfg.has_value())
			trigAction.SetUserConfig(cfg.value());

		if (params.has_value())
			trigAction.SetAudioParams(params.value());

		_receiver->OnAction(trigAction);
	}

	if (popBack)
		_lastLoopTakes.pop_back();
}

void Trigger::StartPunchIn(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	_state = TRIGSTATE_PUNCHEDIN;

	std::cout << "~~~~ Trigger START PUNCHIN" << std::endl;

	_delayedActions.clear();
	_delayedActions.push_back(DelayedAction(constants::MaxLoopFadeSamps, 0.0));

	if ((_receiver) && !_lastLoopTakes.empty())
	{
		auto lastTake = _lastLoopTakes.back();

		TriggerAction trigAction;
		trigAction.ActionType = TriggerAction::TRIGGER_PUNCHIN_START;
		trigAction.TargetId = lastTake.TargetTakeId;
		trigAction.SampleCount = _recordSampCount;
		_receiver->OnAction(trigAction);
	}
}

void Trigger::EndPunchIn(std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	if (TRIGSTATE_PUNCHEDINDITCHDOWN == _state)
		_state = TRIGSTATE_OVERDUBBINGDITCHDOWN;
	else
		_state = TRIGSTATE_OVERDUBBING;

	std::cout << "~~~~ Trigger END PUNCHIN" << std::endl;

	_delayedActions.push_back(DelayedAction(constants::MaxLoopFadeSamps, 1.0));

	if ((_receiver) && !_lastLoopTakes.empty())
	{
		auto lastTake = _lastLoopTakes.back();

		TriggerAction trigAction;
		trigAction.ActionType = TriggerAction::TRIGGER_PUNCHIN_END;
		trigAction.TargetId = lastTake.TargetTakeId;
		trigAction.SampleCount = _recordSampCount;

		if (cfg.has_value())
			trigAction.SetUserConfig(cfg.value());

		if (params.has_value())
			trigAction.SetAudioParams(params.value());

		_receiver->OnAction(trigAction);
	}
}

void Trigger::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_textureRecording.InitResources(resourceLib, forceInit);
	_textureDitchDown.InitResources(resourceLib, forceInit);
	_textureOverdubbing.InitResources(resourceLib, forceInit);
	_texturePunchedIn.InitResources(resourceLib, forceInit);

	GuiElement::_InitResources(resourceLib, forceInit);
}

void Trigger::_ReleaseResources()
{
	GuiElement::_ReleaseResources();

	_textureRecording.ReleaseResources();
	_textureDitchDown.ReleaseResources();
	_textureOverdubbing.ReleaseResources();
	_texturePunchedIn.ReleaseResources();
}

void Trigger::_UpdateBehaviour()
{
	_overdubMixer->SetBehaviour(
		std::move(std::make_unique<audio::BounceMixBehaviour>(
			GetOverdubBehaviourParams(_inputChannels))));
}