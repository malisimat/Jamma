#include "Scene.h"
#include <iostream>
#include <set>
#include "glm/ext.hpp"
#include "../io/WavReadWriter.h"
#include "../io/TextReadWriter.h"
#include "../utils/PathUtils.h"
#include "../graphics/VstEditorWindow.h"
#include "../midi/MidiTimestampMapper.h"
#include "../vst/Vst3Plugin.h"

using namespace base;
using namespace actions;
using namespace audio;
using namespace engine;
using namespace gui;
using namespace io;
using namespace midi;
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
	_viewRotOnlyProj(glm::mat4()),
	_skyboxViewProj(glm::mat4()),
	_skyboxStarted(false),
	_skyboxStartTime(Timer::GetZero()),
	_channelMixer(std::make_shared<ChannelMixer>(ChannelMixerParams{})),
	_label(nullptr),
	_selector(nullptr),
	_modeRadio(nullptr),
	_audioDevice(nullptr),
	_quantisation(),
	_midiRouter(),
	_loggingConfig{},
	_stations(),
	_ninjamController(),
	_touchDownElement(std::weak_ptr<GuiElement>()),
	_hoverElement3d(std::weak_ptr<GuiElement>()),
	_hoverPath3d(),
	_isMidiPhaseDragging(false),
	_midiPhaseDragStartPosition{},
	_midiPhaseDragStartOffsetSamps(0),
	_audioSampleCounter(0u),
	_midiAnchorMicros(0),
	_camera(CameraParams(
		MoveableParams(
			Position2d{ 0,0 },
			Position3d{ 0, 0, 420 },
			1.0),
		0)),
	_userConfig(user),
	_viewMode(VIEW_STATION)
{
	_quantisation.SetClock(std::make_shared<Timer>());
	_quantisation.SetSeedUsesPowers(_userConfig.Loop.SeedUsesPowers);

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
	_selector->SetSelectDepth(base::DEPTH_STATION);

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
	_PublishAudioStations();

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
	stationParams.Size = { 136, 300 };
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
				{
					if (rigStruct.Triggers[stationParams.Index].MidiTrigger.has_value())
						scene->_RegisterMidiTriggerRoute(
							rigStruct.Triggers[stationParams.Index].MidiTrigger->Device,
							trigger.value());
					station.value()->AddTrigger(trigger.value());
				}
			}

			scene->_AddStation(station.value());
		}

		stationParams.Index++;
		stationParams.Position += { 600, 0 };
		stationParams.ModelPosition += { 600, 0 };
	}

	scene->_SetQuantisation(jamStruct.QuantiseSamps, jamStruct.Quantisation);
	scene->_ninjamController.LoadConfig(jamStruct.Ninjam);
	scene->InitReceivers();

	return scene;
}

void Scene::CloseAllVstEditorWindows()
{
	for (auto& window : _vstEditorWindows)
	{
		if (window)
			window->Destroy();
	}
	_vstEditorWindows.clear();
}

void Scene::_PruneClosedVstEditorWindows()
{
	_vstEditorWindows.erase(std::remove_if(_vstEditorWindows.begin(), _vstEditorWindows.end(), [](const std::unique_ptr<graphics::VstEditorWindow>& window) {
		return !window || !window->IsOpen();
	}), _vstEditorWindows.end());
}

bool Scene::_OpenVstEditorForPlugin(const std::shared_ptr<vst::IVstPlugin>& plugin)
{
	if (!plugin || !plugin->IsLoaded())
		return false;

	auto window = std::make_unique<graphics::VstEditorWindow>();
	const auto hInstance = GetModuleHandle(nullptr);
	if (!window->Create(hInstance, plugin))
		return false;

	_vstEditorWindows.push_back(std::move(window));
	return true;
}

bool Scene::_TryOpenVstEditorForLoop(const std::shared_ptr<Loop>& loop, size_t pluginIndex)
{
	if (!loop)
		return false;

	auto plugin = loop->GetVstPlugin(pluginIndex);
	if (!plugin || !plugin->IsLoaded())
		return false;

	return _OpenVstEditorForPlugin(plugin);
}

bool Scene::_TryOpenVstEditorForStation(const std::shared_ptr<Station>& station, size_t pluginIndex)
{
	if (!station || station->IsRemote())
		return false;

	auto stationPlugin = station->GetVstPlugin(pluginIndex);
	if (stationPlugin && stationPlugin->IsLoaded())
		return _OpenVstEditorForPlugin(stationPlugin);

	for (const auto& take : station->GetLoopTakes())
	{
		for (const auto& loop : take->GetLoops())
		{
			if (_TryOpenVstEditorForLoop(loop, pluginIndex))
				return true;
		}
	}

	return false;
}

bool Scene::_TryOpenVstEditorForHover(const std::shared_ptr<base::GuiElement>& hovering,
	base::SelectDepth depth,
	size_t pluginIndex)
{
	if (!hovering)
		return false;

	switch (depth)
	{
	case base::SelectDepth::DEPTH_STATION:
	{
		auto station = std::dynamic_pointer_cast<Station>(hovering);
		return _TryOpenVstEditorForStation(station, pluginIndex);
	}
	case base::SelectDepth::DEPTH_LOOPTAKE:
	{
		auto take = std::dynamic_pointer_cast<LoopTake>(hovering);
		if (!take)
			return false;

		for (const auto& loop : take->GetLoops())
		{
			if (_TryOpenVstEditorForLoop(loop, pluginIndex))
				return true;
		}

		return false;
	}
	case base::SelectDepth::DEPTH_LOOP:
	{
		auto loop = std::dynamic_pointer_cast<Loop>(hovering);
		return _TryOpenVstEditorForLoop(loop, pluginIndex);
	}
	default:
		return false;
	}
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

	if (PASS_SCENE == pass)
	{
		if (!_skyboxStarted)
		{
			_skyboxStartTime = Timer::GetTime();
			_skyboxStarted = true;
		}
		{
			auto t = (float)Timer::GetElapsedSeconds(_skyboxStartTime, Timer::GetTime());
			auto yaw   = glm::radians(2.0f * std::sin(0.047f * t) + 1.5f * std::sin(0.031f * t + 1.1f));
			auto pitch = glm::radians(1.5f * std::sin(0.053f * t + 2.3f) + 1.0f * std::sin(0.019f * t + 0.7f));
			auto roll  = glm::radians(0.8f * std::sin(0.037f * t + 1.8f));
			auto R = glm::rotate(glm::mat4(1.0f), yaw,   glm::vec3(0.0f, 1.0f, 0.0f));
			R = glm::rotate(R, pitch, glm::vec3(1.0f, 0.0f, 0.0f));
			R = glm::rotate(R, roll,  glm::vec3(0.0f, 0.0f, 1.0f));
			_skyboxViewProj = _viewRotOnlyProj * R;
		}

		glCtx.ClearMvp();
		glCtx.PushMvp(_skyboxViewProj);
		_skybox.Draw(glCtx);
		glCtx.PopMvp();
	}

	glCtx.ClearMvp();
	glCtx.PushMvp(_viewProj);

	if (PASS_SCENE == pass)
		_ApplyQuantisationOverlayAlpha(_QuantisationOverlayAlpha(Timer::GetTime()));

	for (auto& station : _stations)
		station->Draw3d(ctx, 1, pass);

	glCtx.PopMvp();
}

void Scene::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	_skybox.InitResources(resourceLib, forceInit);
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
	_skybox.ReleaseResources();
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

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State)
		&& (0 == action.Index)
		&& (Action::MODIFIER_SHIFT & action.Modifiers)
		&& !(Action::MODIFIER_CTRL & action.Modifiers)
		&& _TrySetMasterFromHover(true))
	{
		res.IsEaten = true;
		res.ResultType = ACTIONRESULT_DEFAULT;
		return res;
	}

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State)
		&& (0 == action.Index)
		&& _IsMidiPhaseDragModifier(action.Modifiers))
		return _BeginMidiPhaseDrag(action);

	if (_isMidiPhaseDragging)
	{
		if (TouchAction::TouchState::TOUCH_UP == action.State)
			return _EndMidiPhaseDrag(action);

		res.IsEaten = true;
		res.ResultType = ACTIONRESULT_DEFAULT;
		return res;
	}

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
	res.ResultType = ACTIONRESULT_DEFAULT;
	res.Undo = std::shared_ptr<ActionUndo>();
	res.ActiveElement = std::weak_ptr<GuiElement>();
	return res;
}

ActionResult Scene::OnAction(TouchMoveAction action)
{
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);

	if (_isMidiPhaseDragging)
		return _UpdateMidiPhaseDrag(action);

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

	if (17 == action.KeyChar)
		_SetQuantisationOverlayHeld(actions::KeyAction::KEY_DOWN == action.KeyActionType);

	if ((32 == action.KeyChar) && (actions::KeyAction::KEY_UP == action.KeyActionType))
	{
		_PulseQuantisationOverlay();
		_HandleTapTempo(action.GetActionTime());
		return ActionResult::NoAction();
	}

	// Ctrl+Shift+R - arm one-shot reclock: clear quantisation so the next
	// completed recording becomes the new master quantisation. After completion the
	// derived BPM/BPI is queued and sent to the NINJAM server at the next
	// interval boundary.
	if ((82 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		_HandleReclockArm();
		return ActionResult::NoAction();
	}

	if ((90 == action.KeyChar) && (actions::KeyAction::KEY_UP == action.KeyActionType) && (Action::MODIFIER_CTRL & action.Modifiers))
	{
		return _HandleUndo();
	}

	// Ctrl+Shift+V - insert a VST on the hovered station/take/loop.
	if ((86 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		const auto pluginPath = utils::PickFile(L"Choose VST plugin");
		if (pluginPath.empty())
			return ActionResult::NoAction();

		auto hovering = _ChildFromPath(_selector->CurrentHover());
		return _HandleVstInsert(pluginPath, _selector->CurrentSelectDepth(), hovering);
	}

	// Ctrl+Shift+E - open the first plugin editor for the hovered station/take/loop.
	if ((69 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		return _HandleVstEditorOpen();
	}

	// Ctrl+S - export session to directory.
	if ((83 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers))
	{
		return _HandleExportSession();
	}

	bool checkReset = false;
	auto result = ActionResult::NoAction();

	for (auto& station : _stations)
	{
		auto res = station->OnAction(action);

		if (!res.IsEaten)
			continue;

		std::cout << "KeyAction eaten: " << res.SourceId << ", " << res.TargetId << ", " << res.ResultType << std::endl;
		switch (res.ResultType)
		{
		case ACTIONRESULT_ACTIVATE:
			_isSceneReset.store(false, std::memory_order_relaxed);
			checkReset = true;
			// Propagate any grain the clock just acquired (e.g. from first-loop seed).
			// _SetQuantisation is not called by Station's TrySeedClockFromFirstLoop, so
			// do it here after every activation so all LoopTakes see the current grain.
			if (auto clock = _quantisation.Clock())
				_SetMidiQuantisationGrain(clock->QuantiseSamps(), "loop activated");
			break;
		case ACTIONRESULT_DITCH:
			checkReset = true;
			break;
		default:
			break;
		}

		if (!result.IsEaten || (res.ResultType != ACTIONRESULT_DEFAULT))
			result = res;
	}

	if (checkReset)
		_ResetIfEmpty();

	if (result.IsEaten)
		return result;

	auto res = _selector->OnAction(action);

	if (res.IsEaten)
	{
		std::cout << "KeyAction eaten by selector: " << res.ResultType << std::endl;
		_UpdateSelection(res.ResultType);
		return res;
	}

	return ActionResult::NoAction();
}

void Scene::_HandleReclockArm()
{
	std::cout << ">> Reclock armed (Ctrl+Shift+R) <<" << std::endl;
	_quantisation.ArmReclock();
	_quantisation.SetMidiGrain(0u, "reclock arm", _stations);
}

ActionResult Scene::_HandleUndo()
{
	std::cout << ">> Undo <<" << std::endl;
	auto res = _undoHistory.Undo();
	return { res };
}

ActionResult Scene::_HandleVstInsert(const std::wstring& pluginPath,
	base::SelectDepth depth,
	const std::shared_ptr<base::GuiElement>& hovering)
{
	if (!hovering)
	{
		std::cout << "VST insert: no hovered target" << std::endl;
		return ActionResult::NoAction();
	}

	std::cout << "VST insert request: depth=" << static_cast<int>(depth)
		<< ", path=" << utils::EncodeUtf8(pluginPath) << std::endl;

	switch (depth)
	{
	case base::SelectDepth::DEPTH_STATION:
	{
		auto station = std::dynamic_pointer_cast<Station>(hovering);
		if (!station)
			return ActionResult::NoAction();

		std::cout << "VST insert target: station '" << station->Name() << "' (busChannels=" << station->NumBusChannels() << ")" << std::endl;

		if (station->IsRemote())
		{
			std::cout << "VST insert: remote stations are read-only" << std::endl;
			return ActionResult::NoAction();
		}

		station->LoadVstPlugin(pluginPath);
		CommitChanges();
		break;
	}
	case base::SelectDepth::DEPTH_LOOPTAKE:
	{
		auto take = std::dynamic_pointer_cast<LoopTake>(hovering);
		if (!take)
			return ActionResult::NoAction();

		std::cout << "VST insert target: looptake (numLoops=" << take->GetLoops().size() << ")" << std::endl;

		take->LoadVstPlugin(pluginPath);
		CommitChanges();
		break;
	}
	case base::SelectDepth::DEPTH_LOOP:
	{
		auto loop = std::dynamic_pointer_cast<Loop>(hovering);
		if (!loop)
			return ActionResult::NoAction();

		std::cout << "VST insert target: single loop" << std::endl;

		loop->LoadVstPlugin(pluginPath);
		CommitChanges();
		break;
	}
	default:
		return ActionResult::NoAction();
	}

	std::cout << "VST inserted: " << utils::EncodeUtf8(pluginPath) << std::endl;

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = actions::ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult Scene::_HandleVstEditorOpen()
{
	auto hovering = _ChildFromPath(_selector->CurrentHover());
	_PruneClosedVstEditorWindows();

	auto eatAction = []() {
		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	};

	if (_TryOpenVstEditorForHover(hovering, _selector->CurrentSelectDepth(), 0))
		return eatAction();

	for (const auto& station : _stations)
	{
		if (_TryOpenVstEditorForStation(station, 0))
			return eatAction();
	}

	std::cout << "VST editor open: no loaded plugin found" << std::endl;
	return ActionResult::NoAction();
}

ActionResult Scene::_HandleExportSession()
{
	struct AudioPauseGuard
	{
		explicit AudioPauseGuard(audio::AudioDevice* device) : Device(device), WasPlaying(device && device->Pause()) {}
		~AudioPauseGuard() { Resume(); }

		void Resume()
		{
			if (WasPlaying && Device)
			{
				Device->Resume();
				WasPlaying = false;
			}
		}

		audio::AudioDevice* Device;
		bool WasPlaying;
	};

	struct LoopSnapshot
	{
		std::wstring Path;
		std::vector<float> Samples;
	};

	const auto exportDir = utils::PickDirectory(L"Choose export directory");
	if (exportDir.empty())
		return ActionResult::NoAction();

	const auto streamSampleRate = _audioDevice->GetAudioStreamParams().SampleRate;
	const auto sampleRate = (streamSampleRate == 0u) ? _userConfig.Audio.SampleRate : streamSampleRate;

	io::JamFile jam;
	jam.Version = io::JamFile::VERSION_V;
	jam.Name = "export";
	jam.Ninjam = _ninjamController.Config();
	jam.TimerTicks = 0;
	jam.QuantiseSamps = 0;
	jam.Quantisation = engine::Timer::QUANTISE_OFF;

	std::vector<LoopSnapshot> loops;

	{
		AudioPauseGuard pause(_audioDevice.get());
		std::scoped_lock lock(_audioMutex);

		for (const auto& station : _stations)
		{
			if (station->IsRemote())
				continue;

			io::JamFile::Station jamStation;
			jamStation.Name = station->Name();
			jamStation.StationType = 0;
			jamStation.VstChain = station->VstEntries();

			for (const auto& take : station->GetLoopTakes())
			{
				io::JamFile::LoopTake jamTake;
				jamTake.Name = take->Id();
				jamTake.VstChain = take->VstEntries();

				for (const auto& loop : take->GetLoops())
				{
					const auto wavFilename = loop->Id() + ".wav";

					auto samples = loop->ExportSamples();
					if (samples.empty())
						continue;

					jamTake.Loops.push_back(loop->ToJamFile(wavFilename));

					LoopSnapshot snap;
					snap.Path = exportDir + L"\\" + utils::DecodeUtf8(wavFilename);
					snap.Samples = std::move(samples);
					loops.push_back(std::move(snap));
				}

				if (!jamTake.Loops.empty())
					jamStation.LoopTakes.push_back(std::move(jamTake));
			}

			jam.Stations.push_back(std::move(jamStation));
		}
	}

	if (jam.Stations.empty())
	{
		std::cout << "Export: nothing to export" << std::endl;
		return ActionResult::NoAction();
	}

	io::WavReadWriter wavWriter;
	unsigned int wavCount = 0;
	for (const auto& loop : loops)
	{
		if (wavWriter.Write(loop.Path, loop.Samples, (unsigned int)loop.Samples.size(), sampleRate))
			++wavCount;
	}

	std::stringstream jamStream;
	io::JamFile::ToStream(jam, jamStream);
	const auto jamPath = exportDir + L"\\session.jam";
	const auto wroteJamFile = io::TextReadWriter().Write(jamPath, jamStream.str(), 0, 0);
	if (!wroteJamFile)
	{
		std::cout << "Export: failed to write session.jam to "
			<< utils::EncodeUtf8(jamPath) << std::endl;
		return ActionResult::NoAction();
	}

	std::cout << "Exported " << wavCount << " loop(s) + session.jam to "
		<< utils::EncodeUtf8(exportDir) << std::endl;

	return ActionResult::NoAction();
}

ActionResult Scene::OnAction(GuiAction action)
{
	switch (action.ElementType)
	{
		case GuiAction::ACTIONELEMENT_RADIO:
			if (auto i = std::get_if<GuiAction::GuiInt>(&action.Data))
			{
				_viewMode = (ViewMode)i->Value;
				_UpdateSelectDepth((unsigned int)_viewMode);
			}
			break;
	}

	return ActionResult::NoAction();
}

void Scene::OnTick(Time curTime,
	unsigned int samps,
	std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	if (auto clock = _quantisation.Clock())
		clock->Tick(samps, 0u);

	unsigned int totalNumLoops = 0u;
	const auto stationsSnapshot = _audioStations.load(std::memory_order_acquire);
	static const std::vector<std::shared_ptr<Station>> emptyStations;
	const auto& stations = stationsSnapshot ? *stationsSnapshot : emptyStations;

	for (auto& station : stations)
	{
		station->OnTick(curTime,
			samps,
			_userConfig,
			_audioDevice->GetAudioStreamParams());

		totalNumLoops += station->NumTakes();
	}

	if ((0u == totalNumLoops) && !_isSceneReset.load(std::memory_order_relaxed))
	{
		_ClearTimingState(false);
		_isSceneReset.store(true, std::memory_order_relaxed);
	}
}

void Scene::OnJobTick(Time curTime)
{
	_PumpMidi();
	_PumpSerial();

	auto snapshot = _ninjamController.Pump();
	{
		// Always sync the station clock state to the scene-level quantisation.
		// This ensures that when the first loop seeds the station clock locally
		// (without a NINJAM session), _effectiveQuantiseSamps is updated promptly.
		std::scoped_lock lock(_audioMutex);
		_QueueLocalTempoFromClock();
		if (snapshot.has_value())
		{
			_SendQueuedTempoAtIntervalWrap(snapshot.value());
			_ApplyRemoteTempoToClock(snapshot.value());
		}
	}

	actions::JobAction job;
	job.SetActionTime(Timer::GetTime());
	job.SetUserConfig(_userConfig);
	job.SetAudioParams(_audioDevice->GetAudioStreamParams());

	{
		std::scoped_lock lock(_jobMutex);

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

	// Hand any PreInit'd VST plugin (created on the UI thread) back to the UI
	// thread for destruction. On success the chain holds its own ref so this
	// queued ref is a no-op; on failure (Load returned false) this is the only
	// remaining ref, and draining it on the UI thread ensures ~Vst3Plugin →
	// IComponent::terminate() / FreeLibrary run on the thread that PreInit'd
	// the plugin. Releasing on this job thread instead violates VST3 threading
	// and can crash plugins or leave dangling state until window close.
	if (job.PreInitPlugin)
		vst::QueueForUiThreadDestroy(std::move(job.PreInitPlugin));
}

void Scene::_PumpMidi()
{
	// Trigger-driven engine mutation (record start/end, overdub, ditch, MIDI
	// loop writes) happens here via Trigger::OnEvent and LoopTake::RecordMidiEvent.
	// Hold _audioMutex so mutation cannot interleave with Scene::CommitChanges()
	// publishing back buffers on the main thread - this prevents partial
	// multi-channel record-start visibility that produced block-quantised
	// loop start offsets across channels.
	std::scoped_lock lock(_audioMutex);
	const auto globalSampleNow = static_cast<std::uint32_t>(_audioSampleCounter.load(std::memory_order_acquire));
	auto summary = _midiRouter.PumpMidi(_stations,
		globalSampleNow,
		_userConfig,
		_audioDevice->GetAudioStreamParams());
	if (summary.Activated)
	{
		_isSceneReset.store(false, std::memory_order_relaxed);
		if (auto clock = _quantisation.Clock())
			_SetMidiQuantisationGrain(clock->QuantiseSamps(), "loop activated");
	}
	if (summary.Ditched)
		_ResetIfEmpty();
}

void Scene::_RegisterMidiTriggerRoute(const std::string& deviceName, std::shared_ptr<Trigger> trigger)
{
	_midiRouter.RegisterTrigger(deviceName, std::move(trigger));
}

void Scene::_PumpSerial()
{
	// See _PumpMidi: trigger dispatch must be serialised with CommitChanges
	// publication to keep multi-channel record-start coherent.
	std::scoped_lock lock(_audioMutex);
	auto summary = _midiRouter.PumpSerial(_stations, _userConfig, _audioDevice->GetAudioStreamParams());
	if (summary.Activated)
	{
		_isSceneReset.store(false, std::memory_order_relaxed);
		if (auto clock = _quantisation.Clock())
			_SetMidiQuantisationGrain(clock->QuantiseSamps(), "loop activated");
	}
	if (summary.Ditched)
		_ResetIfEmpty();
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
	std::vector<unsigned char> fullElementPath;
	std::vector<unsigned char> elementPath;

	for (auto segment : path)
	{
		if (0 == segment)
			break;

		fullElementPath.push_back(segment - 1);
	}

	_hoverPath3d = fullElementPath;
	_hoverElement3d = _ChildFromPath(fullElementPath);
	elementPath = fullElementPath;

	elementPath = TrimPath(elementPath, _selector->CurrentSelectDepth() + 1);

	auto hovering = _ChildFromPath(elementPath);
	if (nullptr != hovering)
	{
		isSelected = hovering->IsSelected();

		if (std::shared_ptr<Tweakable> tweakable = std::dynamic_pointer_cast<Tweakable>(hovering))
			tweakState = tweakable->GetTweakState();
	}

	_selector->UpdateCurrentHover(elementPath,
		modifiers,
		isSelected,
		tweakState);

	_UpdateSelection(ACTIONRESULT_DEFAULT);

	auto candidate = (Action::MODIFIER_SHIFT & modifiers) ? _ChildFromPath(elementPath) : nullptr;
	_UpdateStationQuantisation(candidate, _selector->CurrentSelectDepth(), false);
}

void Scene::Reset()
{
	std::cout << "Reset" << std::endl;
	_ClearTimingState(true);
	_ClearStationQuantisation();
	_quantisation.ClearOverlay();
	_isSceneReset.store(true, std::memory_order_relaxed);
}

void Scene::InitGui()
{
	_modeRadio->Init();
	_selector->Init();
}

void Scene::InitAudio()
{
	{
		std::scoped_lock lock(_audioMutex);

		auto dev = AudioDevice::Open(Scene::AudioCallback,
			[](RtAudioError::Type type, const std::string& err) { std::cout << "[" << type << " RtAudio Error] " << err << std::endl; },
			_userConfig.Audio,
			this);

		if (dev.has_value())
		{
			_audioDevice = std::move(dev.value());
			_audioSampleCounter.store(0u, std::memory_order_release);

			auto audioStreamParams = _audioDevice->GetAudioStreamParams();

			auto inLatency = (0u == audioStreamParams.InputLatency) ?
				_userConfig.Audio.LatencyIn :
				audioStreamParams.InputLatency;

			_channelMixer->SetParams(ChannelMixerParams({
					_userConfig.AdcBufferDelay(inLatency) + audioStreamParams.BufSize,
					ChannelMixer::DefaultBufferSize,
					audioStreamParams.NumInputChannels,
					audioStreamParams.NumOutputChannels }));

			for (auto& station : _stations)
			{
				if (station)
				{
					station->SetupBuffers(audioStreamParams.BufSize);
					station->SetSampleRate(static_cast<float>(audioStreamParams.SampleRate));
					station->SetNumAdcChannels(audioStreamParams.NumInputChannels);
					station->SetNumDacChannels(audioStreamParams.NumOutputChannels);
					//station->SetNumBusChannels(audioStreamParams.NumOutputChannels);
				}
			}
			_ninjamController.SetAudioFormat(
				audioStreamParams.SampleRate,
				audioStreamParams.BufSize,
				audioStreamParams.NumInputChannels,
				audioStreamParams.NumOutputChannels);

			InitMidi();
			InitSerial();
			_audioDevice->Start();
			_audioDevice->GetAudioStreamParams().PrintParams();
		}
	}

	// Dispatch any VST loads that were staged during scene FromFile so they run
	// with the real audio device sample rate and buffer size.
	CommitChanges();
}

void Scene::SetLogging(io::LoggingConfig config) noexcept
{
	_loggingConfig = config;
	for (auto& station : _stations)
	{
		if (station)
			station->SetLogging(_loggingConfig);
	}
}

void Scene::InitMidi()
{
	_midiRouter.InitMidi(_userConfig, _loggingConfig, _audioSampleCounter, _midiAnchorMicros);
}

void Scene::CloseMidi()
{
	_midiRouter.CloseMidi();
}

void Scene::InitSerial()
{
	_midiRouter.InitSerial(_userConfig);
}

void Scene::CloseSerial()
{
	_midiRouter.CloseSerial();
}

void Scene::CloseAudio()
{
	CloseSerial();
	CloseMidi();

	// Do not hold the audio callback mutex while stopping the stream.
	// RtAudio shutdown may wait for the callback thread to return, and the
	// callback takes this same mutex.
	if (_audioDevice)
		_audioDevice->Stop();

	std::scoped_lock lock(_audioMutex);

	_ninjamController.Stop();

	_audioDevice->Stop();
}

void Scene::Shutdown()
{
	_isSceneQuitting.store(true, std::memory_order_release);
	if (_jobRunner.joinable())
		_jobRunner.join();

	CloseAudio();
}

void Scene::CommitChanges()
{
	std::vector<JobAction> syncJobs = {};
	std::vector<JobAction> jobList = {};
	std::optional<ninjam::NinjamRemoteSnapshot> pendingRemoteSnapshot = _ninjamController.TakePendingSnapshot();

	{
		std::scoped_lock lock(_audioMutex);

		if (pendingRemoteSnapshot.has_value())
			_UpdateRemoteStationsFromSnapshot(pendingRemoteSnapshot.value());

		for (auto& station : _stations)
		{
			auto jobs = station->CommitChanges();
			if (!jobs.empty())
			{
				for (auto& job : jobs)
				{
					switch (job.JobActionType)
					{
					case JobAction::JOB_UPDATELOOPS:
					case JobAction::JOB_ENDRECORDING:
						syncJobs.push_back(std::move(job));
						break;
					default:
						jobList.push_back(std::move(job));
						break;
					}
				}
			}
		}
	}

	for (auto& job : syncJobs)
	{
		auto receiver = job.Receiver.lock();
		if (receiver)
			receiver->OnAction(job);
	}

	// Pre-initialise VST DLLs on the UI thread before handing jobs to the job
	// thread. Do this after releasing _audioMutex so LoadLibraryW stays out of
	// the audio lock and later attached() calls remain UI-thread bound.
	// MakePluginForPath selects VST3 (Vst3Plugin) or VST2 (Vst2Plugin) by
	// file extension (.dll → VST2, anything else → VST3).
	for (auto& job : jobList)
	{
		if (job.JobActionType == JobAction::JOB_LOADVST)
		{
			auto plugin = vst::MakePluginForPath(job.VstPath);
			if (plugin->PreInit(job.VstPath))
				job.PreInitPlugin = std::move(plugin);
			// If PreInit fails, fall back to a fresh plugin on the job thread.
		}
	}

	if (!jobList.empty())
	{
		std::scoped_lock lock(_jobMutex);
		_jobList.insert(_jobList.end(), jobList.begin(), jobList.end());
	}
}

void Scene::SendNinjamChat(const std::string& msg)
{
	_ninjamController.SendChat(msg);
}

void Scene::ConnectNinjam(const std::string& host)
{
	_ninjamController.Connect(host);
}

void Scene::DisconnectNinjam()
{
	_ninjamController.Disconnect();
}

std::shared_ptr<StationRemote> Scene::FindRemoteStation(const std::vector<std::shared_ptr<Station>>& stations,
	const std::string& userName)
{
	for (const auto& station : stations)
	{
		auto remote = std::dynamic_pointer_cast<StationRemote>(station);
		if (remote && remote->RemoteUserName() == userName)
			return remote;
	}

	return nullptr;
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
	scene->_OnAudio((float*)inBuffer, (float*)outBuffer, numSamps);

	return 0;
}

void Scene::_OnAudio(float* inBuf,
	float* outBuf,
	unsigned int numSamps)
{
	const auto audioStreamParams = nullptr == _audioDevice ?
		AudioStreamParams() : _audioDevice->GetAudioStreamParams();
	const auto blockStartSample = _audioSampleCounter.load(std::memory_order_relaxed);
	const auto stationsSnapshot = _audioStations.load(std::memory_order_acquire);
	static const std::vector<std::shared_ptr<Station>> emptyStations;
	const auto& stations = stationsSnapshot ? *stationsSnapshot : emptyStations;

	if (nullptr != inBuf)
	{
		auto inLatency = (0u == audioStreamParams.InputLatency) ?
			_userConfig.Audio.LatencyIn :
			audioStreamParams.InputLatency;

		_channelMixer->FromAdc(inBuf, audioStreamParams.NumInputChannels, numSamps);

		_channelMixer->InitPlay(0u, numSamps);
		_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_MONITOR);

		for (auto& station : stations)
		{
			if (station->IsRemote())
				continue;

			_channelMixer->WriteToSink(station, numSamps);
		}

		_channelMixer->InitPlay(_userConfig.AdcBufferDelay(inLatency), numSamps);
		_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_ADC);

		for (auto& station : stations)
		{
			if (station->IsRemote())
				continue;

			_channelMixer->WriteToSink(station, numSamps);
		
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
			station->OnBounce(numSamps, _userConfig, audioStreamParams);

			station->SetSourceType(Audible::AUDIOSOURCE_BOUNCE);
			station->OnBounce(numSamps, _userConfig, audioStreamParams);

			station->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_BOUNCE);
		}
	}

	_channelMixer->Source()->EndMultiPlay(numSamps);

	_channelMixer->Sink()->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
	_ninjamController.ProcessAudioBlock(inBuf, numSamps, audioStreamParams.SampleRate);

	auto ingestRemoteStation = [&](const std::shared_ptr<Station>& stationBase) {
		auto station = std::dynamic_pointer_cast<StationRemote>(stationBase);
		if (!station || !station->IsConnectedRemote())
			return;

		const float* left = nullptr;
		const float* right = nullptr;
		unsigned int frameCount = 0u;
		if (_ninjamController.ConsumeStereoPair(station->AssignedOutputChannel(), left, right, frameCount))
		{
			auto ingestFrames = frameCount < numSamps ? frameCount : numSamps;
			station->IngestStereoBlock(left, right, ingestFrames);
		}
	};

	if (nullptr != outBuf)
	{
		std::fill(outBuf, outBuf + numSamps * audioStreamParams.NumOutputChannels, 0.0f);

		for (auto& station : stations)
		{
			station->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
			ingestRemoteStation(station);
			station->WriteBlock(_channelMixer->Sink(), nullptr, 0, numSamps,
				static_cast<std::uint32_t>(blockStartSample));
			station->EndMultiPlay(numSamps);
		}

		_channelMixer->ToDac(outBuf, audioStreamParams.NumOutputChannels, numSamps);
	}
	else
	{
		for (auto& station : stations)
		{
			ingestRemoteStation(station);
			station->WriteBlock(_channelMixer->Sink(), nullptr, 0, numSamps,
				static_cast<std::uint32_t>(blockStartSample));
			station->EndMultiPlay(numSamps);
		}
	}
	
	_channelMixer->Sink()->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_LOOPS);

	OnTick(Timer::GetTime(),
		numSamps,
		_userConfig,
		_audioDevice->GetAudioStreamParams());

	_audioSampleCounter.store(blockStartSample + numSamps, std::memory_order_release);
	_midiAnchorMicros.store(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);
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
	_viewRotOnlyProj = projection * glm::mat4(glm::mat3(_View()));
	// _skyboxViewProj is updated in Draw3d(); do not reset it here

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

	// Stations can be added after scene resources are initialised.
	auto stations = _stations;
	for (auto& station : stations)
	{
		if (station)
			station->InitResources(resourceLib, forceInit);
	}
}

glm::mat4 Scene::_View()
{
	auto camPos = _camera.ModelPosition();
	return glm::lookAt(glm::vec3(camPos.X, camPos.Y, camPos.Z), glm::vec3(camPos.X, camPos.Y, 0.f), glm::vec3(0.f, 1.f, 0.f));
}

void Scene::_AddStation(std::shared_ptr<Station> station)
{
	station->SetLogging(_loggingConfig);
	station->SetClock(_quantisation.Clock());
	station->SetupBuffers(ChannelMixer::DefaultBufferSize);
	station->SetNumAdcChannels(_channelMixer->Source()->NumOutputChannels(Audible::AUDIOSOURCE_ADC));
	station->SetNumDacChannels(_channelMixer->Sink()->NumInputChannels(Audible::AUDIOSOURCE_LOOPS));
	station->Init();

	auto selectDepth = (unsigned int)_viewMode;
	if (selectDepth > (unsigned int)DEPTH_LOOP)
		selectDepth = (unsigned int)DEPTH_LOOP;

	station->SetSelectDepth((SelectDepth)selectDepth);

	_stations.push_back(station);
	_PublishAudioStations();
}

void Scene::_SetQuantisation(unsigned int quantiseSamps, Timer::QuantisationType quantisation)
{
	_quantisation.SetSeedUsesPowers(_userConfig.Loop.SeedUsesPowers);
	_quantisation.Set(quantiseSamps, quantisation);
	_quantisation.SetMidiGrain(quantiseSamps, "scene quantisation set", _stations);
}

void Scene::_SetMidiQuantisationGrain(unsigned int grainSamps, const char* source)
{
	_quantisation.SetMidiGrain(grainSamps, source, _stations);
}

bool Scene::_IsMidiPhaseDragModifier(base::Action::Modifiers modifiers) const noexcept
{
	return (Action::MODIFIER_CTRL & modifiers)
		&& (Action::MODIFIER_SHIFT & modifiers);
}

ActionResult Scene::_BeginMidiPhaseDrag(TouchAction action)
{
	_isMidiPhaseDragging = true;
	_midiPhaseDragStartPosition = action.Position;
	_midiPhaseDragStartOffsetSamps = _quantisation.GlobalPhaseOffsetSamps();
	_SetQuantisationOverlayHeld(true);
	_ApplyQuantisationOverlayAlpha(1.0f);

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult Scene::_UpdateMidiPhaseDrag(TouchMoveAction action)
{
	const auto delta = action.Position - _midiPhaseDragStartPosition;
	const auto offsetSamps = Quantisation::ResolvePhaseOffsetDrag(_midiPhaseDragStartOffsetSamps,
		delta.X,
		_CurrentSampleRate());
	_quantisation.SetGlobalPhaseOffsetSamps(offsetSamps, _stations);
	_ApplyQuantisationOverlayAlpha(1.0f);

	ActionResult res;
	res.IsEaten = true;
	res.ResultType = ACTIONRESULT_DEFAULT;
	return res;
}

ActionResult Scene::_EndMidiPhaseDrag(TouchAction action)
{
	if (_isMidiPhaseDragging)
	{
		const auto delta = action.Position - _midiPhaseDragStartPosition;
		const auto offsetSamps = Quantisation::ResolvePhaseOffsetDrag(_midiPhaseDragStartOffsetSamps,
			delta.X,
			_CurrentSampleRate());
		_quantisation.SetGlobalPhaseOffsetSamps(offsetSamps, _stations);
	}

	_isMidiPhaseDragging = false;
	_SetQuantisationOverlayHeld(false);
	_PulseQuantisationOverlay();
	_ApplyQuantisationOverlayAlpha(_QuantisationOverlayAlpha(Timer::GetTime()));

	return ActionResult::NoAction();
}

void Scene::_ClearTimingState(bool clearTapTempo)
{
	_quantisation.Clear(clearTapTempo);
	_quantisation.SetMidiGrain(0u, "timing clear", _stations);
}

void Scene::_ResetIfEmpty()
{
	if (_isSceneReset.load(std::memory_order_relaxed))
		return;
	unsigned int total = 0u;
	for (const auto& s : _stations)
		total += s->NumTakes();
	if (0u == total)
		Reset();
}

void Scene::_JobLoop()
{
	while (!_isSceneQuitting.load(std::memory_order_acquire))
	{
		OnJobTick(Timer::GetTime());
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

void Scene::_PublishAudioStations()
{
	auto stations = std::make_shared<const std::vector<std::shared_ptr<Station>>>(_stations.begin(), _stations.end());
	_audioStations.store(stations, std::memory_order_release);
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
	if (depth > (unsigned int)DEPTH_LOOP)
		depth = (unsigned int)DEPTH_LOOP;

	auto selectDepth = (SelectDepth)depth;
	_selector->SetSelectDepth(selectDepth);

	for (auto& station : _stations)
		station->SetSelectDepth(selectDepth);

	_UpdateSelection(ACTIONRESULT_DEFAULT);
}

void Scene::_UpdateRemoteStationsFromSnapshot(const ninjam::NinjamRemoteSnapshot& snapshot)
{
	std::set<std::string> seenUsers;

	for (const auto& remoteUser : snapshot.Users)
	{
		seenUsers.insert(remoteUser.UserName);

		auto remoteStation = Scene::FindRemoteStation(_stations, remoteUser.UserName);

		if (!remoteStation)
		{
			StationParams stationParams;
			stationParams.Name = remoteUser.UserName;
			stationParams.Size = { 200, 280 };
			stationParams.Index = static_cast<unsigned int>(_stations.size());
			stationParams.Position = {
				static_cast<int>(stationParams.Index) * 600,
				0 };
			stationParams.ModelPosition = {
				static_cast<float>(stationParams.Index) * 600.0f,
				0.0f,
				0.0f };

			audio::MergeMixBehaviourParams merge;
			auto mixerParams = Station::GetMixerParams(stationParams.Size, merge);
			remoteStation = std::make_shared<StationRemote>(stationParams, mixerParams);
			remoteStation->SetRemoteUserName(remoteUser.UserName);
			remoteStation->SetNumBusChannels(2);
			remoteStation->SetNumDacChannels(2);
			_AddStation(remoteStation);
			std::cout << "[NINJAM] User joined: " << remoteUser.UserName << std::endl;
		}

		remoteStation->SetAssignedOutputChannel(remoteUser.AssignedOutputChannel);
		remoteStation->SetRemoteChannelCount(remoteUser.ChannelCount);
		remoteStation->SetConnectedRemote(true);

		if (snapshot.IntervalLengthSamps > 0)
		{
			auto visualIntervalSamps = snapshot.IntervalLengthSamps;
			if (snapshot.HasTiming)
			{
				const auto derivedInterval = IntervalSampsFromTempo(snapshot.Bpm,
					static_cast<unsigned int>(snapshot.Bpi),
					snapshot.SampleRate);
				if (derivedInterval > 0u)
					visualIntervalSamps = std::max(visualIntervalSamps, derivedInterval);
			}

			remoteStation->SetRemoteInterval(snapshot.IntervalLengthSamps, snapshot.IntervalPositionSamps, visualIntervalSamps);
		}

		// EnsureRemoteTake is run from Scene::CommitChanges on the main thread
		// so scene/station graph mutation stays single-threaded.
		remoteStation->EnsureRemoteTake();
		remoteStation->UpdateRemoteVisuals();
	}

	// Remove stations for users who have left.
	bool stationsChanged = false;
	for (auto it = _stations.begin(); it != _stations.end();)
	{
		auto remoteStation = std::dynamic_pointer_cast<StationRemote>(*it);
		if (!remoteStation)
		{
			++it;
			continue;
		}

		if (seenUsers.find(remoteStation->RemoteUserName()) == seenUsers.end())
		{
			remoteStation->SetConnectedRemote(false);
			std::cout << "[NINJAM] User left: " << remoteStation->RemoteUserName() << std::endl;
			it = _stations.erase(it);
			stationsChanged = true;
		}
		else
		{
			++it;
		}
	}

	if (stationsChanged)
		_PublishAudioStations();
}

QuantisationPolicy Scene::_QuantisationPolicy() const
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = _userConfig.Loop.SeedGrainMinMs;
	policy.SeedGrainTargetMaxMs = _userConfig.Loop.SeedGrainTargetMaxMs;
	policy.SeedBpmMin = _userConfig.Loop.SeedBpmMin;
	policy.SeedUsesPowers = _userConfig.Loop.SeedUsesPowers;
	return policy;
}

unsigned int Scene::_CurrentSampleRate() const
{
	if (_audioDevice)
	{
		const auto streamRate = _audioDevice->GetAudioStreamParams().SampleRate;
		if (streamRate > 0u)
			return streamRate;
	}

	if (_userConfig.Audio.SampleRate > 0u)
		return _userConfig.Audio.SampleRate;

	return constants::DefaultSampleRate;
}

std::uint64_t Scene::_EstimatedAudioSampleAt(Time actionTime) const
{
	const auto sampleRate = _CurrentSampleRate();
	const auto anchorSample = _audioSampleCounter.load(std::memory_order_acquire);
	const auto anchorMicros = _midiAnchorMicros.load(std::memory_order_acquire);
	const auto actionMicros = std::chrono::duration_cast<std::chrono::microseconds>(
		actionTime.time_since_epoch()).count();

	return MapMidiTimestampToAudioSample(sampleRate,
		anchorSample,
		anchorMicros,
		actionMicros);
}

void Scene::_ApplyQuantisationTiming(const QuantisationTiming& timing, const char* source)
{
	_quantisation.SetSeedUsesPowers(_userConfig.Loop.SeedUsesPowers);
	_quantisation.ApplyTiming(timing, source);
	_quantisation.SetMidiGrain(timing.SeedSamps, source, _stations);
}

bool Scene::_HandleTapTempo(Time actionTime)
{
	const auto handled = _quantisation.HandleTapTempo(_EstimatedAudioSampleAt(actionTime),
		_CurrentSampleRate(),
		_stations,
		_userConfig);
	if (handled)
		_UpdateStationQuantisation(nullptr, _selector->CurrentSelectDepth(), false);
	return handled;
}

void Scene::_PulseQuantisationOverlay()
{
	_quantisation.PulseOverlay();
}

void Scene::_SetQuantisationOverlayHeld(bool held)
{
	_quantisation.SetOverlayHeld(held);
}

float Scene::_QuantisationOverlayAlpha(Time now) const
{
	return _quantisation.OverlayAlpha(now);
}

void Scene::_ApplyQuantisationOverlayAlpha(float alpha)
{
	_quantisation.ApplyOverlayAlpha(alpha, _stations);
}

bool Scene::_TrySetMasterFromHover(bool confirm)
{
	return _quantisation.TrySetMasterFromHover(_ChildFromPath(_selector->CurrentHover()),
		_selector->CurrentSelectDepth(),
		_stations,
		_CurrentSampleRate(),
		_userConfig,
		confirm);
}

void Scene::_UpdateStationQuantisation(std::shared_ptr<base::GuiElement> candidate,
	base::SelectDepth depth,
	bool confirmCandidate)
{
	_quantisation.UpdateStationHints(candidate, depth, confirmCandidate, _stations);
}

void Scene::_ClearStationQuantisation()
{
	_quantisation.ClearStationHints(_stations);
}

void Scene::_ApplyRemoteTempoToClock(const ninjam::NinjamRemoteSnapshot& snapshot)
{
	_quantisation.ApplyRemoteTempo(snapshot, _stations, _userConfig);
}

void Scene::_QueueLocalTempoFromClock()
{
	_quantisation.QueueLocalTempo(_quantisation.RemoteSampleRate(), _CurrentSampleRate(), _userConfig);
}

void Scene::_SendQueuedTempoAtIntervalWrap(const ninjam::NinjamRemoteSnapshot& snapshot)
{
	_quantisation.SendQueuedTempo(snapshot,
		_ninjamController.Session(),
		_quantisation.RemoteSampleRate(),
		_CurrentSampleRate());
}
