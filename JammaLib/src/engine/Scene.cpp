#include "Scene.h"
#include "glm/ext.hpp"

using namespace base;
using namespace actions;
using namespace audio;
using namespace engine;
using namespace gui;
using namespace io;
using namespace graphics;
using namespace resources;
using namespace utils;
using namespace std::placeholders;

Scene::Scene(SceneParams params,
	UserConfig user) :
	Drawable(params),
	Moveable(params),
	Sizeable(params),
	_isSceneTouching(false),
	_isSceneQuitting(false),
	_isSceneReset(true),
	_isSceneDragged(false),
	_initTouchDownPosition{},
	_initTouchCamPosition{},
	_viewProj(glm::mat4()),
	_overlayViewProj(glm::mat4()),
	_channelMixer(std::make_shared<ChannelMixer>(ChannelMixerParams{})),
	_label(nullptr),
	_selector(nullptr),
	_modeRadio(nullptr),
	_audioDevice(nullptr),
	_masterLoop(nullptr),
	_stations(),
	_touchDownElement(std::weak_ptr<GuiElement>()),
	_hoverElement3d(std::weak_ptr<GuiElement>()),
	_audioCallbackCount(0),
	_camera(CameraParams(
		MoveableParams(
			Position2d{ 0,0 },
			Position3d{ 0, 0, 420 },
			1.0),
		0)),
	_userConfig(user),
	_clock(std::make_shared<Timer>())
{
	GuiLabelParams labelParams;
	labelParams.String = "Jamma";
	labelParams.Position = { 10, 2 };
	labelParams.ModelPosition = { 10, 2, 0 };
	labelParams.Size = { 20, 40 };
	_label = std::make_unique<GuiLabel>(labelParams);

	GuiSelectorParams selectorParams;
	selectorParams.Position = { 10, 2 };
	selectorParams.Size = params.Size;
	_selector = std::make_unique<GuiSelector>(selectorParams);

	GuiRadioParams modeRadioParams;
	modeRadioParams.Size = { 480, 64 };
	modeRadioParams.Position = { 0, (int)params.Size.Height - (int)modeRadioParams.Size.Height };
	std::vector<GuiToggleParams> radioToggleParams;

	for (auto i = 0u; i < 3; i++)
	{
		GuiToggleParams toggleParams;
		toggleParams.Position = { (int)i * 128, 0 };
		toggleParams.Size = { 128, 64 };

		switch (i)
		{
		case 0:
			toggleParams.Texture = "stationmode";
			toggleParams.OverTexture = "stationmode_over";
			toggleParams.DownTexture = "stationmode_down";
			toggleParams.ToggledTexture = "stationmode_toggled";
			toggleParams.ToggledOverTexture = "stationmode_over";
			toggleParams.ToggledDownTexture = "stationmode_down";
			break;
		case 1:
			toggleParams.Texture = "takemode";
			toggleParams.OverTexture = "takemode_over";
			toggleParams.DownTexture = "takemode_down";
			toggleParams.ToggledTexture = "takemode_toggled";
			toggleParams.ToggledOverTexture = "takemode_over";
			toggleParams.ToggledDownTexture = "takemode_down";
			break;
		case 2:
			toggleParams.Texture = "loopmode";
			toggleParams.OverTexture = "loopmode_over";
			toggleParams.DownTexture = "loopmode_down";
			toggleParams.ToggledTexture = "loopmode_toggled";
			toggleParams.ToggledOverTexture = "loopmode_over";
			toggleParams.ToggledDownTexture = "loopmode_down";
			break;
		}

		radioToggleParams.push_back(toggleParams);
	}
	
	modeRadioParams.ToggleParams = radioToggleParams;
	_modeRadio = std::make_shared<GuiRadio>(modeRadioParams);

	_audioDevice = std::make_unique<AudioDevice>();

	_jobRunner = std::thread([this]() { this->_JobLoop(); });
}

std::optional<std::shared_ptr<Scene>> Scene::FromFile(SceneParams sceneParams,
	io::JamFile jamStruct,
	io::RigFile rigStruct,
	std::wstring dir)
{
	auto scene = std::make_shared<Scene>(sceneParams, rigStruct.User);

	TriggerParams trigParams;
	trigParams.Size = { 24, 24 };
	trigParams.Position = { 6, 6 };	
	trigParams.Texture = "green";
	trigParams.TextureRecording = "red";
	trigParams.TextureDitchDown = "blue";
	trigParams.TextureOverdubbing = "orange";
	trigParams.TexturePunchedIn = "purple";
	trigParams.DebounceMs = rigStruct.User.Trigger.DebounceSamps;

	StationParams stationParams;
	stationParams.Index = 0;
	stationParams.Position = { 20, 20 };
	stationParams.ModelPosition = { -50, -20 };
	stationParams.Size = { 140, 300 };
	stationParams.FadeSamps = rigStruct.User.Loop.FadeSamps > constants::MaxLoopFadeSamps ?
		constants::MaxLoopFadeSamps :
		rigStruct.User.Loop.FadeSamps;

	MergeMixBehaviourParams mergeParams;
	AudioMixerParams mixerParams = Station::GetMixerParams(stationParams.Size, mergeParams);

	for (auto& stationStruct : jamStruct.Stations)
	{
		auto station = Station::FromFile(stationParams, mixerParams, stationStruct, dir);
		if (station.has_value())
		{
			if (rigStruct.Triggers.size() > stationParams.Index)
			{
				auto trigger = Trigger::FromFile(trigParams, rigStruct.Triggers[stationParams.Index]);

				if (trigger.has_value())
					station.value()->AddTrigger(trigger.value());
			}

			scene->_AddStation(station.value());
		}

		stationParams.Index++;
		stationParams.Position += { 600, 0 };
		stationParams.ModelPosition += { 600, 0 };
	}

	scene->_SetQuantisation(jamStruct.QuantiseSamps, jamStruct.Quantisation);

	return scene;
}

void Scene::Draw(DrawContext& ctx)
{
	glDisable(GL_DEPTH_TEST);

	// Draw overlays
	auto &glCtx = dynamic_cast<GlDrawContext&>(ctx);
	glCtx.ClearMvp();
	glCtx.PushMvp(_overlayViewProj);

	_label->Draw(ctx);

	for (auto& station : _stations)
		station->Draw(ctx);

	_selector->Draw(ctx);
	_modeRadio->Draw(ctx);

	glCtx.PopMvp();
}

void Scene::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	if (PASS_SCENE == pass)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	glCtx.ClearMvp();
	glCtx.PushMvp(_viewProj);

	for (auto& station : _stations)
		station->Draw3d(ctx, 1, pass);

	glCtx.PopMvp();
}

void Scene::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_label->InitResources(resourceLib, forceInit);
	_selector->InitResources(resourceLib, forceInit);
	_modeRadio->InitResources(resourceLib, forceInit);

	for (auto& station : _stations)
		station->InitResources(resourceLib, forceInit);

	_InitSize();

	ResourceUser::_InitResources(resourceLib, forceInit);
}

void Scene::_ReleaseResources()
{
	_label->ReleaseResources();
	_selector->ReleaseResources();
	_modeRadio->ReleaseResources();

	for (auto& station : _stations)
		station->ReleaseResources();

	Drawable::_ReleaseResources();
}

ActionResult Scene::OnAction(TouchAction action)
{
	ActionResult res;
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);

	std::cout << "Touch action " << action.Touch << " [State " << action.State << "] Index " << action.Index << "(Modifiers " << action.Modifiers << ")" << std::endl;

	if (TouchAction::TouchState::TOUCH_UP == action.State)
	{
		auto activeElement = _touchDownElement.lock();

		if (activeElement)
		{
			res = activeElement->OnAction(activeElement->GlobalToLocal(action));

			if (res.IsEaten)
			{
				if (nullptr != res.Undo)
					_undoHistory.Add(res.Undo);
			}
		}
		else if (_isSceneTouching)
		{
			_isSceneTouching = false;

			// Clear selection only if not dragged
			// isSceneTouching should only be true if selector mode is SELECT_NONE
			// so try to use that instead!
			if (!_isSceneDragged)
				_UpdateSelection(ACTIONRESULT_CLEARSELECT);
		}

		// Update selector and then react to result (selecting, deselecting, muting, unmuting)
		res = _selector->OnAction(_selector->ParentToLocal(action));

		_UpdateSelection(res.ResultType);

		_touchDownElement.reset();

		return ActionResult::NoAction();
	}

	res = static_cast<std::shared_ptr<base::GuiElement>>(_modeRadio)->OnAction(_modeRadio->ParentToLocal(action));

	if (res.IsEaten)
	{
		if (nullptr != res.Undo)
			_undoHistory.Add(res.Undo);

		if (!_touchDownElement.lock())
			_touchDownElement = res.ActiveElement;

		return res;
	}

	for (auto& station : _stations)
	{
		res = static_cast<std::shared_ptr<base::GuiElement>>(station)->OnAction(station->ParentToLocal(action));

		if (res.IsEaten)
		{
			if (nullptr != res.Undo)
				_undoHistory.Add(res.Undo);

			if (!_touchDownElement.lock())
				_touchDownElement = res.ActiveElement;

			return res;
		}
	}

	res = _selector->OnAction(_selector->ParentToLocal(action));

	_UpdateSelection(res.ResultType);

	if (res.IsEaten)
	{
		if (nullptr != res.Undo)
			_undoHistory.Add(res.Undo);

		return res;
	}

	_isSceneTouching = true;
	_isSceneDragged = false;
	_initTouchDownPosition = action.Position;
	_initTouchCamPosition = _camera.ModelPosition();

	res.IsEaten = true;
	res.SourceId = "";
	res.TargetId = "";
	res.ResultType = ACTIONRESULT_ID;
	res.Undo = std::shared_ptr<ActionUndo>();
	res.ActiveElement = std::weak_ptr<GuiElement>();
	return res;
}

ActionResult Scene::OnAction(TouchMoveAction action)
{
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);

	auto activeElement = _touchDownElement.lock();

	if (activeElement)
		return activeElement->OnAction(activeElement->GlobalToLocal(action));
	else if (_isSceneTouching)
	{
		auto dPos = action.Position - _initTouchDownPosition;
		_camera.SetModelPosition(_initTouchCamPosition - Position3d{ (float)dPos.X, (float)dPos.Y, 0.0 });
		SetSize(_sizeParams.Size);

		_isSceneDragged = true;
	}
	else
	{
		auto res = static_cast<std::shared_ptr<base::GuiElement>>(_modeRadio)->OnAction(_modeRadio->ParentToLocal(action));

		if (res.IsEaten)
			return res;

		for (auto& station : _stations)
		{
			res = static_cast<std::shared_ptr<base::GuiElement>>(station)->OnAction(station->ParentToLocal(action));

			if (res.IsEaten)
				return res;
		}
	}

	return ActionResult::NoAction();
}

ActionResult Scene::OnAction(KeyAction action)
{
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	action.SetAudioParams(_audioDevice->GetAudioStreamParams());

	std::cout << "Key action " << action.KeyActionType << " [" << action.KeyChar << "] IsSytem:" << action.IsSystem << ", Modifiers:" << action.Modifiers << "]" << std::endl;

	if ((90 == action.KeyChar) && (actions::KeyAction::KEY_UP == action.KeyActionType) && (Action::MODIFIER_CTRL & action.Modifiers))
	{
		std::cout << ">> Undo <<" << std::endl;

		auto res = _undoHistory.Undo();

		return { res };
	}

	bool checkReset = false;

	for (auto& station : _stations)
	{
		auto res = station->OnAction(action);

		if (res.IsEaten)
		{
			std::cout << "KeyAction eaten: " << res.SourceId << ", " << res.TargetId << ", " << res.ResultType << std::endl;
			switch (res.ResultType)
			{
			case ACTIONRESULT_ACTIVATE:
				_isSceneReset = false;
				/*case ACTIONRESULT_ID:
					_masterLoop = std::dynamic_pointer_cast<engine::Loop>(res.IdMasterLoop);
					break;*/
			case ACTIONRESULT_DITCH:
				checkReset = true;
				break;
			}

			if (checkReset && !_isSceneReset)
			{
				unsigned int numTakes = 0;
				for (auto& station : _stations)
				{
					numTakes += station->NumTakes();
				}

				if (0 == numTakes)
					Reset();
			}

			return res;
		}
	}

	auto res = _selector->OnAction(action);

	if (res.IsEaten)
	{
		std::cout << "KeyAction eaten by selector: " << res.ResultType << std::endl;
		_UpdateSelection(res.ResultType);
		return res;
	}

	return ActionResult::NoAction();
}

ActionResult Scene::OnAction(GuiAction action)
{
	switch (action.ElementType)
	{
		case GuiAction::ACTIONELEMENT_RADIO:
			_UpdateSelectDepth(action.Index);
			std::cout << "GuiRadio called" << std::endl;
			break;
	}

	return ActionResult::NoAction();
}

void Scene::OnTick(Time curTime,
	unsigned int samps,
	std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	unsigned int totalNumLoops = 0u;

	for (auto& station : _stations)
	{
		station->OnTick(curTime,
			samps,
			_userConfig,
			_audioDevice->GetAudioStreamParams());

		totalNumLoops += station->NumTakes();
	}

	if ((0u == totalNumLoops) && !_isSceneReset)
	{
		Reset();
	}
}

void Scene::OnJobTick(Time curTime)
{
	actions::JobAction job;
	job.SetActionTime(Timer::GetTime());
	job.SetUserConfig(_userConfig);
	job.SetAudioParams(_audioDevice->GetAudioStreamParams());

	{
		std::shared_lock lock(_jobMutex);

		if (_jobList.empty())
			return;

		job = _jobList.front();
		_jobList.pop_front();

		// Run only the newest job,
		// removing previous identical jobs
		for (auto j = _jobList.begin(); j != _jobList.end();)
		{
			if (job == *j)
				j = _jobList.erase(j);
			else
				++j;
		}
	}

	auto receiver = job.Receiver.lock();
	if (receiver)
		receiver->OnAction(job);
}

void Scene::InitReceivers()
{
	_selector->SetReceiver(ActionReceiver::shared_from_this());
	_modeRadio->SetReceiver(ActionReceiver::shared_from_this());
}

void Scene::SetHover3d(std::vector<unsigned char> path, Action::Modifiers modifiers)
{
	bool isSelected = false;
	auto tweakState = base::Tweakable::TweakState::TWEAKSTATE_NONE;

	path = TrimPath(path, _selector->CurrentSelectDepth());

	auto hovering = _ChildFromPath(path);
	if (nullptr != hovering)
	{
		isSelected = hovering->IsSelected();

		if (std::shared_ptr<Tweakable> tweakable = std::dynamic_pointer_cast<Tweakable>(hovering))
			tweakState = tweakable->GetTweakState();
	}

	_selector->UpdateCurrentHover(path,
		modifiers,
		isSelected,
		tweakState);

	_UpdateSelection(ACTIONRESULT_DEFAULT);
}

void Scene::Reset()
{
	std::cout << "Reset" << std::endl;
	_clock->Clear();
	_isSceneReset = true;
}

void Scene::InitGui()
{
	_modeRadio->Init();
	_selector->Init();
}

void Scene::InitAudio()
{
	std::scoped_lock lock(_audioMutex);

	auto dev = AudioDevice::Open(Scene::AudioCallback,
		[](RtAudioError::Type type, const std::string& err) { std::cout << "[" << type << " RtAudio Error] " << err << std::endl; },
		_userConfig.Audio,
		this);

	if (dev.has_value())
	{
		_audioDevice = std::move(dev.value());

		_audioCallbackCount = 0;
		_audioDevice->Start();

		auto audioStreamParams = _audioDevice->GetAudioStreamParams();
		audioStreamParams.PrintParams();

		auto inLatency = (0u == audioStreamParams.InputLatency) ?
			_userConfig.Audio.LatencyIn :
			audioStreamParams.InputLatency;

		_channelMixer->SetParams(ChannelMixerParams({
				_userConfig.AdcBufferDelay(inLatency),
				ChannelMixer::DefaultBufferSize,
				audioStreamParams.NumInputChannels,
				audioStreamParams.NumOutputChannels }));

		for (auto& station : _stations)
		{
			if (station)
				station->SetupBuffers(audioStreamParams.NumOutputChannels,
					ChannelMixer::DefaultBufferSize);
		}
	}
}

void Scene::CloseAudio()
{
	std::scoped_lock lock(_audioMutex);
	_audioDevice->Stop();
}

void Scene::CommitChanges()
{
	std::vector<JobAction> jobList = {};

	{
		std::scoped_lock lock(_audioMutex);

		for (auto& station : _stations)
		{
			auto jobs = station->CommitChanges();
			if (!jobs.empty())
				jobList.insert(jobList.end(), jobs.begin(), jobs.end());
		}
	}

	if (!jobList.empty())
	{
		std::scoped_lock lock(_jobMutex);
		_jobList.insert(_jobList.end(), jobList.begin(), jobList.end());
	}
}

std::mutex& Scene::GetAudioMutex()
{
	return _audioMutex;
}

std::vector<unsigned char> Scene::TrimPath(std::vector<unsigned char> path, unsigned int depth)
{
	unsigned int pathLength = depth <= path.size() ?
		depth :
		(unsigned int)path.size();

	std::vector<unsigned char> curPath(path.begin(), path.begin() + pathLength);

	return curPath;
}

int Scene::AudioCallback(void* outBuffer,
	void* inBuffer,
	unsigned int numSamps,
	double streamTime,
	RtAudioStreamStatus status,
	void* userData)
{
	Scene* scene = (Scene*)userData;
	std::scoped_lock lock(scene->GetAudioMutex());
	scene->_OnAudio((float*)inBuffer, (float*)outBuffer, numSamps);

	return 0;
}

void Scene::_OnAudio(float* inBuf,
	float* outBuf,
	unsigned int numSamps)
{
	if (nullptr != inBuf)
	{
		auto audioStreamParams = nullptr == _audioDevice ?
			AudioStreamParams() : _audioDevice->GetAudioStreamParams();
		auto inLatency = (0u == audioStreamParams.InputLatency) ?
			_userConfig.Audio.LatencyIn :
			audioStreamParams.InputLatency;

		_channelMixer->FromAdc(inBuf, audioStreamParams.NumInputChannels, numSamps);

		_channelMixer->InitPlay(0u, numSamps);
		_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_MONITOR);

		for (auto& station : _stations)
		{
			_channelMixer->Source()->OnPlay(station, nullptr, 0, numSamps);
		}

		_channelMixer->InitPlay(_userConfig.AdcBufferDelay(inLatency), numSamps);
		_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_ADC);

		for (auto& station : _stations)
		{
			_channelMixer->Source()->OnPlay(station, nullptr, 0, numSamps);
		
			// Overdubbing / bouncing
			// Each trigger knows which looptakes are wired up to which
			// other looptakes (and inputs)
			// Imagine overdubbing a drum take - records from inputs 5-8
			// Then on audio, we transfer audio directly from previous loops
			// to new looptake, no wiring/mixing, so loop 1 goes to new loop 1 etc.
			// The only wiring/mixing done is from input audio.
			// We call a method on station to wind all these internal looptake bounces
			// forward, according to the triggers that are in overdub mode.
			station->SetSourceType(Audible::AUDIOSOURCE_MONITOR);
			station->OnBounce(numSamps, _userConfig);

			station->SetSourceType(Audible::AUDIOSOURCE_BOUNCE);
			station->OnBounce(numSamps, _userConfig);

			station->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_BOUNCE);
		}
	}

	_channelMixer->Source()->EndMultiPlay(numSamps);

	_channelMixer->Sink()->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);

	if (nullptr != outBuf)
	{
		auto audioStreamParams = nullptr == _audioDevice ?
			AudioStreamParams() : _audioDevice->GetAudioStreamParams();
		std::fill(outBuf, outBuf + numSamps * audioStreamParams.NumOutputChannels, 0.0f);

		for (auto& station : _stations)
		{
			station->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
			station->OnPlay(_channelMixer->Sink(), nullptr, 0, numSamps);
			station->EndMultiPlay(numSamps);
		}

		_channelMixer->ToDac(outBuf, audioStreamParams.NumOutputChannels, numSamps);
	}
	else
	{
		for (auto& station : _stations)
		{
			station->OnPlay(_channelMixer->Sink(), nullptr, 0, numSamps);
			station->EndMultiPlay(numSamps);
		}
	}
	
	_channelMixer->Sink()->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_LOOPS);

	OnTick(Timer::GetTime(),
		numSamps,
		_userConfig,
		_audioDevice->GetAudioStreamParams());
}

bool Scene::_OnUndo(std::shared_ptr<base::ActionUndo> undo)
{
	switch (undo->UndoType())
	{
	case UNDO_DOUBLE:
		auto doubleUndo = std::dynamic_pointer_cast<actions::GuiActionUndo>(undo);
		if (doubleUndo)
		{
			doubleUndo->Value();
			return true;
		}		
	}

	return false;
}

void Scene::_InitSize()
{
	auto ar = _sizeParams.Size.Height > 0 ?
		(float)_sizeParams.Size.Width / (float)_sizeParams.Size.Height :
		1.0f;
	auto projection = glm::perspective(glm::radians(80.0f), ar, 10.0f, 1000.0f);
	_viewProj = projection * _View();

	auto hScale = _sizeParams.Size.Width > 0 ? 2.0f / (float)_sizeParams.Size.Width : 1.0f;
	auto vScale = _sizeParams.Size.Height > 0 ? 2.0f / (float)_sizeParams.Size.Height : 1.0f;
	_overlayViewProj = glm::mat4(1.0);
	_overlayViewProj = glm::translate(_overlayViewProj, glm::vec3(-1.0f, -1.0f, -1.0f));
	_overlayViewProj = glm::scale(_overlayViewProj, glm::vec3(hScale, vScale, 1.0f));
}

void Scene::_UpdateSelection(ActionResultType res)
{
	// Called when touch up + down, and when hover updated
	auto currentMode = _selector->CurrentMode();
	std::shared_ptr<GuiElement> hovering = nullptr;

	std::cout << "Scene::UpdateSelection " << res << ", " << currentMode << std::endl;
	switch (res)
	{
	case ACTIONRESULT_DEFAULT:
		// Only called when hover changed or touch down
		switch (currentMode)
		{
		case GuiSelector::SELECT_NONE:
			for (auto& station : _stations)
				station->SetPicking3d(false);

			hovering = _ChildFromPath(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(true);

			break;
		case GuiSelector::SELECT_NONEADD:
			for (auto& station : _stations)
				station->SetPickingFromState(GuiElement::EDIT_SELECT, false);

			hovering = _ChildFromPath(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(!hovering->IsSelected());

			break;
		case GuiSelector::SELECT_SELECT:
			hovering = _ChildFromPath(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(true);

			break;
		case GuiSelector::SELECT_SELECTADD:
			hovering = _ChildFromPath(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(true);

			break;
		case GuiSelector::SELECT_SELECTREMOVE:
			hovering = _ChildFromPath(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(false);

			break;
		case GuiSelector::SELECT_MUTE:
			hovering = _ChildFromPath(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(true);

			break;
		case GuiSelector::SELECT_UNMUTE:
			hovering = _ChildFromPath(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(true);

			break;
		}
		break;
	case ACTIONRESULT_SELECT:
		// Only called on touch up
		for (auto& station : _stations)
		{
			station->SetStateFromPicking(GuiElement::EDIT_SELECT, false);
			station->SetPicking3d(false);
		}

		break;
	case ACTIONRESULT_MUTE:
		// Only called on touch up
		for (auto& station : _stations)
		{
			station->SetStateFromPicking(GuiElement::EDIT_MUTE, false);
			station->SetPicking3d(false);
		}

		SetHover3d(_selector->CurrentHover(), Action::MODIFIER_NONE);

		break;
	case ACTIONRESULT_UNMUTE:
		// Only called on touch up
		for (auto& station : _stations)
		{
			station->SetStateFromPicking(GuiElement::EDIT_MUTE, true);
			station->SetPicking3d(false);
		}

		SetHover3d(_selector->CurrentHover(), Action::MODIFIER_NONE);

		break;
	case ACTIONRESULT_INITSELECT:
		for (auto& station : _stations)
			station->SetPicking3d(false);

		hovering = _ChildFromPath(_selector->CurrentHover());
		if (nullptr != hovering)
			hovering->SetPicking3d(true);

		break;
	case ACTIONRESULT_CLEARSELECT:
		for (auto& station : _stations)
			station->SetPicking3d(false);

		for (auto& station : _stations)
			station->SetStateFromPicking(GuiElement::EDIT_SELECT, false);

		break;
	}
}

void Scene::InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	ResourceUser::InitResources(resourceLib, forceInit);

	_label->InitResources(resourceLib, forceInit);

	for (auto& station : _stations)
		station->InitResources(resourceLib, forceInit);
};

glm::mat4 Scene::_View()
{
	auto camPos = _camera.ModelPosition();
	return glm::lookAt(glm::vec3(camPos.X, camPos.Y, camPos.Z), glm::vec3(camPos.X, camPos.Y, 0.f), glm::vec3(0.f, 1.f, 0.f));
}

void Scene::_AddStation(std::shared_ptr<Station> station)
{
	_stations.push_back(station);

	station->SetClock(_clock);
	station->SetupBuffers(_channelMixer->Sink()->NumInputChannels(), ChannelMixer::DefaultBufferSize);
	station->Init();
}

void Scene::_SetQuantisation(unsigned int quantiseSamps, Timer::QuantisationType quantisation)
{
	_clock->SetQuantisation(quantiseSamps, quantisation);
}

void Scene::_JobLoop()
{
	while (!_isSceneQuitting)
	{
		OnJobTick(Timer::GetTime());
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

std::shared_ptr<GuiElement> Scene::_ChildFromPath(std::vector<unsigned char> path)
{
	if (path.size() < 1)
		return nullptr;

	std::shared_ptr<GuiElement> curChild;
	std::vector<unsigned char> curPath(path);

	auto stationIndex = path[0];

	if (stationIndex < _stations.size())
	{
		curChild = _stations[stationIndex];

		std::vector<unsigned char> curPath(path);

		while (nullptr != curChild)
		{
			curPath.erase(curPath.begin());

			if (curPath.empty() || (0xFF == curPath[0]))
				return curChild;

			curChild = curChild->TryGetChild(curPath[0]);
		}
	}

	return nullptr;
}

void Scene::_UpdateSelectDepth(unsigned int depth)
{
	auto selectDepth = depth > 0 ? (SelectDepth)(depth - 1) : DEPTH_STATION;
	_selector->SetSelectDepth(selectDepth);

	for (auto& station : _stations)
		station->SetSelectDepth(selectDepth);

	_UpdateSelection(ACTIONRESULT_DEFAULT);
}