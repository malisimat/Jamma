#include "Scene.h"
#include <iostream>
#include "glm/ext.hpp"
#include "../utils/PathUtils.h"
#include "../midi/MidiTimestampMapper.h"
#include "../io/IoSessionExporter.h"
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
using namespace timing;
using namespace vst;
using namespace ninjam;
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
	_label(nullptr),
	_selector(nullptr),
	_modeRadio(nullptr),
	_layoutDemoPanel(nullptr),
	_quantisation(),
	_loggingConfig{},
	_stations(),
	_touchDownElement(std::weak_ptr<GuiElement>()),
	_hoverElement3d(std::weak_ptr<GuiElement>()),
	_hoverPath3d(),
	_lastLoggedHoverPath(),
	_ctrlHandleOverlay(),
	_quantisationInteraction(_ctrlHandleOverlay, _quantisation, _stations),
	_cursorPos{},
	_camera(CameraParams(
		MoveableParams(
			Position2d{ 0,0 },
			Position3d{ 0, 0, 420 },
			1.0),
		0)),
	_userConfig(user),
	_viewMode(VIEW_STATION),
		_audioEngine(std::make_unique<audio::AudioHost>(user)),
		_inputSubsystem(std::make_unique<io::IoInputSubsystem>(user, io::LoggingConfig{})),
		_windowSubsystem(std::make_unique<vst::VstEditorWindowManager>()),
		_networkService(std::make_unique<ninjam::NinjamNetworkService>())
{
	_quantisation.SetClock(std::make_shared<Timer>());
	_quantisation.SetSeedUsesPowers(_userConfig.Loop.SeedUsesPowers);

	GuiLabelParams labelParams;
	const std::string versionText = "Jamma v" LIB_VERSION;
	labelParams.String = versionText;
	labelParams.Position = { (int)params.Size.Width - 220, (int)params.Size.Height - 28 };
	labelParams.ModelPosition = { (float)(int)params.Size.Width - 220.0f, (float)(int)params.Size.Height - 28.0f, 0.0f };
	labelParams.Size = { 220, 24 };
	_label = std::make_unique<GuiLabel>(labelParams);

	// ---------------------------------------------------------------------------
	// Layout demo panel — demonstrates Phase 2 retained layout system.
	// Positioned at the top-left corner of the screen.
	// ---------------------------------------------------------------------------
	{
		GuiStackPanelParams rootParams;
		rootParams.Direction = StackDirection::Vertical;
		rootParams.Spacing   = 4u;
		rootParams.PaddingH  = 8u;
		rootParams.PaddingV  = 8u;
		rootParams.Texture   = "rounded_but";
		rootParams.Position  = { 10, 40 };
		rootParams.Size      = { 310u, 200u };
		rootParams.MinSize   = { 100u, 80u };
		_layoutDemoPanel = std::make_shared<GuiStackPanel>(rootParams);
		AddChild(_layoutDemoPanel);

		// Header label (Auto width, fixed height).
		{
			GuiLabelParams lp;
			lp.String      = "-- Layout v2 Demo --";
			lp.Size        = { 280u, 22u };
			lp.MinSize     = { 40u, 22u };
			_layoutDemoPanel->AddChild(std::make_shared<GuiLabel>(lp));
		}

		// Horizontal sub-stack: a toggle, button, and slider grouped together.
		{
			GuiStackPanelParams hParams;
			hParams.Direction   = StackDirection::Horizontal;
			hParams.Spacing     = 6u;
			hParams.Size        = { 290u, 26u };
			hParams.MinSize     = { 80u, 26u };
			auto hStack = std::make_shared<GuiStackPanel>(hParams);

			{
				GuiToggleParams tp;
				tp.Texture = "router";
				tp.OverTexture = "router_over";
				tp.DownTexture = "router_down";
				tp.OutTexture = "router_active";
				tp.ToggledTexture = "router_active";
				tp.ToggledOverTexture = "router_over";
				tp.ToggledDownTexture = "router_down";
				tp.Size = { 72u, 22u };
				tp.MinSize = { 36u, 22u };
				hStack->AddChild(std::make_shared<GuiToggle>(tp));
			}

			{
				GuiButtonParams bp;
				bp.Texture = "router";
				bp.OverTexture = "router_over";
				bp.DownTexture = "router_down";
				bp.OutTexture = "router_active";
				bp.Size = { 72u, 22u };
				bp.MinSize = { 36u, 22u };
				hStack->AddChild(std::make_shared<GuiButton>(bp));
			}

			{
				GuiSliderParams sp;
				sp.Texture = "rounded_but";
				sp.Size = { 120u, 22u };
				sp.MinSize = { 60u, 22u };
				sp.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
				sp.DragTexture = "blue";
				sp.DragControlSize = { 12u, 22u };
				sp.DragControlOffset = { 0, 0 };
				sp.DragGap = { 0u, 0u };
				hStack->AddChild(std::make_shared<GuiSlider>(sp));
			}

			_layoutDemoPanel->AddChild(hStack);
		}

		// 2×2 grid: four control cells showing toggle, button, slider, and selector.
		{
			GuiGridParams gp;
			gp.Texture     = "";
			gp.Size     = { 290u, 74u };
			gp.MinSize  = { 80u, 40u };
			gp.PaddingH = 2u;
			gp.PaddingV = 2u;

			GridCellDef col;
			col.sizing  = GridCellDef::Sizing::Fill;
			col.spacing = 4u;
			gp.Cols = { col, col };

			GridCellDef row;
			row.sizing    = GridCellDef::Sizing::Fixed;
			row.fixedSize = 32u;
			row.spacing   = 4u;
			gp.Rows = { row, row };

			auto grid = std::make_shared<GuiGrid>(gp);

			const std::array<std::function<std::shared_ptr<GuiElement>()>, 4> cellCreators = {
				[]() {
					GuiToggleParams tp;
					tp.Texture = "rounded_but";
					tp.OverTexture = "rounded_but_over";
					tp.DownTexture = "rounded_but_down";
					tp.OutTexture = "";
					tp.ToggledTexture = "rounded_but_on";
					tp.ToggledOverTexture = "rounded_but_on_over";
					tp.ToggledDownTexture = "rounded_but_on_down";
					tp.Size = { 80u, 22u };
					tp.MinSize = { 20u, 22u };
					return std::make_shared<GuiToggle>(tp);
				},
				[]() {
					GuiButtonParams bp;
					bp.Texture = "rounded_but";
					bp.OverTexture = "rounded_but_over";
					bp.DownTexture = "rounded_but_down";
					bp.OutTexture = "rounded_but_on";
					bp.Size = { 80u, 22u };
					bp.MinSize = { 20u, 22u };
					return std::make_shared<GuiButton>(bp);
				},
				[]() {
					GuiSliderParams sp;
					sp.Texture = "rounded_but";
					sp.OverTexture = "rounded_but_over";
					sp.DownTexture = "rounded_but_down";
					sp.OutTexture = "rounded_but_on";
					sp.Size = { 80u, 22u };
					sp.MinSize = { 20u, 22u };
					sp.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
					sp.DragTexture = "green";
					sp.DragOverTexture = "";
					sp.DragControlSize = { 10u, 22u };
					return std::make_shared<GuiSlider>(sp);
				},
				[]() {
					GuiSelectorParams sp;
					sp.Texture = "router";
					sp.OverTexture = "router_over";
					sp.DownTexture = "router_down";
					sp.OutTexture = "router_active";
					sp.Size = { 80u, 22u };
					sp.MinSize = { 20u, 22u };
					return std::make_shared<GuiSelector>(sp);
				}
			};

			for (int ci = 0; ci < 4; ++ci)
			{
				GridChildPlacement placement;
				placement.row    = static_cast<unsigned int>(ci / 2);
				placement.col    = static_cast<unsigned int>(ci % 2);
				placement.hAlign = LayoutHAlign::Fill;
				placement.vAlign = LayoutVAlign::Fill;
				grid->AddGridChild(cellCreators[ci](), placement);
			}
			_layoutDemoPanel->AddChild(grid);
		}

		// Wrapping horizontal stack — demonstrates responsive narrow-width behaviour with another control mix.
		{
			GuiStackPanelParams wParams;
			wParams.Direction    = StackDirection::Horizontal;
			wParams.Spacing      = 4u;
			wParams.WrapContent  = true;
			wParams.Size         = { 290u, 42u };
			wParams.MinSize      = { 60u, 24u };
			auto wStack = std::make_shared<GuiStackPanel>(wParams);

			{
				GuiToggleParams tp;
				tp.Texture = "rounded_but";
				tp.OverTexture = "rounded_but_over";
				tp.DownTexture = "rounded_but_down";
				tp.OutTexture = "rounded_but_on";
				tp.ToggledTexture = "rounded_but_on";
				tp.ToggledOverTexture = "rounded_but_on_over";
				tp.ToggledDownTexture = "rounded_but_on_down";
				tp.Size = { 80u, 22u };
				tp.MinSize = { 30u, 22u };
				wStack->AddChild(std::make_shared<GuiToggle>(tp));
			}

			{
				GuiButtonParams bp;
				bp.Texture = "rounded_but";
				bp.OverTexture = "rounded_but_over";
				bp.DownTexture = "rounded_but_down";
				bp.OutTexture = "rounded_but_on";
				bp.Size = { 80u, 22u };
				bp.MinSize = { 30u, 22u };
				wStack->AddChild(std::make_shared<GuiButton>(bp));
			}

			{
				GuiSliderParams sp;
				sp.Texture = "rounded_but";
				sp.OverTexture = "rounded_but_over";
				sp.DownTexture = "rounded_but_down";
				sp.OutTexture = "rounded_but_on";
				sp.Size = { 92u, 22u };
				sp.MinSize = { 30u, 22u };
				sp.Orientation = GuiSliderParams::SLIDER_HORIZONTAL;
				sp.DragTexture = "red";
				sp.DragOverTexture = "";
				sp.DragControlSize = { 10u, 22u };
				wStack->AddChild(std::make_shared<GuiSlider>(sp));
			}

			_layoutDemoPanel->AddChild(wStack);
		}
	}

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
	scene->_quantisation.SetGlobalPhaseOffsetSamps(jamStruct.GlobalPhaseOffsetSamps, scene->_stations);
	scene->_networkService->GetController()->LoadConfig(jamStruct.Ninjam);
	scene->InitReceivers();

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

	for (auto& child : _guiChildren)
		if (child)
			child->Draw(ctx);

	for (auto& station : _stations)
		station->Draw(ctx);

	_selector->Draw(ctx);
	_modeRadio->Draw(ctx);
	_ctrlHandleOverlay.Draw(ctx);

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
		const auto now = Timer::GetTime();
		_ApplyQuantisationOverlayAlpha(_QuantisationOverlayAlpha(now));
		_quantisationInteraction.Tick(now);
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
	for (auto& child : _guiChildren)
		if (child)
			child->InitResources(resourceLib, forceInit);
	_ctrlHandleOverlay.InitResources(resourceLib, forceInit);

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
	for (auto& child : _guiChildren)
		if (child)
			child->ReleaseResources();
	_ctrlHandleOverlay.ReleaseResources();

	for (auto& station : _stations)
		station->ReleaseResources();

	Drawable::_ReleaseResources();
}

ActionResult Scene::OnAction(TouchAction action)
{
	ActionResult res;
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	_cursorPos = action.Position;

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

	if (auto overlayRes = _quantisationInteraction.TryHandleTouchAction(action,
		_CurrentSampleRate(),
		_IsMidiPhaseDragModifier(action.Modifiers),
		_InteractionContext(),
		[this](const std::vector<unsigned char>& path) { return _ChildFromPath(path); });
		overlayRes.has_value())
	{
		return overlayRes.value();
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
			_EndBackgroundDrag();

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

	for (auto it = _guiChildren.rbegin(); it != _guiChildren.rend(); ++it)
	{
		if (!*it)
			continue;

		res = static_cast<std::shared_ptr<base::GuiElement>>(*it)->OnAction((*it)->ParentToLocal(action));
		if (res.IsEaten)
		{
			if (nullptr != res.Undo)
				_undoHistory.Add(res.Undo);

			if (!_touchDownElement.lock())
				_touchDownElement = res.ActiveElement;

			return res;
		}
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

	return _BeginBackgroundDrag(action);
}

ActionResult Scene::OnAction(TouchMoveAction action)
{
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	_cursorPos = action.Position;

	if (auto overlayRes = _quantisationInteraction.TryHandleTouchMove(action,
		_CurrentSampleRate());
		overlayRes.has_value())
	{
		return overlayRes.value();
	}

	auto activeElement = _touchDownElement.lock();

	if (activeElement)
		return activeElement->OnAction(activeElement->GlobalToLocal(action));
	else if (_isSceneTouching)
		return _UpdateBackgroundDrag(action);
	else
	{
		for (auto it = _guiChildren.rbegin(); it != _guiChildren.rend(); ++it)
		{
			if (!*it)
				continue;

			auto res = static_cast<std::shared_ptr<base::GuiElement>>(*it)->OnAction((*it)->ParentToLocal(action));
			if (res.IsEaten)
				return res;
		}

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
	action.SetAudioParams(_audioEngine->GetStreamParams());

	std::cout << "Key action " << action.KeyActionType << " [" << action.KeyChar << "] IsSytem:" << action.IsSystem << ", Modifiers:" << action.Modifiers << "]" << std::endl;

	if (17 == action.KeyChar)
	{
		const bool held = (actions::KeyAction::KEY_DOWN == action.KeyActionType);
		_quantisationInteraction.OnCtrlModifierChanged(held,
			Timer::GetTime(),
			_InteractionContext(),
			[this](const std::vector<unsigned char>& path) { return _ChildFromPath(path); });
	}

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
		return _windowSubsystem->HandleVstInsert(pluginPath,
			_selector->CurrentSelectDepth(),
			hovering,
			[this]() { CommitChanges(); });
	}

	// Ctrl+Shift+E - open the first plugin editor for the hovered station/take/loop.
	if ((69 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		auto hovering = _ChildFromPath(_selector->CurrentHover());
		return _windowSubsystem->HandleVstEditorOpen(hovering,
			_selector->CurrentSelectDepth(),
			_stations);
	}

	// Ctrl+S - export session to directory.
	if ((83 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers))
	{
		return io::IoSessionExporter::ExportSession(_stations,
			_quantisation,
			_userConfig,
			_audioEngine->GetStreamParams(),
			_audioEngine->GetDevice(),
			_sceneMutex,
			_networkService->GetController());
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
	const auto stationsSnapshot = _audioEngine->GetStationsSnapshot();
	static const std::vector<std::shared_ptr<Station>> emptyStations;
	const auto& stations = stationsSnapshot ? *stationsSnapshot : emptyStations;

	for (auto& station : stations)
	{
		station->OnTick(curTime,
			samps,
			_userConfig,
			_audioEngine->GetStreamParams());

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

	auto snapshot = _networkService->GetController()->Pump();
	{
		// Always sync the station clock state to the scene-level quantisation.
		// This ensures that when the first loop seeds the station clock locally
		// (without a NINJAM session), _effectiveQuantiseSamps is updated promptly.
		std::scoped_lock lock(_sceneMutex);
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
	job.SetAudioParams(_audioEngine->GetStreamParams());

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
    auto summary = _inputSubsystem->PumpMidi(_stations, _audioEngine->GetAudioSampleCounter(), _audioEngine->GetStreamParams(), _sceneMutex);
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
	_inputSubsystem->RegisterMidiTriggerRoute(deviceName, std::move(trigger));
}

void Scene::_PumpSerial()
{
    auto summary = _inputSubsystem->PumpSerial(_stations, _audioEngine->GetStreamParams(), _sceneMutex);
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

void Scene::AddChild(std::shared_ptr<base::GuiElement> child)
{
	if (!child)
		return;

	auto it = std::find(_guiChildren.begin(), _guiChildren.end(), child);
	if (it == _guiChildren.end())
	{
		_guiChildren.push_back(child);
		child->Init();
	}
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

	if ((elementPath != _lastLoggedHoverPath) && (_loggingConfig.Ui == "verbose"))
	{
		std::string pathString = "[";
		for (size_t i = 0; i < elementPath.size(); ++i)
		{
			pathString += std::to_string(static_cast<unsigned int>(elementPath[i]));
			if ((i + 1) < elementPath.size())
				pathString += ",";
		}
		pathString += "]";
		std::cout << "Hover3d resolved: " << pathString << std::endl;
		_lastLoggedHoverPath = elementPath;
	}
	else if (elementPath != _lastLoggedHoverPath)
	{
		_lastLoggedHoverPath = elementPath;
	}

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
	_quantisationInteraction.RefreshOverlay(_InteractionContext(),
		[this](const std::vector<unsigned char>& path) { return _ChildFromPath(path); });

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
	// Setup audio engine which starts device
	bool started = _audioEngine->Init(_networkService->GetController(), [this](Time streamTime, unsigned int numSamps, const io::UserConfig& cfg, const audio::AudioStreamParams& params) {
		this->OnTick(Timer::GetTime(), numSamps, cfg, params);
	});

	if (started) {
		InitMidi();
		InitSerial();
	}

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

void Scene::CloseAudio()
{
	CloseSerial();
	CloseMidi();
	_audioEngine->Close();
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
	std::optional<ninjam::NinjamRemoteSnapshot> pendingRemoteSnapshot = _networkService->GetController()->TakePendingSnapshot();

	{
		std::scoped_lock lock(_sceneMutex);

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
	// thread. Do this after releasing _sceneMutex so LoadLibraryW stays out of
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

	_quantisationInteraction.RefreshOverlay(_InteractionContext(),
		[this](const std::vector<unsigned char>& path) { return _ChildFromPath(path); });
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

int Scene::CtrlOverlayVisibleButtonCountForTest() const noexcept
{
	return _quantisationInteraction.VisibleButtonCountForTest();
}

std::optional<utils::Position2d> Scene::CtrlOverlayButtonCenterForTest(int buttonIndex) const noexcept
{
	return _quantisationInteraction.ButtonCenterForTest(buttonIndex);
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
	station->SetNumAdcChannels(_audioEngine->GetChannelMixer()->Source()->NumOutputChannels(Audible::AUDIOSOURCE_ADC));
	station->SetNumDacChannels(_audioEngine->GetChannelMixer()->Sink()->NumInputChannels(Audible::AUDIOSOURCE_LOOPS));
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

bool Scene::_IsMidiPhaseDragModifier(base::Action::Modifiers modifiers) const noexcept
{
	return (Action::MODIFIER_CTRL & modifiers);
}

QuantisationInteractionContext Scene::_InteractionContext() const
{
	QuantisationInteractionContext context;
	context.CursorPos = _cursorPos;
	context.ViewportSize = _sizeParams.Size;
	context.SelectDepth = _selector->CurrentSelectDepth();
	context.HoverPath = _selector->CurrentHover();
	context.HoverPath3d = _hoverPath3d;
	return context;
}

ActionResult Scene::_BeginBackgroundDrag(TouchAction action)
{
	_isSceneTouching = true;
	_isSceneDragged = false;
	_initTouchDownPosition = action.Position;
	_initTouchCamPosition = _camera.ModelPosition();

	ActionResult res;
	res.IsEaten = true;
	res.SourceId = "";
	res.TargetId = "";
	res.ResultType = ACTIONRESULT_DEFAULT;
	res.Undo = std::shared_ptr<ActionUndo>();
	res.ActiveElement = std::weak_ptr<GuiElement>();
	return res;
}

ActionResult Scene::_UpdateBackgroundDrag(TouchMoveAction action)
{
	auto dPos = action.Position - _initTouchDownPosition;
	_camera.SetModelPosition(_initTouchCamPosition - Position3d{ (float)dPos.X, (float)dPos.Y, 0.0 });
	SetSize(_sizeParams.Size);

	_isSceneDragged = true;
	return ActionResult::NoAction();
}

void Scene::_EndBackgroundDrag()
{
	_isSceneTouching = false;
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
	_audioEngine->SetStations(stations);
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
	if (_audioEngine->GetDevice())
	{
		const auto streamRate = _audioEngine->GetStreamParams().SampleRate;
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
	const auto anchorSample = _audioEngine->GetAudioSampleCounter();
	const auto anchorMicros = _audioEngine->GetMidiAnchorMicros();
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

bool Scene::_HasQuantisationSelection() const
{
	for (const auto& station : _stations)
	{
		if (!station)
			continue;

		if (station->IsSelected())
			return true;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;

			if (take->IsSelected())
				return true;

			for (const auto& loop : take->GetLoops())
			{
				if (loop && loop->IsSelected())
					return true;
			}
		}
	}

	return false;
}

bool Scene::_HasQuantisationHover() const
{
	return !_selector->CurrentHover().empty() || !_hoverPath3d.empty();
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
