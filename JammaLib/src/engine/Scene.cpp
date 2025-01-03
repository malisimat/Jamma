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
	_viewProj(glm::mat4()),
	_overlayViewProj(glm::mat4()),
	_channelMixer(std::make_shared<ChannelMixer>(ChannelMixerParams{})),
	_label(std::unique_ptr<GuiLabel>()),
	_audioDevice(std::unique_ptr<AudioDevice>()),
	_masterLoop(std::shared_ptr<Loop>()),
	_stations(),
	_touchDownElement(std::weak_ptr<GuiElement>()),
	_hoverElement3d(std::weak_ptr<GuiElement>()),
	_touchDownElement3d(std::weak_ptr<GuiElement>()),
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
	GuiLabelParams labelParams(GuiElementParams(0,
		DrawableParams{ "" },
		MoveableParams(utils::Position2d{ 10, 10 }, utils::Position3d{ 10, 10, 0 }, 1.0),
		SizeableParams{ 200,80 },
		"",
		"",
		"",
		{}), "Jamma");
	_label = std::make_unique<GuiLabel>(labelParams);

	_audioDevice = std::make_unique<AudioDevice>();

	_jobRunner = std::thread([this]() { this->JobLoop(); });
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

	for (auto stationStruct : jamStruct.Stations)
	{
		auto station = Station::FromFile(stationParams, stationStruct, dir);
		if (station.has_value())
		{
			if (rigStruct.Triggers.size() > stationParams.Index)
			{
				auto trigger = Trigger::FromFile(trigParams, rigStruct.Triggers[stationParams.Index]);

				if (trigger.has_value())
					station.value()->AddTrigger(trigger.value());
			}

			scene->AddStation(station.value());
		}

		stationParams.Index++;
		stationParams.Position += { 600, 0 };
		stationParams.ModelPosition += { 600, 0 };
	}

	scene->SetQuantisation(jamStruct.QuantiseSamps, jamStruct.Quantisation);

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

	glCtx.PopMvp();
}

void Scene::Draw3d(DrawContext& ctx,
	unsigned int numInstances)
{
	glEnable(GL_DEPTH_TEST);

	// Draw scene
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	glCtx.ClearMvp();
	glCtx.PushMvp(_viewProj);

	for (auto& station : _stations)
		station->Draw3d(ctx, 1);

	glCtx.PopMvp();
}

void Scene::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_label->InitResources(resourceLib, forceInit);

	for (auto& station : _stations)
		station->InitResources(resourceLib, forceInit);

	InitSize();

	ResourceUser::_InitResources(resourceLib, forceInit);
}

void Scene::_ReleaseResources()
{
	_label->ReleaseResources();

	for (auto& station : _stations)
		station->ReleaseResources();

	Drawable::_ReleaseResources();
}

void Scene::SetHover3d(std::vector<unsigned char> path)
{
	for (auto& station : _stations)
		station->SetHover3d(false);

	bool isHovering = path.size() > 2 ?
		(path[0] != 0) || (path[1] != 0) || (path[2] != 0) :
		false;

	if (!isHovering)
	{
		std::cout << "No hover 3d element" << std::endl;
		_hoverElement3d.reset();
		return;
	}

	bool foundChild = true;
	std::shared_ptr<GuiElement> curChild;
	auto stationIndex = path[0];

	if (stationIndex < _stations.size())
	{
		curChild = _stations[stationIndex];

		std::vector<unsigned char> curPath(path);

		while (nullptr != curChild) {
			// Remove this element's index from the front
			curPath.erase(curPath.begin());

			if (curPath.empty() || (0xFF == curPath[0]))
			{
				// Set this and all below to hovering
				curChild->SetHover3d(true);

				_hoverElement3d = curChild;

				std::cout << "SET hover 3d element" << std::endl;
				break;
			}

			auto nextChild = curChild->TryGetChild(curPath[0]);
			curChild = nextChild;
		}
	}
}

ActionResult Scene::OnAction(TouchAction action)
{
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);

	std::cout << "Touch action " << action.Touch << " [" << action.State << "] " << action.Index << std::endl;

	if (TouchAction::TouchState::TOUCH_UP == action.State)
	{
		auto activeElement = _touchDownElement.lock();
		auto activeElement3d = _touchDownElement3d.lock();

		if (activeElement)
		{
			auto res = activeElement->OnAction(activeElement->GlobalToLocal(action));

			if (res.IsEaten)
			{
				if (nullptr != res.Undo)
					_undoHistory.Add(res.Undo);
			}
		}
		else if (activeElement3d)
		{
			auto hoverElement3d = _hoverElement3d.lock();

			if (hoverElement3d == activeElement3d)
			{
				std::cout << "Matched element 3d UP with currently hovering element 3d" << std::endl;

				auto res = activeElement3d->OnAction(activeElement3d->GlobalToLocal(action));

				if (res.IsEaten)
				{
					if (nullptr != res.Undo)
						_undoHistory.Add(res.Undo);
				}
			}
		}
		else if (_isSceneTouching)
			_isSceneTouching = false;

		_touchDownElement.reset();
		_touchDownElement3d.reset();

		return { false, "", "", ACTIONRESULT_DEFAULT };
	}

	for (auto& station : _stations)
	{
		auto res = station->OnAction(station->ParentToLocal(action));

		if (res.IsEaten)
		{
			if (nullptr != res.Undo)
				_undoHistory.Add(res.Undo);

			if (!_touchDownElement.lock())
				_touchDownElement = res.ActiveElement;

			return res;
		}
		else if (_hoverElement3d.lock())
		{
			_touchDownElement3d = _hoverElement3d;
			res.ActiveElement = _hoverElement3d;
			res.IsEaten = true;

			return res;
		}
	}
	
	_isSceneTouching = true;
	_initTouchDownPosition = action.Position;
	_initTouchCamPosition = _camera.ModelPosition();

	ActionResult res;
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
	}

	return { false, "", "", ACTIONRESULT_DEFAULT };
}

ActionResult Scene::OnAction(KeyAction action)
{
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	action.SetAudioParams(_audioDevice->GetAudioStreamParams());

	std::cout << "Key action " << action.KeyActionType << " [" << action.KeyChar << "] IsSytem:" << action.IsSystem << ", Modifiers:" << action.Modifiers << "]" << std::endl;

	if ((90 == action.KeyChar) && (actions::KeyAction::KEY_UP == action.KeyActionType) && (actions::MODIFIER_CTRL == action.Modifiers))
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

	return { false, "", "", ACTIONRESULT_DEFAULT };
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

void Scene::Reset()
{
	std::cout << "Reset" << std::endl;
	_clock->Clear();
	_isSceneReset = true;
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

int Scene::AudioCallback(void* outBuffer,
	void* inBuffer,
	unsigned int numSamps,
	double streamTime,
	RtAudioStreamStatus status,
	void* userData)
{
	Scene* scene = (Scene*)userData;
	std::scoped_lock lock(scene->GetAudioMutex());
	scene->OnAudio((float*)inBuffer, (float*)outBuffer, numSamps);

	return 0;
}

void Scene::OnAudio(float* inBuf,
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
		_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_INPUT);

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

			station->EndMultiWrite(numSamps, true);
		}
	}

	_channelMixer->Source()->EndMultiPlay(numSamps);

	_channelMixer->Sink()->Zero(numSamps);

	if (nullptr != outBuf)
	{
		auto audioStreamParams = nullptr == _audioDevice ?
			AudioStreamParams() : _audioDevice->GetAudioStreamParams();
		std::fill(outBuf, outBuf + numSamps * audioStreamParams.NumOutputChannels, 0.0f);

		for (auto& station : _stations)
		{
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
	
	_channelMixer->Sink()->EndMultiWrite(numSamps, true);

	OnTick(Timer::GetTime(),
		numSamps,
		_userConfig,
		_audioDevice->GetAudioStreamParams());
}

bool Scene::OnUndo(std::shared_ptr<base::ActionUndo> undo)
{
	switch (undo->UndoType())
	{
	case UNDO_DOUBLE:
		auto doubleUndo = std::dynamic_pointer_cast<actions::DoubleActionUndo>(undo);
		if (doubleUndo)
		{
			doubleUndo->Value();
			return true;
		}		
	}

	return false;
}

void Scene::InitSize()
{
	auto ar = _sizeParams.Size.Height > 0 ?
		(float)_sizeParams.Size.Width / (float)_sizeParams.Size.Height :
		1.0f;
	auto projection = glm::perspective(glm::radians(80.0f), ar, 10.0f, 1000.0f);
	_viewProj = projection * View();

	auto hScale = _sizeParams.Size.Width > 0 ? 2.0f / (float)_sizeParams.Size.Width : 1.0f;
	auto vScale = _sizeParams.Size.Height > 0 ? 2.0f / (float)_sizeParams.Size.Height : 1.0f;
	_overlayViewProj = glm::mat4(1.0);
	_overlayViewProj = glm::translate(_overlayViewProj, glm::vec3(-1.0f, -1.0f, -1.0f));
	_overlayViewProj = glm::scale(_overlayViewProj, glm::vec3(hScale, vScale, 1.0f));
}

void Scene::InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	ResourceUser::InitResources(resourceLib, forceInit);

	_label->InitResources(resourceLib, forceInit);

	for (auto& station : _stations)
		station->InitResources(resourceLib, forceInit);
};

glm::mat4 Scene::View()
{
	auto camPos = _camera.ModelPosition();
	return glm::lookAt(glm::vec3(camPos.X, camPos.Y, camPos.Z), glm::vec3(camPos.X, camPos.Y, 0.f), glm::vec3(0.f, 1.f, 0.f));
}

void Scene::AddStation(std::shared_ptr<Station> station)
{
	_stations.push_back(station);

	station->SetClock(_clock);
	station->Init();
}

void Scene::SetQuantisation(unsigned int quantiseSamps, Timer::QuantisationType quantisation)
{
	_clock->SetQuantisation(quantiseSamps, quantisation);
}

void Scene::JobLoop()
{
	while (!_isSceneQuitting)
	{
		OnJobTick(Timer::GetTime());
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}
