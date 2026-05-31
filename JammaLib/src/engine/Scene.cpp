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
using namespace graphics;
using namespace resources;
using namespace utils;
using namespace std::placeholders;

namespace
{
	constexpr std::uint8_t kUnresolvedMidiDeviceSlot = 0xffu;
	constexpr double QuantisationOverlayFadeSeconds = 2.0;

	template<typename T>
	void AppendUniqueTarget(std::vector<std::shared_ptr<T>>& targets,
		const std::shared_ptr<T>& candidate)
	{
		if (!candidate)
			return;

		if (std::find(targets.begin(), targets.end(), candidate) == targets.end())
			targets.push_back(candidate);
	}

	const char* MidiActionLabel(actions::ActionResultType rt)
	{
		switch (rt)
		{
		case actions::ACTIONRESULT_ACTIVATE: return "Activate";
		case actions::ACTIONRESULT_DITCH:    return "Ditch";
		case actions::ACTIONRESULT_TOGGLE:   return "Toggle";
		default:                             return "Action";
		}
	}

	const char* MidiEventDirection(const engine::MidiEvent& event)
	{
		if (event.IsNoteOn())  return " Down";
		if (event.IsNoteOff()) return " Up";
		return "";
	}

	void LogMidiEventDetail(std::ostream& out, std::uint8_t deviceSlot, const engine::MidiEvent& event)
	{
		constexpr std::uint8_t CC            = 0xB0;
		constexpr std::uint8_t ProgramChange = 0xC0;

		out << "dev: " << (deviceSlot + 1) << ", chan " << (event.Channel() + 1) << ", ";

		switch (event.MessageType())
		{
		case engine::MidiEvent::NoteOn:
			out << (event.data2 != 0 ? "noteon" : "noteoff") << ": " << static_cast<int>(event.data1);
			break;
		case engine::MidiEvent::NoteOff:
			out << "noteoff: " << static_cast<int>(event.data1);
			break;
		case CC:
			out << "cc " << static_cast<int>(event.data1) << ": " << static_cast<int>(event.data2);
			break;
		case ProgramChange:
			out << "pc: " << static_cast<int>(event.data1);
			break;
		default:
			out << "0x" << std::hex << std::uppercase << static_cast<int>(event.status) << std::dec;
			break;
		}
	}

	std::shared_ptr<StationRemote> FindRemoteStation(const std::vector<std::shared_ptr<Station>>& stations,
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
}

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
	_midiInputs(),
	_loggingConfig{},
	_serialDevices(),
	_midiTriggerRoutes(),
	_midiTriggerRoutesSnapshot(),
	_lastSerialDropCount(0u),
	_masterLoop(nullptr),
	_masterLoopLengthSamps(0ul),
	_tapTempo(),
	_quantisationOverlayHeld(false),
	_quantisationOverlayLastActive(Timer::GetZero()),
	_pendingQuantisationOverlayPulse(false),
	_stations(),
	_ninjamConfig(std::nullopt),
	_ninjamSession(std::make_unique<NinjamSession>()),
	_touchDownElement(std::weak_ptr<GuiElement>()),
	_hoverElement3d(std::weak_ptr<GuiElement>()),
	_hoverPath3d(),
	_dragLoopTakeTargets(),
	_audioSampleCounter(0u),
	_midiAnchorMicros(0),
	_camera(CameraParams(
		MoveableParams(
			Position2d{ 0,0 },
			Position3d{ 0, 0, 420 },
			1.0),
		0)),
	_userConfig(user),
	_clock(std::make_shared<Timer>()),
	_viewMode(VIEW_STATION)
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
	_midiInputs.store(std::make_shared<const std::vector<std::shared_ptr<MidiInputEndpoint>>>(), std::memory_order_release);
	_PublishMidiTriggerRoutes();

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
	scene->_ninjamConfig = jamStruct.Ninjam;
	if (jamStruct.Ninjam.has_value())
		scene->_ninjamSession->Start(jamStruct.Ninjam.value());
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
	{
		if (_pendingQuantisationOverlayPulse.exchange(false, std::memory_order_acq_rel))
			_PulseQuantisationOverlay();

		_ApplyQuantisationOverlayAlpha(_QuantisationOverlayAlpha(Timer::GetTime()));
	}

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

	if ((TouchAction::TouchState::TOUCH_UP == action.State)
		&& !_dragLoopTakeTargets.empty())
	{
		for (const auto& take : _dragLoopTakeTargets)
		{
			if (!take)
				continue;

			auto takeRes = take->OnAction(take->GlobalToLocal(action));
			if (takeRes.IsEaten && (nullptr != takeRes.Undo))
				_undoHistory.Add(takeRes.Undo);
		}

		res = _selector->OnAction(_selector->ParentToLocal(action));
		_UpdateSelection(res.ResultType);
		_dragLoopTakeTargets.clear();
		_touchDownElement.reset();
		return ActionResult::NoAction();
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

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State)
		&& (0 == action.Index)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		_dragLoopTakeTargets = _CurrentLoopTakeInteractionTargets();
		for (const auto& take : _dragLoopTakeTargets)
		{
			if (!take)
				continue;

			auto takeRes = take->BeginMidiQuantisationGesture(action);

			if (takeRes.IsEaten)
			{
				res = takeRes;
				if (nullptr != takeRes.Undo)
					_undoHistory.Add(takeRes.Undo);
			}
		}

		if (res.IsEaten)
			return res;

		_dragLoopTakeTargets.clear();
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

	if (!_dragLoopTakeTargets.empty())
	{
		ActionResult res = ActionResult::NoAction();
		for (const auto& take : _dragLoopTakeTargets)
		{
			if (!take)
				continue;

			auto takeRes = take->OnAction(take->GlobalToLocal(action));
			if (takeRes.IsEaten)
				res = takeRes;
		}

		return res;
	}

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
		std::cout << ">> Reclock armed (Ctrl+Shift+R) <<" << std::endl;
		{
			if (_clock)
				_clock->Clear();
			_masterLoopLengthSamps.store(0ul, std::memory_order_release);
		}
		{
			std::scoped_lock tapTempoLock(_tapTempoMutex);
			_tapTempo.Clear();
		}
		_armReclock.store(true, std::memory_order_release);
		_effectiveQuantiseSamps.store(0u, std::memory_order_release);
		_hasPendingTempo.store(false, std::memory_order_release);
		return ActionResult::NoAction();
	}

	if ((90 == action.KeyChar) && (actions::KeyAction::KEY_UP == action.KeyActionType) && (Action::MODIFIER_CTRL & action.Modifiers))
	{
		std::cout << ">> Undo <<" << std::endl;

		auto res = _undoHistory.Undo();

		return { res };
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
		if (!hovering)
		{
			std::cout << "VST insert: no hovered target" << std::endl;
			return ActionResult::NoAction();
		}

		std::cout << "VST insert request: depth=" << static_cast<int>(_selector->CurrentSelectDepth())
			<< ", path=" << utils::EncodeUtf8(pluginPath) << std::endl;

		switch (_selector->CurrentSelectDepth())
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
		}

		std::cout << "VST inserted: " << utils::EncodeUtf8(pluginPath) << std::endl;

		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}

	// Ctrl+Shift+E - open the first plugin editor for the hovered station/take/loop.
	if ((69 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
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

	// Ctrl+S - export session to directory.
	if ((83 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers))
	{
		struct LoopSnapshot
		{
			std::wstring Path;
			std::vector<float> Samples;
		};

		struct LoopRef
		{
			std::shared_ptr<Loop> Loop;
			std::string WavFilename;
		};

		struct TakeRef
		{
			std::vector<LoopRef> Loops;
		};

		struct StationRef
		{
			std::vector<TakeRef> Takes;
		};

		const auto exportDir = utils::PickDirectory(L"Choose export directory");
		if (exportDir.empty())
			return ActionResult::NoAction();

		const auto streamSampleRate = _audioDevice->GetAudioStreamParams().SampleRate;
		const auto sampleRate = (streamSampleRate == 0u) ? _userConfig.Audio.SampleRate : streamSampleRate;

		io::JamFile jam;
		jam.Version = io::JamFile::VERSION_V;
		jam.Name = "export";
		jam.Ninjam = _ninjamConfig;
		jam.TimerTicks = 0;
		jam.QuantiseSamps = 0;
		jam.Quantisation = engine::Timer::QUANTISE_OFF;

		std::vector<StationRef> stationRefs;
		std::vector<LoopSnapshot> loops;

		{
			std::scoped_lock lock(_audioMutex);

			for (const auto& station : _stations)
			{
				if (station->IsRemote())
					continue;

				StationRef stationRef;

				io::JamFile::Station jamStation;
				jamStation.Name = station->Name();
				jamStation.StationType = 0;
				jamStation.VstChain = station->VstEntries();

				for (const auto& take : station->GetLoopTakes())
				{
					TakeRef takeRef;

					io::JamFile::LoopTake jamTake;
					jamTake.Name = take->Id();
					jamTake.VstChain = take->VstEntries();

					for (const auto& loop : take->GetLoops())
					{
						const auto wavFilename = loop->Id() + ".wav";
						takeRef.Loops.push_back({ loop, wavFilename });
						jamTake.Loops.push_back(loop->ToJamFile(wavFilename));
					}

					if (!takeRef.Loops.empty())
						stationRef.Takes.push_back(std::move(takeRef));

					if (!jamTake.Loops.empty())
						jamStation.LoopTakes.push_back(std::move(jamTake));
				}

				if (!stationRef.Takes.empty())
					stationRefs.push_back(std::move(stationRef));

				if (!jamStation.LoopTakes.empty())
					jam.Stations.push_back(std::move(jamStation));
			}
		}

		if (jam.Stations.empty())
		{
			std::cout << "Export: nothing to export" << std::endl;
			return ActionResult::NoAction();
		}

		// Copy loop samples outside the audio lock to avoid callback stalls.
		for (const auto& stationRef : stationRefs)
		{
			for (const auto& takeRef : stationRef.Takes)
			{
				for (const auto& loopRef : takeRef.Loops)
				{
					auto samples = loopRef.Loop->ExportSamples();
					if (samples.empty())
						continue;

					LoopSnapshot snap;
					snap.Path = exportDir + L"\\" + utils::DecodeUtf8(loopRef.WavFilename);
					snap.Samples = std::move(samples);
					loops.push_back(std::move(snap));
				}
			}
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
			if (_clock)
				_SetMidiQuantisationGrain(_clock->QuantiseSamps(), "loop activated");
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

	if (checkReset && !_isSceneReset.load(std::memory_order_relaxed))
	{
		unsigned int numTakes = 0;
		for (auto& station : _stations)
		{
			numTakes += station->NumTakes();
		}

		if (0 == numTakes)
			Reset();
	}

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
	if (_clock)
		_clock->Tick(samps, 0u);

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

	auto snapshot = _ninjamSession->Pump();
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

	if (snapshot.has_value())
	{
		// Scene graph changes must be applied on the main/render thread.
		std::scoped_lock snapshotLock(_remoteSnapshotMutex);
		_pendingRemoteSnapshot = snapshot.value();
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

	MidiEvent ingress{};
	const auto globalSampleNow = static_cast<std::uint32_t>(_audioSampleCounter.load(std::memory_order_acquire));
	const auto midiInputs = _midiInputs.load(std::memory_order_acquire);
	const auto stationsSnapshot = _audioStations.load(std::memory_order_acquire);
	static const std::vector<std::shared_ptr<Station>> emptyStations;
	const auto& stations = stationsSnapshot ? *stationsSnapshot : emptyStations;

	if (!midiInputs)
		return;

	for (const auto& input : *midiInputs)
	{
		if (!input)
			continue;

		while (input->Ingress.Pop(ingress))
		{
			_DispatchMidiTriggerEvent(input->DeviceSlot, ingress);

			const auto msgType = ingress.MessageType();
			if ((msgType >= 0x80u) && (msgType <= 0xE0u))
			{
				const auto& deviceName = input->ConfiguredName;
				for (const auto& station : stations)
				{
					if (station && !station->IsRemote() && station->AcceptsLiveMidiFromDevice(deviceName))
						station->EnqueueLiveMidiEvent(ingress);
				}
			}

			if ((msgType != MidiEvent::NoteOn) && (msgType != MidiEvent::NoteOff))
				continue;

			// Trigger routing runs before loop recording, but both consume the same
			// ingress event in this loop, so a trigger consuming the action does not
			// suppress MIDI loop recording.
			for (const auto& station : stations)
			{
				for (const auto& take : station->GetLoopTakes())
				{
					if (take->IsArmed())
						take->RecordMidiEvent(ingress, input->ConfiguredName, globalSampleNow);
				}
			}
		}

		auto dropped = input->Ingress.DroppedCount();
		if (dropped != input->LastDroppedCount)
		{
			std::cout << "[MIDI] Ingress queue dropped " << (dropped - input->LastDroppedCount)
				<< " event(s) on device \"" << input->ConfiguredName
				<< "\", total dropped=" << dropped << std::endl;
			input->LastDroppedCount = dropped;
		}
	}
}

void Scene::_PushMidiEvent(std::uint8_t deviceSlot,
	std::uint8_t status,
	std::uint8_t data1,
	std::uint8_t data2,
	unsigned int sampleRate) noexcept
{
	auto midiInputs = _midiInputs.load(std::memory_order_acquire);
	if (!midiInputs)
		return;

	for (const auto& input : *midiInputs)
	{
		if (!input || (input->DeviceSlot != deviceSlot))
			continue;

		MidiEvent ingress{};
		const auto nowMicros = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		const auto anchorSample = _audioSampleCounter.load(std::memory_order_acquire);
		const auto anchorMicros = _midiAnchorMicros.load(std::memory_order_acquire);
		const auto mappedSample = MapMidiTimestampToAudioSample(sampleRate,
			anchorSample,
			anchorMicros,
			nowMicros);

		ingress.sampleOffset = static_cast<std::uint32_t>(mappedSample);
		ingress.status = status;
		ingress.data1 = data1;
		ingress.data2 = data2;
		ingress._pad = 0u;
		input->Ingress.Push(ingress);
		break;
	}
}

void Scene::_DispatchMidiTriggerEvent(std::uint8_t deviceSlot,
	const MidiEvent& event)
{
	Action triggerAction;
	triggerAction.SetUserConfig(_userConfig);
	if (_audioDevice)
		triggerAction.SetAudioParams(_audioDevice->GetAudioStreamParams());
	triggerAction.SetActionTime(Timer::GetTime());

	auto routes = _midiTriggerRoutesSnapshot.load(std::memory_order_acquire);
	if (!routes)
		return;

	for (const auto& route : *routes)
	{
		if ((route.DeviceSlot != deviceSlot) || !route.Trigger)
			continue;

		auto res = route.Trigger->OnEvent(event, triggerAction);
		if (!res.IsEaten)
			continue;

		std::cout << "[MIDI Trigger] trigger=\"" << route.Trigger->Name()
			<< "\" " << MidiActionLabel(res.ResultType)
			<< MidiEventDirection(event) << " (";
		LogMidiEventDetail(std::cout, route.DeviceSlot, event);
		std::cout << ")\n";
	}
}

void Scene::_RegisterMidiTriggerRoute(const std::string& deviceName, std::shared_ptr<Trigger> trigger)
{
	if (!trigger)
		return;

	_midiTriggerRoutes.push_back({ deviceName.empty() ? "default" : deviceName, kUnresolvedMidiDeviceSlot, trigger });
	_PublishMidiTriggerRoutes();
}

void Scene::_PublishMidiTriggerRoutes()
{
	auto routes = std::make_shared<const std::vector<MidiTriggerRoute>>(_midiTriggerRoutes.begin(), _midiTriggerRoutes.end());
	_midiTriggerRoutesSnapshot.store(routes, std::memory_order_release);
}

void Scene::_PumpSerial()
{
	// See _PumpMidi: trigger dispatch must be serialised with CommitChanges
	// publication to keep multi-channel record-start coherent.
	std::scoped_lock lock(_audioMutex);

	static const std::string kEmptyDevice;
	while (true)
	{
		io::SerialTriggerEvent ev{};
		{
			std::scoped_lock lock(_serialIngressMutex);
			if (!_serialIngress.Pop(ev))
				break;
		}

		base::Action action;
		action.SetActionTime(Timer::GetTime());
		action.SetUserConfig(_userConfig);
		action.SetAudioParams(_audioDevice->GetAudioStreamParams());
		const auto& device = ev.Device ? *ev.Device : kEmptyDevice;

		for (auto& station : _stations)
		{
			station->OnTriggerEvent(
				TriggerSource::TRIGGER_SERIAL,
				ev.ButtonIndex,
				ev.IsPressed ? 1u : 0u,
				action,
				device);
		}
	}

	std::uint64_t dropped = 0u;
	{
		std::scoped_lock lock(_serialIngressMutex);
		dropped = _serialIngress.DroppedCount();
	}
	if (dropped != _lastSerialDropCount)
	{
		std::cout << "[Serial] Ingress queue dropped " << (dropped - _lastSerialDropCount)
			<< " event(s), total dropped=" << dropped << std::endl;
		_lastSerialDropCount = dropped;
	}
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
	_masterLoop.reset();
	_ClearStationQuantisation();
	_quantisationOverlayHeld = false;
	_quantisationOverlayLastActive = Timer::GetZero();
	_pendingQuantisationOverlayPulse.store(false, std::memory_order_release);
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
			_ninjamSession->SetAudioFormat(
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
	CloseMidi();

	_midiAnchorMicros.store(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);

	if (_userConfig.Midi.Devices.empty())
	{
		std::cout << "[MIDI] No MIDI devices configured." << std::endl;
		_PublishMidiTriggerRoutes();
		return;
	}

	const auto sampleRate = _userConfig.Audio.SampleRate;
	auto midiInputs = std::make_shared<std::vector<std::shared_ptr<MidiInputEndpoint>>>();
	std::uint8_t nextSlot = 0u;

	for (const auto& midiConfig : _userConfig.Midi.Devices)
	{
		if (!midiConfig.Enabled)
		{
			std::cout << "[MIDI] Device \"" << midiConfig.Name << "\" disabled by rig settings." << std::endl;
			continue;
		}

		if (nextSlot == kUnresolvedMidiDeviceSlot)
		{
			std::cout << "[MIDI] Too many enabled MIDI input devices; remaining devices ignored." << std::endl;
			break;
		}

		auto endpoint = std::make_shared<MidiInputEndpoint>();
		endpoint->ConfiguredName = midiConfig.Name.empty() ? "default" : midiConfig.Name;
		endpoint->Device = std::make_unique<MidiDevice>();

		auto opened = endpoint->Device->Open(
			endpoint->ConfiguredName,
			[endpoint, sampleRate, audioSampleCounter = &_audioSampleCounter, midiAnchorMicros = &_midiAnchorMicros](std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
			{
				MidiEvent ingress{};
				const auto nowMicros = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				const auto anchorSample = audioSampleCounter->load(std::memory_order_acquire);
				const auto anchorMicros = midiAnchorMicros->load(std::memory_order_acquire);
				const auto mappedSample = MapMidiTimestampToAudioSample(sampleRate,
					anchorSample,
					anchorMicros,
					nowMicros);

				ingress.sampleOffset = static_cast<std::uint32_t>(mappedSample);
				ingress.status = status;
				ingress.data1 = data1;
				ingress.data2 = data2;
				ingress._pad = 0u;
				endpoint->Ingress.Push(ingress);
			},
			_loggingConfig.Midi == "verbose");

		if (!opened)
			continue;

		endpoint->DeviceSlot = nextSlot++;
		midiInputs->push_back(endpoint);
	}

	_midiInputs.store(midiInputs, std::memory_order_release);

	std::set<std::string> activeMidiInputNames;
	for (const auto& input : *midiInputs)
	{
		if (input)
			activeMidiInputNames.insert(input->ConfiguredName);
	}

	for (auto& route : _midiTriggerRoutes)
	{
		route.DeviceSlot = kUnresolvedMidiDeviceSlot;
		for (const auto& input : *midiInputs)
		{
			if (input && (input->ConfiguredName == route.DeviceName))
			{
				route.DeviceSlot = input->DeviceSlot;
				break;
			}
		}

		if (route.DeviceSlot == kUnresolvedMidiDeviceSlot)
			std::cout << "[MIDI] No active MIDI input matches trigger device \"" << route.DeviceName << "\"." << std::endl;

		if (route.Trigger)
		{
			for (const auto& midiInputDevice : route.Trigger->MidiInputDevices())
			{
				if (!midiInputDevice.empty() && (activeMidiInputNames.find(midiInputDevice) == activeMidiInputNames.end()))
				{
					std::cout << "[MIDI] No active MIDI input matches loop-record device \""
						<< midiInputDevice << "\" for trigger \"" << route.Trigger->Name() << "\"." << std::endl;
				}
			}
		}
	}

	_PublishMidiTriggerRoutes();

	if (midiInputs->empty())
		std::cout << "[MIDI] No active MIDI input connection." << std::endl;

}

void Scene::CloseMidi()
{
	for (auto& route : _midiTriggerRoutes)
		route.DeviceSlot = kUnresolvedMidiDeviceSlot;
	_PublishMidiTriggerRoutes();

	auto midiInputs = _midiInputs.exchange(std::make_shared<const std::vector<std::shared_ptr<MidiInputEndpoint>>>(), std::memory_order_acq_rel);
	if (!midiInputs)
		return;

	for (const auto& input : *midiInputs)
	{
		if (input && input->Device)
			input->Device->Close();
	}
}

void Scene::InitSerial()
{
	CloseSerial();
	{
		std::scoped_lock lock(_serialIngressMutex);
		_serialIngress.Clear();
	}
	_lastSerialDropCount = 0u;

	if (_userConfig.Serial.Devices.empty())
		return;

	auto availablePorts = io::SerialDevice::EnumeratePorts();
	std::cout << "[Serial] Ports found: " << availablePorts.size() << std::endl;
	for (const auto& port : availablePorts)
		std::cout << "[Serial]   " << port << std::endl;

	unsigned int activeConnections = 0u;
	for (const auto& serialConfig : _userConfig.Serial.Devices)
	{
		if (!serialConfig.Enabled)
		{
			std::cout << "[Serial] Device \"" << serialConfig.Name << "\" disabled by rig settings." << std::endl;
			continue;
		}

		if (serialConfig.Port.empty())
		{
			std::cout << "[Serial] Device \"" << serialConfig.Name << "\" has no port configured." << std::endl;
			continue;
		}

		auto serialDevice = std::make_unique<io::SerialDevice>();
		auto opened = serialDevice->Open(
			serialConfig.Name,
			serialConfig.Port,
			serialConfig.BaudRate,
			[this](const io::SerialTriggerEvent& event)
			{
				std::scoped_lock lock(_serialIngressMutex);
				_serialIngress.Push(event);
			});

		if (!opened)
			continue;

		_serialDevices.push_back(std::move(serialDevice));
		activeConnections++;
	}

	if (0u == activeConnections)
		std::cout << "[Serial] No active serial trigger connections." << std::endl;
}

void Scene::CloseSerial()
{
	for (auto& serialDevice : _serialDevices)
	{
		if (serialDevice)
			serialDevice->Close();
	}

	_serialDevices.clear();
	{
		std::scoped_lock lock(_serialIngressMutex);
		_serialIngress.Clear();
	}
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

	_ninjamSession->Stop();

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
	std::optional<io::NinjamRemoteSnapshot> pendingRemoteSnapshot;
	{
		std::scoped_lock snapshotLock(_remoteSnapshotMutex);
		if (_pendingRemoteSnapshot.has_value())
		{
			pendingRemoteSnapshot = std::move(_pendingRemoteSnapshot.value());
			_pendingRemoteSnapshot.reset();
		}
	}

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
	if (_ninjamSession)
		_ninjamSession->SendChat(msg);
}

void Scene::ConnectNinjam(const std::string& host)
{
	if (!_ninjamSession)
		return;

	io::JamFile::NinjamConfig config;
	if (_ninjamConfig.has_value())
		config = _ninjamConfig.value();

	config.Host = host;
	_ninjamConfig = config;

	std::cout << "[NINJAM] Connecting to " << host << std::endl;
	_ninjamSession->Start(config);
}

void Scene::DisconnectNinjam()
{
	if (!_ninjamSession)
		return;

	std::cout << "[NINJAM] Disconnecting" << std::endl;
	_ninjamSession->Stop();
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
	_ninjamSession->ProcessAudioBlock(inBuf, numSamps, audioStreamParams.SampleRate);

	auto ingestRemoteStation = [&](const std::shared_ptr<Station>& stationBase) {
		auto station = std::dynamic_pointer_cast<StationRemote>(stationBase);
		if (!station || !station->IsConnectedRemote())
			return;

		const float* left = nullptr;
		const float* right = nullptr;
		unsigned int frameCount = 0u;
		if (_ninjamSession->ConsumeStereoPair(station->AssignedOutputChannel(), left, right, frameCount))
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
	station->SetClock(_clock);
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
	_clock->SetQuantisation(quantiseSamps, quantisation);
	_effectiveQuantiseSamps.store(quantiseSamps, std::memory_order_release);
	_SetMidiQuantisationGrain(quantiseSamps, "scene quantisation set");
}

void Scene::_SetMidiQuantisationGrain(unsigned int grainSamps, const char* source)
{
	unsigned int takeCount = 0u;
	for (const auto& station : _stations)
	{
		if (!station)
			continue;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;

			MidiQuantisationSettings settings = take->MidiQuantisation();
			if (settings.GrainSamps != grainSamps)
			{
				settings.GrainSamps = grainSamps;
				take->SetMidiQuantisation(settings);
			}
			++takeCount;
		}
	}

	if (_loggingConfig.Ui == "verbose")
	{
		std::cout << "MIDI quantisation grain update: source=" << source
			<< " grain=" << grainSamps
			<< " takes=" << takeCount << '\n';
	}
}

void Scene::_ClearTimingState(bool clearTapTempo)
{
	if (_clock)
		_clock->Clear();
	_masterLoopLengthSamps.store(0ul, std::memory_order_release);
	_effectiveQuantiseSamps.store(0u, std::memory_order_release);
	_hasPendingTempo.store(false, std::memory_order_release);
	_SetMidiQuantisationGrain(0u, "timing clear");

	if (clearTapTempo)
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_tapTempo.Clear();
	}
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

void Scene::_UpdateRemoteStationsFromSnapshot(const io::NinjamRemoteSnapshot& snapshot)
{
	std::set<std::string> seenUsers;

	for (const auto& remoteUser : snapshot.Users)
	{
		seenUsers.insert(remoteUser.UserName);

		auto remoteStation = FindRemoteStation(_stations, remoteUser.UserName);

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
	if (!_clock || (timing.SeedSamps == 0u))
		return;

	const auto quantisation = _userConfig.Loop.SeedUsesPowers ? Timer::QUANTISE_POWER : Timer::QUANTISE_MULTIPLE;
	_clock->SetQuantisation(timing.SeedSamps, quantisation);
	_clock->SetSeedSourceLength(timing.MasterLoopSamps);

	_effectiveQuantiseSamps.store(timing.SeedSamps, std::memory_order_release);
	_SetMidiQuantisationGrain(timing.SeedSamps, source);
	_hasPendingTempo.store((timing.Bpm > 0.0f) && (timing.Bpi > 0u), std::memory_order_release);
	_armReclock.store(false, std::memory_order_release);

	std::cout << "Quantisation " << source
		<< ": seed=" << timing.SeedSamps
		<< " master=" << timing.MasterLoopSamps
		<< " seeds=" << timing.SeedCount
		<< " bpm=" << timing.Bpm
		<< " bpi=" << timing.Bpi << std::endl;
}

bool Scene::_HandleTapTempo(Time actionTime)
{
	const auto sampleRate = _CurrentSampleRate();

	std::optional<QuantisationTiming> timing;
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		const auto masterLoopLengthSamps = _masterLoopLengthSamps.load(std::memory_order_acquire);
		if (masterLoopLengthSamps == 0ul)
		{
			std::cout << "Tap tempo: no master loop, tap ignored" << std::endl;
			return true;
		}
		timing = _tapTempo.TapAtSample(_EstimatedAudioSampleAt(actionTime),
			sampleRate,
			masterLoopLengthSamps,
			_QuantisationPolicy());
	}

	if (!timing.has_value())
	{
		std::cout << "Tap tempo: first tap" << std::endl;
		return true;
	}

	_ApplyQuantisationTiming(timing.value(), "tap tempo");
	_UpdateStationQuantisation(nullptr, _selector->CurrentSelectDepth(), false);
	return true;
}

void Scene::_PulseQuantisationOverlay()
{
	_quantisationOverlayLastActive = Timer::GetTime();
}

void Scene::_SetQuantisationOverlayHeld(bool held)
{
	_quantisationOverlayHeld = held;
	_PulseQuantisationOverlay();
}

float Scene::_QuantisationOverlayAlpha(Time now) const
{
	if (_quantisationOverlayHeld)
		return 1.0f;

	if (Timer::IsZero(_quantisationOverlayLastActive))
		return 0.0f;

	const auto elapsed = Timer::GetElapsedSeconds(_quantisationOverlayLastActive, now);
	if (elapsed >= QuantisationOverlayFadeSeconds)
		return 0.0f;

	return static_cast<float>(1.0 - (elapsed / QuantisationOverlayFadeSeconds));
}

void Scene::_ApplyQuantisationOverlayAlpha(float alpha)
{
	for (const auto& station : _stations)
	{
		if (station)
			station->SetQuantisationOverlayAlpha(alpha);
	}
}

bool Scene::_TrySetMasterFromHover(bool confirm)
{
	auto hovering = _ChildFromPath(_selector->CurrentHover());
	if (!hovering)
		return false;

	const auto depth = _selector->CurrentSelectDepth();
	const auto target = _ResolveInteractionTarget(hovering, depth);
	const auto masterLength = target ? target->MasterLength : 0ul;
	if (masterLength == 0ul)
		return false;

	auto timing = DeduceSeedTiming(masterLength, _CurrentSampleRate(), _QuantisationPolicy());
	if (!timing.has_value())
		return false;

	_masterLoop = target->RepresentativeLoop;
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_masterLoopLengthSamps.store(masterLength, std::memory_order_release);
		_tapTempo.Clear();
	}
	_ApplyQuantisationTiming(timing.value(), "master loop");
	if (_clock && _masterLoop)
		_clock->SetMasterLoopIndexFrac(_masterLoop->LoopIndexFrac());
	_UpdateStationQuantisation(hovering, depth, confirm);

	std::cout << "Master quantisation target set: depth=" << static_cast<int>(depth)
		<< " length=" << masterLength << std::endl;
	return true;
}

void Scene::_UpdateStationQuantisation(std::shared_ptr<base::GuiElement> candidate,
	base::SelectDepth depth,
	bool confirmCandidate)
{
	_ClearStationQuantisation();

	if (!_clock || !_clock->IsQuantisable())
		return;

	const auto seed = _clock->QuantiseSamps();
	const auto masterLengthSamps = _masterLoopLengthSamps.load(std::memory_order_acquire);
	const auto master = masterLengthSamps > 0ul ? static_cast<unsigned int>(masterLengthSamps) : seed;
	if (_masterLoop)
	{
		auto masterTarget = _ResolveInteractionTarget(_masterLoop, base::SelectDepth::DEPTH_LOOP);
		if (masterTarget && masterTarget->Station)
			masterTarget->Station->SetQuantisationParams(QuantisationParams{
				seed,
				master
			}, false);
	}

	if (!candidate)
		return;

	const auto candidateTarget = _ResolveInteractionTarget(candidate, depth);
	if (!candidateTarget || candidateTarget->MasterLength == 0ul)
		return;

	if (candidateTarget->Station)
		candidateTarget->Station->SetQuantisationParams(QuantisationParams{
			seed,
			static_cast<unsigned int>(candidateTarget->MasterLength)
		}, confirmCandidate);
}

void Scene::_ClearStationQuantisation()
{
	for (const auto& station : _stations)
		station->ClearQuantisationParams();
}

std::optional<Scene::InteractionTarget> Scene::_ResolveInteractionTarget(
	const std::shared_ptr<base::GuiElement>& target,
	base::SelectDepth depth) const
{
	if (!target)
		return std::nullopt;

	InteractionTarget resolved;
	resolved.Station = std::dynamic_pointer_cast<Station>(target);
	resolved.Take = std::dynamic_pointer_cast<LoopTake>(target);
	resolved.TargetLoop = std::dynamic_pointer_cast<Loop>(target);

	switch (depth)
	{
	case base::SelectDepth::DEPTH_STATION:
		if (!resolved.Station)
			return std::nullopt;
		return resolved;
	case base::SelectDepth::DEPTH_LOOPTAKE:
	{
		if (!resolved.Take)
			return std::nullopt;

		for (const auto& station : _stations)
		{
			const auto& takes = station->GetLoopTakes();
			if (std::find(takes.begin(), takes.end(), resolved.Take) != takes.end())
			{
				resolved.Station = station;
				break;
			}
		}

		for (const auto& loop : resolved.Take->GetLoops())
		{
			if (!loop)
				continue;

			if (!resolved.RepresentativeLoop || (loop->LoopLength() > resolved.RepresentativeLoop->LoopLength()))
				resolved.RepresentativeLoop = loop;
			resolved.MasterLength = std::max(resolved.MasterLength, loop->LoopLength());
		}

		return resolved;
	}
	case base::SelectDepth::DEPTH_LOOP:
	{
		if (!resolved.TargetLoop)
			return std::nullopt;

		for (const auto& station : _stations)
		{
			for (const auto& take : station->GetLoopTakes())
			{
				const auto& loops = take->GetLoops();
				if (std::find(loops.begin(), loops.end(), resolved.TargetLoop) != loops.end())
				{
					resolved.Station = station;
					resolved.Take = take;
					resolved.RepresentativeLoop = resolved.TargetLoop;
					resolved.MasterLength = resolved.TargetLoop->LoopLength();
					return resolved;
				}
			}
		}

		return std::nullopt;
	}
	default:
		return std::nullopt;
	}
}

std::vector<std::shared_ptr<LoopTake>> Scene::_CurrentLoopTakeInteractionTargets()
{
	std::vector<std::shared_ptr<LoopTake>> targets;
	const auto depth = _selector->CurrentSelectDepth();

	const auto appendFromElement = [&](const std::shared_ptr<base::GuiElement>& element)
	{
		const auto resolved = _ResolveInteractionTarget(element, depth);
		if (!resolved)
			return;

		switch (depth)
		{
		case base::SelectDepth::DEPTH_STATION:
			if (!resolved->Station)
				return;
			for (const auto& take : resolved->Station->GetLoopTakes())
				AppendUniqueTarget(targets, take);
			break;
		case base::SelectDepth::DEPTH_LOOPTAKE:
			AppendUniqueTarget(targets, resolved->Take);
			break;
		case base::SelectDepth::DEPTH_LOOP:
			AppendUniqueTarget(targets, resolved->Take);
			break;
		default:
			break;
		}
	};

	appendFromElement(_ChildFromPath(TrimPath(_hoverPath3d, static_cast<unsigned int>(depth) + 1u)));

	switch (depth)
	{
	case base::SelectDepth::DEPTH_STATION:
		for (const auto& station : _stations)
		{
			if (station && station->IsSelected())
				appendFromElement(std::static_pointer_cast<base::GuiElement>(station));
		}
		break;
	case base::SelectDepth::DEPTH_LOOPTAKE:
		for (const auto& station : _stations)
		{
			for (const auto& take : station->GetLoopTakes())
			{
				if (take && take->IsSelected())
					appendFromElement(std::static_pointer_cast<base::GuiElement>(take));
			}
		}
		break;
	case base::SelectDepth::DEPTH_LOOP:
		for (const auto& station : _stations)
		{
			for (const auto& take : station->GetLoopTakes())
			{
				for (const auto& loop : take->GetLoops())
				{
					if (loop && loop->IsSelected())
						appendFromElement(std::static_pointer_cast<base::GuiElement>(loop));
				}
			}
		}
		break;
	default:
		break;
	}

	return targets;
}

void Scene::_ApplyRemoteTempoToClock(const io::NinjamRemoteSnapshot& snapshot)
{
	if (!_clock || !snapshot.HasTiming)
		return;

	// Don't override quantisation while a one-shot reclock is armed -
	// the next recording will establish the new quantisation.
	if (_armReclock.load(std::memory_order_acquire))
		return;

	if (_hasPendingTempo.load(std::memory_order_acquire))
		return;

	auto intervalLengthSamps = snapshot.IntervalLengthSamps;
	if (intervalLengthSamps == 0u)
	{
		intervalLengthSamps = IntervalSampsFromTempo(snapshot.Bpm,
			static_cast<unsigned int>(snapshot.Bpi),
			snapshot.SampleRate);
	}

	const auto tempoChanged = (intervalLengthSamps != _remoteMasterLoopSamps)
		|| (snapshot.SampleRate != _remoteSampleRate);

	if (!tempoChanged && (_effectiveQuantiseSamps.load(std::memory_order_acquire) != 0u) && _clock->IsQuantisable())
		return;

	const auto timing = _userConfig.DeduceLoopTiming(intervalLengthSamps, snapshot.SampleRate);
	if (!timing.has_value() || (timing->GrainSamps == 0u))
		return;

	_remoteMasterLoopSamps = intervalLengthSamps;
	_remoteSampleRate = snapshot.SampleRate;
	_effectiveQuantiseSamps.store(timing->GrainSamps, std::memory_order_release);
	_masterLoopLengthSamps.store(static_cast<unsigned long>(intervalLengthSamps), std::memory_order_release);
	{
		std::scoped_lock tapTempoLock(_tapTempoMutex);
		_tapTempo.Clear();
	}

	const auto quantisation = _userConfig.Loop.SeedUsesPowers ? Timer::QUANTISE_POWER : Timer::QUANTISE_MULTIPLE;
	_clock->SetQuantisation(timing->GrainSamps, quantisation);
	_clock->SetSeedSourceLength(intervalLengthSamps);
	if (intervalLengthSamps > 0u)
	{
		auto loopIndexFrac = 1.0;
		if (snapshot.IntervalPositionSamps > 0u)
		{
			const auto intervalPos = snapshot.IntervalPositionSamps % intervalLengthSamps;
			loopIndexFrac = 1.0 - (static_cast<double>(intervalPos) / static_cast<double>(intervalLengthSamps));
		}
		_clock->SetMasterLoopIndexFrac(loopIndexFrac);
	}

	std::cout << "[NINJAM] Tempo policy applied: bpm=" << snapshot.Bpm
		<< " bpi=" << snapshot.Bpi
		<< " sr=" << snapshot.SampleRate
		<< " intervalSamps=" << intervalLengthSamps
		<< " mode=" << (_userConfig.Loop.SeedUsesPowers ? "power" : "multiple")
		<< " grain=" << timing->GrainSamps << std::endl;
}

void Scene::_QueueLocalTempoFromClock()
{
	if (!_clock || !_clock->IsQuantisable())
		return;

	const auto quantiseSamps = _clock->QuantiseSamps();
	const auto previousQuantiseSamps = _effectiveQuantiseSamps.load(std::memory_order_acquire);
	if ((0u == quantiseSamps) || (quantiseSamps == previousQuantiseSamps))
		return;

	const auto shouldPulseOverlay = (0u == previousQuantiseSamps);

	const auto seedLoopLengthSamps = _clock->SeedSourceLength();
	if (seedLoopLengthSamps == 0u)
	{
		_effectiveQuantiseSamps.store(quantiseSamps, std::memory_order_release);
		_armReclock.store(false, std::memory_order_release);
		if (shouldPulseOverlay)
			_pendingQuantisationOverlayPulse.store(true, std::memory_order_release);
		return;
	}

	auto sampleRate = _remoteSampleRate;
	if (sampleRate == 0u)
		sampleRate = _audioDevice ? _audioDevice->GetAudioStreamParams().SampleRate : 0u;
	if (sampleRate == 0u)
		sampleRate = _userConfig.Audio.SampleRate;
	if (sampleRate == 0u)
	{
		return;
	}

	const auto timing = _userConfig.DeduceLoopTiming(seedLoopLengthSamps, sampleRate);
	if (!timing.has_value())
	{
		return;
	}

	_effectiveQuantiseSamps.store(timing->GrainSamps, std::memory_order_release);
	_masterLoopLengthSamps.store(seedLoopLengthSamps, std::memory_order_release);
	_hasPendingTempo.store(true, std::memory_order_release);
	_armReclock.store(false, std::memory_order_release);
	if (shouldPulseOverlay)
		_pendingQuantisationOverlayPulse.store(true, std::memory_order_release);

	std::cout << "[NINJAM] Local tempo queued: bpm=" << timing->Bpm
		<< " bpi=" << timing->Bpi
		<< " grain=" << timing->GrainSamps
		<< " seedLoopLength=" << seedLoopLengthSamps
		<< " (queued for next interval boundary)" << std::endl;
}

void Scene::_SendQueuedTempoAtIntervalWrap(const io::NinjamRemoteSnapshot& snapshot)
{
	// Detect interval wrap from the latest remote snapshot.
	const auto pos = snapshot.IntervalPositionSamps;
	const bool wrapped = (pos < _lastRemoteIntervalPos);
	_lastRemoteIntervalPos = pos;

	if (!_hasPendingTempo.load(std::memory_order_acquire))
		return;

	if (!_ninjamSession || !_ninjamSession->IsConnected())
		return;

	if (!wrapped)
		return;

	// Derive BPM/BPI on demand from current seed and master lengths.
	auto sampleRate = _remoteSampleRate;
	if (sampleRate == 0u)
		sampleRate = _audioDevice ? _audioDevice->GetAudioStreamParams().SampleRate : 0u;
	if (sampleRate == 0u)
		sampleRate = _userConfig.Audio.SampleRate;

	const auto qtOpt = engine::TimingFromSeedAndMaster(
		_effectiveQuantiseSamps.load(std::memory_order_acquire),
		_masterLoopLengthSamps.load(std::memory_order_acquire),
		sampleRate);
	if (!qtOpt.has_value() || (qtOpt->Bpm <= 0.0f) || (qtOpt->Bpi == 0u))
		return;

	if (_ninjamSession->RequestServerTempo(qtOpt->Bpm, static_cast<int>(qtOpt->Bpi)))
	{
		_hasPendingTempo.store(false, std::memory_order_release);
	}
}
