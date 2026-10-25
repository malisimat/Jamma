#include "Workspace.h"
#include "../utils/Timer.h"
#include <iostream>
#include "../graphics/GlDrawContext.h"
#include "../gui/GuiRack.h"
#include "glm/ext.hpp"

using namespace gui;
using namespace engine;
using namespace base;
using namespace actions;
using namespace graphics;
using namespace midi;
using namespace resources;
using namespace utils;

Workspace::Workspace(utils::Size2d size, std::uint64_t sceneIdentity) : _sceneIdentity(sceneIdentity), _size(size),
	_camera(CameraParams(MoveableParams({0, 0}, {0, 0, 420}, 1.0), 0))
{}

void Workspace::Initialize(GuiHudParams hud, std::uint8_t channel, double offset,
	io::JamFile::GlobalMidiQuantState quantisation,
	std::function<void(const std::shared_ptr<base::GuiElement>&)> beforeHide,
	std::function<void(bool, engine::GroupCommand, std::size_t)> groupCommand)
{
	hud.Size = hud.MinSize = _size;
	hud.PopupManager = &_popupManager;
	hud.FocusEditor = [this](const auto& editor, bool acquire) {
		if (acquire) _focusManager.RequestFocus(editor);
		else if (_focusManager.CurrentFocus() == editor) _focusManager.ClearFocus();
	};
	_hudPanel = std::make_shared<GuiHud>(hud);
	AddChild(_hudPanel);
	GuiSelectorParams selector;
	selector.Position = {10, 2};
	selector.Size = _size;
	_selector = std::make_unique<SceneSelector>(selector);
	_selector->SetSelectDepth(base::DEPTH_STATION);
	_settingsControls = std::make_unique<SceneSettingsControls>(channel, offset, quantisation);
	GuiMainPanelParams panel;
	panel.Size = _size;
	panel.PopupManager = &_popupManager;
	panel.BeforeHide = std::move(beforeHide);
	panel.Settings = _settingsControls->SettingsEntries();
	_mainPanel = std::make_shared<GuiMainPanel>(panel);
	AddChild(_mainPanel);
	panel.SelectionOnly = true;
	panel.Settings = _settingsControls->SelectionEntries();
	_selectionPanel = std::make_shared<GuiMainPanel>(panel);
	AddChild(_selectionPanel);
	_selectionPanel->SetPerformanceGroupHandler(std::move(groupCommand));
}

void Workspace::DrawOverlayLocked(DrawContext& ctx, const std::vector<std::shared_ptr<Station>>& stations,
	LoopGridEditor& editor, QuantiserController& quantisation, const OverlayPresentation& presentation)
{
	const bool loopEditorEngaged = editor.IsEngaged();
	_mainPanel->SetRigSaveEnabled(presentation.RigSaveEnabled);
	UpdateRackVisibilityLocked(stations, editor.IsEngaged());
	if (_hudPanel)
	{
		_hudPanel->SetLoopEditorMode(loopEditorEngaged);
		_hudPanel->SetQuantisationFeedback(presentation.QuantisationFeedback);
		_hudPanel->SetEditModeAlpha(quantisation.PanelAlpha());
	}

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	_compositor.BeginOverlay(glCtx);

	_settingsControls->RefreshChannelPresentation(presentation.ForcedChannel);

	if (_hudPanel)
	{
		const auto samples = std::max(1u, presentation.Stream.BufSize);
		for (auto channel = 0u; channel < presentation.AudioPeaks.size(); ++channel)
			_hudPanel->SetAudioInputPeak(channel, presentation.AudioPeaks[channel], samples);
		for (auto channel = 0u; channel < presentation.MidiPeaks.size(); ++channel)
			_hudPanel->SetMidiInputPeak(channel, presentation.MidiPeaks[channel], samples);
	}

	if (!loopEditorEngaged)
	{
		auto opacity = ctx.WithOpacity(1.0f - quantisation.PanelAlpha());
		for (auto& station : stations)
			station->Draw(ctx);
	}

	if (!editor.IsEngaged())
	{
		for (const auto& station : stations)
			if (auto label = station->GetGuiRack()->StationLabel()) label->DrawOverlay(ctx);
		_stationControls.Draw(ctx);
	}
	_selector->Draw(ctx);
	if (!loopEditorEngaged)
		_ctrlHandleOverlay.Draw(ctx);


	{
		auto opacity = ctx.WithOpacity(1.0f - quantisation.PanelAlpha());
		for (auto& child : _guiChildren)
			if (child && child != _mainPanel && child != _selectionPanel && child != _hudPanel)
				child->Draw(ctx);
	}
	if (_hudPanel) _hudPanel->Draw(ctx);
	_compositor.DrawStatus(ctx, _size, presentation.Tempo);
	if (!_popupManager.IsOpen()) editor.Draw(ctx);
	{
		auto opacity = ctx.WithOpacity(1.0f - quantisation.PanelAlpha());
		_mainPanel->Draw(ctx);
		_selectionPanel->Draw(ctx);
	}
	_popupManager.Draw(ctx);

	_compositor.EndOverlay(glCtx);
}

void Workspace::DrawWorldLocked(DrawContext& ctx, DrawPass pass, const std::vector<std::shared_ptr<Station>>& stations,
	LoopGridEditor& editor, QuantiserController& quantisation, const PerformanceGroups& groups, WorldPresentation presentation)
{

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	_viewProj = _compositor.BeginWorld(glCtx, _camera, _size, StationCentre(stations), pass);
	UpdateHudStationAnchorsLocked(stations);
	if (PASS_PICKER == pass && editor.IsEngaged())
	{
		_compositor.EndProjection(glCtx);
		return;
	}

	if (PASS_SCENE == pass)
	{
		const auto now = Timer::GetTime();
		quantisation.Tick(now);
	}

	const bool probeBound = _compositor.SetWorldPresentation(glCtx,
		{ PASS_SCENE == pass && presentation.HasSelection, editor.SurroundingDim(),
			PASS_SCENE == pass && editor.IsEngaged(),
			PASS_SCENE == pass && editor.IsEngaged() ? editor.ProbeEyeLocal() : glm::vec3{} });
	editor.ApplyToModels();
	if (_selectionPanel && (!_selectionPanel->IsVisible() || !_selectionPanel->IsEnabled()
		|| _popupManager.IsOpen() || editor.IsEngaged() || presentation.EditControlsSuppressed))
		_selectionPanel->ClearPerformanceGroupPointerState();
	const auto hoveredGroup = PASS_SCENE == pass && _selectionPanel && !_popupManager.IsOpen()
		&& !editor.IsEngaged() && !presentation.EditControlsSuppressed
		? _selectionPanel->HoveredPerformanceGroup() : std::nullopt;
	const auto mutePreview = hoveredGroup && !hoveredGroup->first ? groups.Mute.Get(hoveredGroup->second) : nullptr;
	const auto selectionPreview = hoveredGroup && hoveredGroup->first ? groups.Selection.Get(hoveredGroup->second) : nullptr;
	const bool backgroundPreview = PASS_SCENE == pass && presentation.BackgroundPreview;
	std::vector<GroupTargetId> livePreviewTargets;
	if (mutePreview || selectionPreview)
		for (const auto& station : stations)
		{
			if (station->IsRemote()) continue;
			livePreviewTargets.push_back({ GroupTargetKind::Station, station->Id(), {}, {} });
			for (const auto& take : station->GetLoopTakes())
			{
				livePreviewTargets.push_back({ GroupTargetKind::Take, station->Id(), take->Id(), {} });
				for (const auto& loop : take->GetLoops())
					livePreviewTargets.push_back({ GroupTargetKind::AudioLoop, station->Id(), take->Id(), loop->Id() });
				for (const auto& loop : take->GetMidiLoops())
					livePreviewTargets.push_back({ GroupTargetKind::MidiLoop, station->Id(), take->Id(), loop->Id() });
			}
		}
	const auto member = [&](const GroupTargetId& target) {
		return mutePreview ? PerformanceGroupPreview::ContainsLive(*mutePreview, target, livePreviewTargets)
			: selectionPreview && PerformanceGroupPreview::ContainsLive(*selectionPreview, target, livePreviewTargets);
	};
	std::map<std::vector<unsigned int>, bool> previewMembers;
	if (backgroundPreview || mutePreview || selectionPreview)
		for (const auto& station : stations)
		{
			if (station->IsRemote()) continue;
			previewMembers[station->GlobalId()] = backgroundPreview || member({ GroupTargetKind::Station, station->Id(), {}, {} });
			for (const auto& take : station->GetLoopTakes())
			{
				for (const auto& loop : take->GetLoops())
					if (const auto model = loop->Model())
						previewMembers[model->GlobalId()] = backgroundPreview || member({ GroupTargetKind::AudioLoop, station->Id(), take->Id(), loop->Id() });
				for (const auto& loop : take->GetMidiLoops())
					if (const auto model = loop->Model())
						// MIDI playback mute belongs to the take; mute snapshots
						// intentionally store no separate MIDI-loop mute flag.
						previewMembers[model->GlobalId()] = backgroundPreview || (mutePreview
							? PerformanceGroupPreview::AudibleTake(*mutePreview, { GroupTargetKind::Take, station->Id(), take->Id(), {} })
							: member({ GroupTargetKind::MidiLoop, station->Id(), take->Id(), loop->Id() }));
			}
		}
	const bool selectionTint = backgroundPreview ? presentation.BackgroundSelectionPreview : selectionPreview != nullptr;
	glCtx.SetGroupPreview(backgroundPreview || mutePreview || selectionPreview,
		selectionTint ? glm::vec3(1.0f, 0.875f, 0.208f) : glm::vec3(0.259f, 0.588f, 1.0f), std::move(previewMembers), !backgroundPreview);
	for (auto& station : stations)
	{
		station->Draw3d(ctx, 1, pass);
	}
	_compositor.EndWorld(glCtx, probeBound);
}

void Workspace::ResizeLocked(utils::Size2d size, LoopGridEditor& editor, const std::vector<std::shared_ptr<Station>>& stations)
{
	_size = size;
	editor.SetSize(_size);
	_tempoPrompt.FitToViewport(_size);
	_runtimeDialogs.FitToViewport(_size);
	if (_mainPanel) _mainPanel->SetViewportSize(_size);
	if (_selectionPanel) _selectionPanel->SetViewportSize(_size);
	auto ar = _size.Height > 0 ?
		(float)_size.Width / (float)_size.Height :
		1.0f;
	auto projection = _camera.Projection(ar, StationCentre(stations));
	_viewProj = projection * _camera.ViewMatrix();
	_compositor.Resize(_size);

	if (_hudPanel)
	{
		if (_selectionPanel) _hudPanel->SetTopPanelLayout(_selectionPanel->TopPanelLayout());
		_hudPanel->SetSize(_size);
		if (_mainPanel) _mainPanel->SetOccupiedTopBounds(
			_selectionPanel ? _selectionPanel->TopPanelLayout().PresentedBounds : Rect2d{}, _hudPanel->OccupiedTopStripBounds());
	}

	UpdateHudStationAnchorsLocked(stations);
	_pointer.InvalidateHover();
}

void Workspace::UpdateHudStationAnchorsLocked(const std::vector<std::shared_ptr<Station>>& stations)
{
	if (!_hudPanel || (_size.Width == 0) || (_size.Height == 0))
		return;

	std::vector<gui::GuiHud::StationAnchor> anchors;
	anchors.reserve(stations.size());

	for (size_t stationIndex = 0u; stationIndex < stations.size(); ++stationIndex)
	{
		const auto& station = stations[stationIndex];
		const auto modelPos = station->TopCapModelPosition();
		auto clip = _viewProj * glm::vec4(modelPos.X, modelPos.Y, 0.0f, 1.0f);
		anchors.push_back({ stationIndex, station->Name(),
			gui::CableInteraction::ProjectAnchor(clip, _size), glm::vec4(0.85f, 0.90f, 0.95f, 0.45f) });
	}

	_hudPanel->SetStationAnchors(std::move(anchors));
}

void Workspace::UpdateViewProjectionLocked(const std::vector<std::shared_ptr<Station>>& stations)
{
	const float aspect = _size.Height > 0
		? static_cast<float>(_size.Width) / static_cast<float>(_size.Height) : 1.0f;
	_viewProj = _camera.Projection(aspect, StationCentre(stations)) * _camera.ViewMatrix();
}

void Workspace::UpdateRackVisibilityLocked(const std::vector<std::shared_ptr<Station>>& stations, bool editorEngaged)
{
	const auto depth = _selector->CurrentSelectDepth();
	if (depth == base::DEPTH_LOOPTAKE || depth == base::DEPTH_LOOP)
		for (const auto& station : stations)
			if (station->LayoutTakeRacks(_viewProj, _size)) _pointer.InvalidateHover();

	// Presentation stays on the UI thread under the existing scene lock.
	const auto forEachRack = [this, &stations, editorEngaged](auto&& visit) {
		for (const auto& station : stations)
		{
			const auto position = station->PresentationModelPosition();
			const auto clip = _viewProj * glm::vec4(position.X, position.Y, position.Z, 1.0f);
			const bool stationInView = station->IsVisible() && !editorEngaged &&
				clip.w > 0.0f && clip.z >= -clip.w && clip.z <= clip.w;
			visit(station->GetGuiRack(), stationInView);
			for (const auto& take : station->GetLoopTakes())
			{
				const auto rack = take->GetGuiRack();
				visit(rack, stationInView && take->IsVisible());
			}
		}
	};

	std::shared_ptr<GuiRack> expanded;
	forEachRack([&](const std::shared_ptr<GuiRack>& rack, bool ownerInView) {
		if (!rack || rack->GetRackState() == GuiRackParams::RACK_MASTER) return;
		if (!ownerInView || !rack->IsInView(_size) || expanded)
		{
			rack->SetRackState(GuiRackParams::RACK_MASTER, true);
			_pointer.InvalidateHover();
		}
		else
			expanded = rack;
	});
	forEachRack([&](const std::shared_ptr<GuiRack>& rack, bool ownerInView) {
		if (!rack) return;
		const bool visible = ownerInView && (!expanded || rack == expanded);
		if (rack->GetMasterSlider()->Parent()->IsVisible() != visible)
		{
			rack->SetMasterControlsVisible(visible);
			_pointer.InvalidateHover();
		}
	});
}

void Workspace::CollapseRacksLocked(const std::vector<std::shared_ptr<Station>>& stations)
{
	const auto collapse = [](const std::shared_ptr<GuiRack>& rack) {
		if (!rack) return;
		rack->SetRackState(GuiRackParams::RACK_MASTER, true);
		rack->SetMasterControlsVisible(true);
	};
	for (const auto& station : stations)
	{
		collapse(station->GetGuiRack());
		for (const auto& take : station->GetLoopTakes()) collapse(take->GetGuiRack());
	}
	_pointer.InvalidateHover();
}

void Workspace::DrawBackground(DrawContext& ctx)
{
	_compositor.DrawBackground(dynamic_cast<GlDrawContext&>(ctx), _camera, _size);
}

void Workspace::InitResourcesLocked(ResourceLib& resourceLib, bool forceInit, LoopGridEditor& editor,
	const std::vector<std::shared_ptr<Station>>& stations)
{

	_compositor.InitResources(resourceLib, forceInit);
	editor.InitResources(resourceLib, forceInit);
	_selector->InitResources(resourceLib, forceInit);
	for (auto& child : _guiChildren)
		if (child)
			child->InitResources(resourceLib, forceInit);
	_tempoPrompt.InitResources(resourceLib, forceInit);
	_ctrlHandleOverlay.InitResources(resourceLib, forceInit);

	for (auto& station : stations)
		station->InitResources(resourceLib, forceInit);

	ResizeLocked(_size, editor, stations);

}

void Workspace::ReleaseResources(LoopGridEditor& editor)
{
	_runtimeRendererReady = false;
	_runtimeDialogs.ReleaseResources();
	_compositor.ReleaseResources();
	editor.ReleaseResources();
	_selector->ReleaseResources();
	for (auto& child : _guiChildren)
		if (child)
			child->ReleaseResources();
	_tempoPrompt.ReleaseResources();
	_ctrlHandleOverlay.ReleaseResources();
	_stationControls.ReleaseResources();
	if (_stationDeleteDialog) _stationDeleteDialog->ReleaseResources();
	if (_takeDitchDialog) _takeDitchDialog->ReleaseResources();
}

void Workspace::AddChild(std::shared_ptr<base::GuiElement> child)
{
	if (!child)
		return;

	auto it = std::find(_guiChildren.begin(), _guiChildren.end(), child);
	if (it == _guiChildren.end())
	{
		_guiChildren.push_back(child);
		child->Init();
		_pointer.InvalidateHover();
	}
}

Position3d Workspace::StationCentre(const std::vector<std::shared_ptr<Station>>& stations)
{
	Position3d stationCentre{};
	if (!stations.empty())
	{
		for (const auto& station : stations)
			stationCentre += station->ModelPosition();
		const auto inverseCount = 1.0f / static_cast<float>(stations.size());
		stationCentre.X *= inverseCount;
		stationCentre.Y *= inverseCount;
		stationCentre.Z *= inverseCount;
	}
	return stationCentre;
}

Camera::Presentation Workspace::ObserveCameraLocked(const std::vector<std::shared_ptr<Station>>& stations, bool diagnosticDue)
{
	Camera::Presentation presentation;
	size_t visibleContentTakeCount = 0u;
	size_t allContentTakeCount = 0u;
	for (size_t index = 0u; index < stations.size(); ++index)
	{
		const auto& station = stations[index];
		if (station->MembershipRemoving()) continue;
		_camera.ObserveStation(index, station, station->LoopTakeRevision(), station->PresentationModelPosition());
		const auto takes = station->GetLoopTakeSnapshot();
		const bool visible = station->IsVisible();
		graphics::CameraBounds stationBounds = visible ? station->CameraDeckBoundsInWorld() : graphics::CameraBounds{};
		const auto stationPosition = station->PresentationModelPosition();
		const auto stationScale = static_cast<float>(station->ModelScale());
		for (const auto& take : takes)
		{
			if (take->IsCaptureCancelled()) continue;
			presentation.LiveTakes.push_back(take);
			const bool hasCameraContent = diagnosticDue && take->HasCameraContent();
			if (hasCameraContent) ++allContentTakeCount;
			if (!visible || !take->IsVisible()) continue;
			auto bounds = take->CameraBoundsInParent(station->CameraTakeVisualScale())
				.Transformed(stationPosition, stationScale);
			const auto centre = stationPosition + take->ModelPosition() * Position3d{ stationScale, stationScale, stationScale };
			// Empty takes still have a meaningful transformed origin for overview framing.
			bounds.Include(centre);
			stationBounds.Include(bounds);
			if (hasCameraContent) ++visibleContentTakeCount;
			presentation.Takes.push_back({ take, bounds, centre });
		}
		presentation.VisibleBounds.Include(stationBounds);
		if (visible)
		{
			presentation.OverviewBounds.Include(stationBounds);
			presentation.Stations.push_back({ station, stationBounds, stationPosition });
		}
	}
	_camera.CompleteStationObservation(stations.size(), stations.empty()
		? std::optional<Position3d>{}
		: std::optional<Position3d>{ stations.front()->ModelPosition() });
	return presentation;
}


void Workspace::InitDynamicResourcesLocked(ResourceLib& resourceLib, const std::vector<std::shared_ptr<Station>>& stations,
	bool ctrlHeld, bool controlsAvailable)
{
	// Motion and camera updates precede this one control layout/init pass.
	UpdateViewProjectionLocked(stations);
	const auto depth = _selector->CurrentSelectDepth();
	const bool stationDitchVisible = depth == base::DEPTH_STATION;
	const bool takeDitchVisible = depth == base::DEPTH_LOOPTAKE || depth == base::DEPTH_LOOP;
	_stationControls.Update(stations, _viewProj, _size, ctrlHeld, stationDitchVisible, takeDitchVisible,
		controlsAvailable);
	_stationControls.InitResources(resourceLib);
	if (_hudPanel)
		_hudPanel->InitResources(resourceLib, false);
}


bool Workspace::AdvancePresentationLocked(const std::vector<std::shared_ptr<Station>>& stations, LoopGridEditor& editor,
	VstRackControls& racks, const PerformanceGroups& groups, const AnimationPresentation& presentation)
{
	// Window calls once per UI frame before deferred hover and drawing.
	// This clock is independent of the transport/audio callback.
	const auto now = Timer::GetTime();
	const float elapsed = _lastPanelAnimationTime
		? static_cast<float>(Timer::GetElapsedSeconds(*_lastPanelAnimationTime, now)) : 0.0f;
	_lastPanelAnimationTime = now;
	const bool mainChanged = _mainPanel && _mainPanel->AdvanceAnimation(elapsed);
	const bool selectionChanged = _selectionPanel && _selectionPanel->AdvanceAnimation(elapsed);
	if (mainChanged || selectionChanged) _pointer.InvalidateHover();
	bool stationsMoved = false;
	for (const auto& station : stations) stationsMoved |= station->AdvancePresentationMove(elapsed);
	const auto previousViewProjection = _viewProj;
	UpdateViewProjectionLocked(stations);
	const bool cameraMoved = previousViewProjection != _viewProj;
	UpdateHudStationAnchorsLocked(stations);
	const auto depth = _selector->CurrentSelectDepth();
	const bool stationDitchVisible = depth == base::DEPTH_STATION;
	const bool takeDitchVisible = depth == base::DEPTH_LOOPTAKE || depth == base::DEPTH_LOOP;
	if (_hudPanel && (_popupManager.IsOpen() || editor.IsEngaged() || presentation.Quitting))
		_hudPanel->CancelTriggerRenames();
	for (const auto& station : stations)
		if (auto label = station->GetGuiRack()->StationLabel())
		{
			if (_popupManager.IsOpen() || editor.IsEngaged() || presentation.Quitting) label->Cancel();
			label->Update(station->Name(), presentation.RigRevision, presentation.CtrlHeld,
				!station->IsRemote() && !_popupManager.IsOpen() && !editor.IsEngaged() && !presentation.StationChangePending && !presentation.RigSaveInProgress,
				!presentation.RigEditsEnabled);
		}
	_stationControls.Update(stations, _viewProj, _size, presentation.CtrlHeld, stationDitchVisible, takeDitchVisible,
		presentation.StationControlsAvailable);
	if (stationsMoved || cameraMoved)
	{
		_pointer.InvalidateHover();
	}
	UpdateRackVisibilityLocked(stations, editor.IsEngaged());
	racks.UpdateLocked(static_cast<base::SelectDepth>(depth));
	for (const auto& station : stations)
		if (const auto rack = station->GetGuiRack()->VstRack())
		{
			rack->SetViewport(_size);
			if (_popupManager.IsOpen() || editor.IsEngaged() || _focusManager.IsEditingText() ||
				presentation.Quitting || presentation.RigSaveInProgress)
			{ rack->ClearPointerState(); rack->SetRevealHeld(false); }
		}

	if (_selectionPanel) _selectionPanel->RefreshPerformanceGroups(groups);
	if (_hudPanel)
	{
		if (_selectionPanel) _hudPanel->SetTopPanelLayout(_selectionPanel->TopPanelLayout());
		_hudPanel->SetPerformanceGroupFeedback(presentation.GroupFeedback);
		if (_mainPanel) _mainPanel->SetOccupiedTopBounds(
			_selectionPanel ? _selectionPanel->TopPanelLayout().PresentedBounds : Rect2d{}, _hudPanel->OccupiedTopStripBounds());
	}
	return stationsMoved || cameraMoved;
}

actions::ActionResult Workspace::OnTouch(actions::TouchAction action, StationAccess access, const InputPresentation& presentation)
{
	if (_popupManager.ConsumeDismissedRelease(action))
	{
		_pointer.ReleaseCapture(); _pointer.ClearConsumedRelease();
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (action.State == TouchAction::TOUCH_DOWN) _cameraPointerPreservesIdle = false;
	const auto result = _OnPointerAction(action, access, presentation);
	if (std::dynamic_pointer_cast<gui::GuiPerformanceGroupButton>(_pointer.Captured()))
		_cameraPointerPreservesIdle = true;
	if (!_cameraPointerPreservesIdle) _camera.HumanActivity(Timer::GetTime());
	return result;
}

actions::ActionResult Workspace::OnMove(actions::TouchMoveAction action, StationAccess access, const InputPresentation& presentation)
{
	const auto result = _OnPointerMove(action, access, presentation);
	if (action.Origin == TouchMoveAction::Provenance::DeliveredPointer
		&& (!_cameraPointerPreservesIdle || action.MouseButtonsDown == 0u)) _camera.HumanActivity(Timer::GetTime());
	return result;
}

actions::ActionResult Workspace::_OnPointerAction(actions::TouchAction action, StationAccess access, const InputPresentation& presentation)
{
	if (presentation.Quitting || presentation.WriterHeld) return ActionResult::NoAction();
	ActionResult res;
	// Wheel events are encoded as DOWN/index 4 and have no matching UP. They
	// must not become pointer captures or replace keyboard focus.
	const bool pointerPress = PointerInteraction::IsPress(action);
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(presentation.Config);
	_cursorPos = action.Position;
	InvalidateHover2d();
	if (presentation.RigSaveDialogOpen) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	if (const auto rack = std::dynamic_pointer_cast<gui::GuiVstRack>(_pointer.Captured()))
	{
		if (_popupManager.IsOpen() || _editor->IsEngaged() || EditControlsSuppressed())
		{ rack->ClearPointerState(); _pointer.ReleaseCapture(); }
		else
		{
			return _pointer.DispatchCapture(action, PointerInteraction::Release::LeftUp, false);
		}
	}
	if (_selector->OwnsBackgroundClick() && (_popupManager.IsOpen() || EditControlsSuppressed() || _editor->IsEngaged() ||
		presentation.QuantisationResetRequested))
		_selector->ClearBackgroundClick();
	if (_selector->OwnsBackgroundClick())
	{
		if (const auto command = _selector->ReleaseBackgroundClick(action))
		{
			{
				std::scoped_lock lock(access.Mutex);
				if (*command == SceneSelector::BackgroundCommand::SelectAll) _SelectAllLocalLocked(access, presentation);
				else
				{
					_inputCommands.MuteAll = *command == SceneSelector::BackgroundCommand::MuteAll;
				}
			}
			if (*command == SceneSelector::BackgroundCommand::SelectAll)
			{
				InvalidateHover2d();
				UpdateSelection(ACTIONRESULT_DEFAULT, access, presentation);
			}
		}
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_popupManager.IsOpen())
	{
		_editor->CancelInput();
		auto popupRes = _popupManager.OnAction(action);

		if (_takeDitchDialog && !_popupManager.IsOpen()) _inputCommands.CancelTakeDitch = true;
		if (_stationDeleteDialog && !_popupManager.IsOpen())
		_inputCommands.CancelStationRemoval = true;
		if (TouchAction::TOUCH_UP == action.State)
		{
			_pointer.ReleaseCapture();
			_pointer.ClearConsumedRelease();
		}
		return popupRes;
	}
	if (_hudPanel && _hudPanel->RouteRenameTouch(_hudPanel->GlobalToLocal(action)))
	{
		if (action.State == TouchAction::TOUCH_UP && _pointer.IsCapturedBy(PointerInteraction::Owner::Hud) && !_hudPanel->HasLabelPress())
		{ _pointer.ReleaseCapture(); }
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_editor->IsEngaged())
	{
		const auto stations = access.Snapshot();
		for (const auto& station : stations)
			if (auto label = station->GetGuiRack()->StationLabel(); label && label->OwnsPointer())
			{
				if (label->Touch(label->GlobalToLocal(action))) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
			}
	}
	// Existing captures and drafts keep their gesture. Fresh panel/HUD hits
	// precede projected station overlays, which may lie behind these controls.
	if (!EditControlsSuppressed() && !_interaction->OwnsPointer())
		if (auto settingsResult = _RouteSettingsTouch(action, access, presentation)) return *settingsResult;
	if (_pointer.IsCapturedBy(PointerInteraction::Owner::Hud))
	{
		std::scoped_lock lock(access.Mutex);
		return _pointer.DispatchCapture(action, PointerInteraction::Release::AnyUp, true);
	}
	if (!EditControlsSuppressed() && !_pointer.Captured() && !_camera.IsBackgroundDragging() && !_isSceneTouching &&
		!_editor->OwnsPointer() && !_editor->IsOrbitDragging() && !_interaction->OwnsPointer() && _hudPanel &&
		(_hudPanel->CapturesPointer(_hudPanel->GlobalToLocal(action.Position)) || _hudPanel->HasCableDrag()))
	{
		// Routing callbacks require scene serialization; focus follows after unlock.
		auto result = ActionResult::NoAction();
		{ std::scoped_lock lock(access.Mutex); result = _hudPanel->OnAction(_hudPanel->GlobalToLocal(action)); }
		if (result.IsEaten)
		{
			if (pointerPress)
			{
				auto active = result.ActiveElement.lock();
				if (!active) active = _hudPanel;
				_pointer.Capture(active, PointerInteraction::Owner::Hud);
				PointerInteraction::FocusPress(active, _focusManager);
			}
			return result;
		}
	}
	if (!_editor->IsEngaged())
	{
		const auto stations = access.Snapshot();
		for (const auto& station : stations)
			if (auto label = station->GetGuiRack()->StationLabel(); label && station->GetGuiRack()->IsInView(_size))
				if (label->Touch(label->GlobalToLocal(action))) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_editor->IsEngaged() && !_interaction->OwnsPointer() &&
		_stationControls.Touch(action, [this, &access, &presentation](auto intent) {
			_inputCommands.StationControl = std::move(intent);
		}))
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	if (_interaction->OwnsPointer() || (EditControlsSuppressed() && !_editor->IsEngaged()))
	{
		if (auto overlay = _RouteQuantisationTouch(action, access, presentation))
			return *overlay;
		return ActionResult::NoAction();
	}
	if (!EditControlsSuppressed() && _editor->IsEngaged() && !_editor->OwnsPointer() && !_editor->IsOrbitDragging() && _hudPanel)
	{
		std::scoped_lock lock(access.Mutex);
		auto hudResult = _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
		if (hudResult.IsEaten) return hudResult;
	}
	if (auto editorRes = _editor->OnAction(action))
		return *editorRes;
	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
	{
		// Clear the old hover now so only the capture target appears pressed.
		std::vector<std::weak_ptr<GuiElement>> none;
		ApplyHoverPath2d(none);
		_pointer.ClearHoverPath();
	}

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State)
		&& (0 == action.Index)
		&& (Action::MODIFIER_SHIFT & action.Modifiers)
		&& !(Action::MODIFIER_CTRL & action.Modifiers)
		&& _TrySetMasterFromHover(true, access, presentation))
	{
		res.IsEaten = true;
		res.ResultType = ACTIONRESULT_DEFAULT;
		return res;
	}

	if (auto overlayRes = _RouteQuantisationTouch(action, access, presentation);
		overlayRes.has_value())
	{
		return overlayRes.value();
	}

	if (TouchAction::TouchState::TOUCH_UP == action.State)
	{
		auto activeElement = _pointer.Captured();

		if (activeElement)
		{
			if (_pointer.IsCapturedBy(PointerInteraction::Owner::Hud))
			{
				std::scoped_lock lock(access.Mutex);
				res = _pointer.DispatchCapture(action, PointerInteraction::Release::Explicit, false);
			}
			else res = _pointer.DispatchCapture(action, PointerInteraction::Release::Explicit, false);

			if (res.IsEaten)
			{
				if (nullptr != res.Undo)
					_undoHistory.Add(res.Undo);
			}
		}
		else if (_camera.IsBackgroundDragging())
		{
			auto wasDragged = _camera.BackgroundDragWasDragged();
			_camera.HandleBackgroundDrag(action);
			EndBackgroundDrag(access, presentation);

			// Clear selection after an unhandled background drag.
			if (!wasDragged && !_camera.IsBackgroundDragging())
				UpdateSelection(ACTIONRESULT_CLEARSELECT, access, presentation);
		}

		// Update selector and then react to result (selecting, deselecting, muting, unmuting)
		res = _selector->OnAction(_selector->ParentToLocal(action));

		UpdateSelection(res.ResultType, access, presentation);

		_pointer.ReleaseCapture();

		return ActionResult::NoAction();
	}

	for (auto it = _guiChildren.rbegin(); it != _guiChildren.rend(); ++it)
	{
		if (!*it)
			continue;

		const auto& child = *it;
		const auto isHudChild = (child == _hudPanel);
		if (isHudChild)
		{
			std::scoped_lock lock(access.Mutex);
			res = child->OnAction(child->ParentToLocal(action));
		}
		else
			res = child->OnAction(child->ParentToLocal(action));
		if (_pointer.CompleteDispatch(action, res,
			isHudChild ? PointerInteraction::Owner::Hud : PointerInteraction::Owner::General, &_focusManager, _undoHistory)) return res;
	}

	for (auto& station : access.Snapshot())
	{
		res = static_cast<std::shared_ptr<base::GuiElement>>(station)->OnAction(station->ParentToLocal(action));

		if (_pointer.CompleteDispatch(action, res, PointerInteraction::Owner::General, nullptr, _undoHistory)) return res;
	}

	res = _selector->OnAction(_selector->ParentToLocal(action));

	UpdateSelection(res.ResultType, access, presentation);

	if (res.IsEaten)
	{
		_cameraPointerPreservesIdle = true;
		if (nullptr != res.Undo)
			_undoHistory.Add(res.Undo);

		return res;
	}

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State) && (4 == action.Index))
		return _camera.HandleWheel(action.Value,
			action.Position,
			_size.Width,
			_size.Height,
			StationCentre(access.Snapshot()));

	// Pressing empty background clears keyboard focus.
	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
		_focusManager.ClearFocus();

	if (pointerPress && action.Touch == TouchAction::TOUCH_MOUSE && !_camera.IsBackgroundPointerHeld() && !_isSceneTouching)
	{
		// Resolve actual targets: the pass-through HUD spans the viewport but
		// must not claim its empty space as a control.
		_ResolveHoverPath2d(_pointer.HoverScratch(), access, presentation); // Includes station racks and controls.
		// Chords retain camera/selection meaning; only a single-button empty hit starts a click.
		if (_pointer.HoverScratch().empty() && !_HasQuantisationHover(access, presentation) && _selector->BeginBackgroundClick(action))
		{
			_camera.CancelBackgroundCoast();
			_cameraPointerPreservesIdle = true;
			return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
		}
	}

	return _BeginBackgroundDrag(action, access, presentation);
}

actions::ActionResult Workspace::_OnPointerMove(actions::TouchMoveAction action, StationAccess access, const InputPresentation& presentation)
{
	if (presentation.Quitting || presentation.WriterHeld) return ActionResult::NoAction();
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(presentation.Config);
	_cursorPos = action.Position;
	InvalidateHover2d();
	if (_selector->OwnsBackgroundClick())
	{
		_selector->MoveBackgroundClick(action, !_popupManager.IsOpen() && !EditControlsSuppressed() && !_editor->IsEngaged()
			&& !presentation.QuantisationResetRequested);
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_popupManager.IsOpen() && _pointer.IsCapturedBy(PointerInteraction::Owner::Hud))
	{
		std::scoped_lock lock(access.Mutex);
		return _pointer.DispatchCapture(action, PointerInteraction::CaptureLoss::MouseZeroButtons, true);
	}
	if (!_popupManager.IsOpen() && !EditControlsSuppressed() && !_pointer.Captured() &&
		!_camera.IsBackgroundDragging() && !_isSceneTouching && !_editor->OwnsPointer() && !_editor->IsOrbitDragging() &&
		!_interaction->OwnsPointer() && _hudPanel && _hudPanel->CapturesPointer(_hudPanel->GlobalToLocal(action.Position)))
	{
		std::scoped_lock lock(access.Mutex);
		_hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_popupManager.IsOpen() && !_editor->IsEngaged() && !_interaction->OwnsPointer())
	{
		for (const auto& station : access.Snapshot())
		{
			if (auto label = station->GetGuiRack()->StationLabel())
				label->Move(label->GlobalToLocal(action));
			if (auto rack = station->GetGuiRack()->VstRack())
				rack->MovePointer(rack->GlobalToLocal(action));
		}
	}
	if (!_popupManager.IsOpen() && !_editor->IsEngaged() && _stationControls.Move(action))
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	if (_interaction->OwnsPointer())
	{
		if (action.MouseButtonsDown == 0u)
			_interaction->CancelInteraction();
		else if (auto overlay = _RouteQuantisationMove(action, access, presentation))
			return *overlay;
		return ActionResult::NoAction();
	}
	if (EditControlsSuppressed() && !_editor->IsEngaged())
		return ActionResult::NoAction();
	if (_popupManager.IsOpen())
	{
		_editor->CancelInput();
		return _popupManager.OnAction(action);
	}
	const std::array<std::shared_ptr<GuiElement>, 2> panels{ _selectionPanel, _mainPanel };
	if (auto panelResult = _pointer.RoutePanels(action, panels,
		EditControlsSuppressed() || _camera.IsBackgroundDragging() || _isSceneTouching
			|| _editor->OwnsPointer() || _editor->IsOrbitDragging() || _interaction->OwnsPointer()))
		return *panelResult;
	if (!EditControlsSuppressed() && _editor->IsEngaged() && !_editor->OwnsPointer() && !_editor->IsOrbitDragging() && _hudPanel)
	{
		std::scoped_lock lock(access.Mutex);
		auto hudResult = _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
		if (hudResult.IsEaten) return hudResult;
	}
	if (auto editorRes = _editor->OnAction(action))
		return *editorRes;

	if (auto overlayRes = _RouteQuantisationMove(action, access, presentation);
		overlayRes.has_value())
	{
		return overlayRes.value();
	}

	if (_camera.IsBackgroundDragging() && (0u != action.MouseButtonsDown))
		return _UpdateBackgroundDrag(action, access, presentation);

	if (auto active = _pointer.Captured())
	{
		if (_pointer.IsCapturedBy(PointerInteraction::Owner::Hud))
		{
			std::scoped_lock lock(access.Mutex);
			return _pointer.DispatchCapture(action, PointerInteraction::CaptureLoss::MouseZeroButtons, false);
		}
		return _pointer.DispatchCapture(action, PointerInteraction::CaptureLoss::MouseZeroButtons, false);
	}

	if (_isSceneTouching)
		return _UpdateBackgroundDrag(action, access, presentation);
	{
		auto selectionMove = _selector->OnAction(_selector->ParentToLocal(action));
		if (selectionMove.IsEaten)
		{
			UpdateSelection(selectionMove.ResultType, access, presentation);
			return selectionMove;
		}
	}
	if (_hudPanel)
	{
		// Lock the HUD tree while the job thread rebuilds it.
		std::scoped_lock lock(access.Mutex);
		return _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
	}

	return ActionResult::NoAction();
}

void Workspace::OnSettingsHidden(const std::shared_ptr<base::GuiElement>& subtree)
{
	_pointer.OnSubtreeHidden(subtree, _focusManager);
}

std::optional<actions::ActionResult> Workspace::_RouteSettingsTouch(actions::TouchAction action, StationAccess access, const InputPresentation& presentation)
{
	const std::array<std::shared_ptr<GuiElement>, 2> panels{ _selectionPanel, _mainPanel };
	return _pointer.RoutePanels(action, panels, _focusManager,
		_camera.IsBackgroundDragging() || _isSceneTouching || _editor->OwnsPointer()
			|| _editor->IsOrbitDragging() || _interaction->OwnsPointer());
}

bool Workspace::BackgroundPreviewActive(StationAccess access, const InputPresentation& presentation) const
{
	return _selector->BackgroundPreview().has_value() && !_popupManager.IsOpen()
		&& !EditControlsSuppressed() && !_editor->IsEngaged()
		&& !presentation.QuantisationResetRequested;
}

bool Workspace::BackgroundSelectionPreviewActive(StationAccess access, const InputPresentation& presentation) const
{
	return BackgroundPreviewActive(access, presentation) && _selector->BackgroundPreview() == SceneSelector::BackgroundCommand::SelectAll;
}

void Workspace::_SelectAllLocalLocked(StationAccess access, const InputPresentation& presentation)
{
	CollapseRacksLocked(access.Stations);
	HierarchySelection::SelectAllLocal(HierarchySelection::BorrowLocked(access.Stations), _selector->CurrentSelectDepth());
}

void Workspace::SetHover3d(std::vector<unsigned char> path, base::Action::Modifiers modifiers, std::optional<utils::Position2d> pointerPosition, StationAccess access, const InputPresentation& presentation)
{
	if (_ctrlHeld || _editor->IsEngaged() || _selector->OwnsBackgroundClick())
		return;
	if (pointerPosition)
	{
		_cursorPos = *pointerPosition;
		InvalidateHover2d();
	}
	// Resolve GUI ownership before taking the scene lock: station snapshots also lock it.
	std::vector<std::weak_ptr<base::GuiElement>> guiPath;
	_ResolveHoverPath2d(guiPath, access, presentation);
	std::unique_lock lock(access.Mutex);
	bool isSelected = false;
	auto tweakState = base::Tweakable::TweakState::TWEAKSTATE_NONE;
	std::vector<unsigned char> fullElementPath;
	std::vector<unsigned char> elementPath;

	for (auto segment : path)
	{
		// Picker path uses 0xFF as the terminator for missing path segments.
		if ((0xFF == segment) || (0 == segment))
			break;

		fullElementPath.push_back(segment - 1);
	}

	_hoverPath3d = fullElementPath;
	_hoverElement3d = _ChildFromPathLocked(fullElementPath, access);
	elementPath = fullElementPath;
	// Keep the picked stream identity; MIDI layers are independent loop targets.

	elementPath = SceneSelector::PathAtDepth(elementPath, _selector->CurrentSelectDepth() + 1);
	// Rack controls own the pointer even when the 3D picker sees a model behind them.
	if (std::any_of(guiPath.begin(), guiPath.end(), [](const auto& element) {
		return nullptr != std::dynamic_pointer_cast<GuiRack>(element.lock());
	}))
		elementPath.clear();

	if (elementPath != _lastLoggedHoverPath)
	{
		if (presentation.Verbose)
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
		}

		_lastLoggedHoverPath = elementPath;
	}

	auto hovering = _ChildFromPathLocked(elementPath, access);
	if (nullptr != hovering)
	{
		isSelected = hovering->IsSelected();

		tweakState = _SelectionTweakStateLocked(hovering, access, presentation);
	}

	_selector->UpdateCurrentHover(elementPath,
		modifiers,
		isSelected,
		tweakState);
	_interaction->RefreshOverlay(InteractionContextLocked(access, presentation),
		[this, &access, &presentation](const std::vector<unsigned char>& path) { return _ChildFromPathLocked(path, access); });

	lock.unlock();
	UpdateSelection(ACTIONRESULT_DEFAULT, access, presentation);

	auto candidate = (Action::MODIFIER_SHIFT & modifiers) ? _ChildFromPath(elementPath, access) : nullptr;
	_UpdateStationQuantisation(candidate, _selector->CurrentSelectDepth(), false, access, presentation);
}

void Workspace::ApplyDeferredHoverUpdates(StationAccess access, const InputPresentation& presentation)
{
	// Ctrl edits the captured targets, so pointer motion must not change their feedback.
	if (_ctrlHeld)
		return;
	// Keep active hover in sync with both deferred 2D hit-testing and latest 3D picker result.
	if (!_pointer.HoverDirty())
		return;

	if (_popupManager.IsOpen())
	{
		if (!_pointer.HoverPath2d().empty())
		{
			std::vector<std::weak_ptr<base::GuiElement>> none;
			ApplyHoverPath2d(none);
			_pointer.ClearHoverPath();
		}

		_pointer.HoverResolved();
		return;
	}

	if (_pointer.Captured() || _isSceneTouching || _selector->OwnsBackgroundClick())
		return;

	_pointer.HoverScratch().clear();
	_ResolveHoverPath2d(_pointer.HoverScratch(), access, presentation);
	auto& nextPath = _pointer.HoverScratch();
	ApplyHoverPath2d(nextPath);

	bool stationHoverPromotedFrom2d = false;
	const bool rackOwnsHover = std::any_of(nextPath.begin(), nextPath.end(), [](const auto& element) {
		return nullptr != std::dynamic_pointer_cast<GuiRack>(element.lock());
	});

	if (!nextPath.empty() && !rackOwnsHover)
	{
		const auto& nextShared = _pointer.HoverElements();
		auto stationIt = std::find_if(nextShared.begin(), nextShared.end(), [](const std::shared_ptr<base::GuiElement>& element) {
			return nullptr != std::dynamic_pointer_cast<Station>(element);
		});

		if (stationIt != nextShared.end())
		{
			auto stationGlobalId = (*stationIt)->GlobalId();
			std::vector<unsigned char> stationPathRaw;
			stationPathRaw.reserve(stationGlobalId.size());
			for (auto part : stationGlobalId)
				stationPathRaw.push_back(static_cast<unsigned char>(part & 0xFFu));

			auto stationPath = SceneSelector::PathAtDepth(stationPathRaw, _selector->CurrentSelectDepth() + 1u);
			bool isSelected = false;
			auto tweakState = base::Tweakable::TweakState::TWEAKSTATE_NONE;
			auto hovering = _ChildFromPath(stationPath, access);
			if (hovering)
			{
				isSelected = hovering->IsSelected();
				if (auto tweakable = std::dynamic_pointer_cast<Tweakable>(hovering))
					tweakState = tweakable->IsMuted() ? Tweakable::TWEAKSTATE_MUTED : Tweakable::TWEAKSTATE_NONE;
			}

			_selector->UpdateCurrentHover(stationPath,
				Action::MODIFIER_NONE,
				isSelected,
				tweakState);
			UpdateSelection(ACTIONRESULT_DEFAULT, access, presentation);
			stationHoverPromotedFrom2d = true;
		}
	}

	if (!stationHoverPromotedFrom2d && (rackOwnsHover || _hoverPath3d.empty()) && !_selector->CurrentHover().empty())
	{
		_selector->UpdateCurrentHover({ },
			Action::MODIFIER_NONE,
			false,
			base::Tweakable::TweakState::TWEAKSTATE_NONE);
		UpdateSelection(ACTIONRESULT_DEFAULT, access, presentation);
	}
	else if (!stationHoverPromotedFrom2d && !rackOwnsHover && !_hoverPath3d.empty())
	{
		// The picker may keep the same model ID while the pointer leaves a control.
		auto pickPath = _hoverPath3d;
		for (auto& segment : pickPath)
			++segment;
		SetHover3d(std::move(pickPath), Action::MODIFIER_NONE, std::nullopt, access, presentation);
	}

	_pointer.CommitHover(std::move(nextPath));
}

void Workspace::InvalidateHover2d()
{
	_pointer.InvalidateHover();
}

void Workspace::_ResolveHoverPath2d(std::vector<std::weak_ptr<base::GuiElement>>& outPath, StationAccess access, const InputPresentation& presentation)
{
	if (EditControlsSuppressed() && !_editor->IsEngaged()) { outPath.clear(); return; }
	if (PointerInteraction::ResolveHoverPath(_guiChildren, true, _cursorPos, outPath)) return;
	std::vector<std::shared_ptr<GuiElement>> stationRoots;
	for (const auto& station : access.Snapshot()) stationRoots.push_back(station);
	PointerInteraction::ResolveHoverPath(stationRoots, false, _cursorPos, outPath);
}

void Workspace::ApplyHoverPath2d(const std::vector<std::weak_ptr<base::GuiElement>>& nextPath)
{
	_pointer.ApplyHover(nextPath, _cursorPos);
}

void Workspace::UpdateSelection(actions::ActionResultType res, StationAccess access, const InputPresentation& presentation)
{
	std::scoped_lock lock(access.Mutex);
	const auto view = HierarchySelection::BorrowLocked(access.Stations);
	const auto update = _selector->SelectionChanges(res);
	HierarchySelection::EditBatch batch{ update.ClearSelection, _selector->CurrentSelectDepth(), {}, update.ChangeCount,
		{ update.HoverPath, update.ClearPicking, update.HighlightHover, update.HighlightMidiSiblings, update.ClickPressed, update.MutePressed } };
	for (std::size_t index = 0u; index < update.ChangeCount; ++index)
	{
		const auto& change = update.Changes[index];
		using Edit = SceneSelector::HierarchyEdit;
		const auto operation = change.Edit == Edit::Select ? HierarchySelection::Edit::Select :
			change.Edit == Edit::Deselect ? HierarchySelection::Edit::Deselect :
			change.Edit == Edit::Mute ? HierarchySelection::Edit::Mute : HierarchySelection::Edit::Unmute;
		batch.Changes[index] = { operation, change.Path };
	}
	if (HierarchySelection::ApplyLocked(view, batch).CollapseRacks) CollapseRacksLocked(access.Stations);

	if (auto hovered = _ChildFromPathLocked(_selector->CurrentHover(), access))
	{
		const auto tweakState = _SelectionTweakStateLocked(hovered, access, presentation);
		_selector->UpdateCurrentHover(_selector->CurrentHover(),
			Action::MODIFIER_NONE, hovered->IsSelected(), tweakState);
	}

	_interaction->RefreshOverlay(InteractionContextLocked(access, presentation),
		[this, &access, &presentation](const std::vector<unsigned char>& path) { return _ChildFromPathLocked(path, access); });
}

base::Tweakable::TweakState Workspace::_SelectionTweakStateLocked(const std::shared_ptr<base::GuiElement>& target, StationAccess access, const InputPresentation& presentation) const
{
	return HierarchySelection::TweakState(HierarchySelection::BorrowLocked(access.Stations), target);
}

actions::ActionResult Workspace::_BeginBackgroundDrag(actions::TouchAction action, StationAccess access, const InputPresentation& presentation)
{
	// A background click only deselects. Activity begins when it actually pans.
	_cameraPointerPreservesIdle = true;
	auto res = _camera.HandleBackgroundDrag(action);
	_isSceneTouching = _camera.IsBackgroundDragging();
	return res;
}

actions::ActionResult Workspace::_UpdateBackgroundDrag(actions::TouchMoveAction action, StationAccess access, const InputPresentation& presentation)
{
	auto res = _camera.UpdateBackgroundDrag(action);
	if (_camera.BackgroundDragWasDragged()) _cameraPointerPreservesIdle = false;
	if (!_camera.IsBackgroundDragging())
		EndBackgroundDrag(access, presentation);

	return res;
}

void Workspace::EndBackgroundDrag(StationAccess access, const InputPresentation& presentation)
{
	_isSceneTouching = false;
}

bool Workspace::_TrySetMasterFromHover(bool confirm, StationAccess access, const InputPresentation& presentation)
{
	std::scoped_lock lock(access.Mutex);
	return _quantiser->TrySetMasterFromHover(_ChildFromPathLocked(_selector->CurrentHover(), access),
		_selector->CurrentSelectDepth(),
		access.Stations,
		presentation.Stream.SampleRate,
		presentation.Config,
		confirm);
}

void Workspace::_UpdateStationQuantisation(std::shared_ptr<base::GuiElement> candidate, base::SelectDepth depth, bool confirmCandidate, StationAccess access, const InputPresentation& presentation)
{
	std::scoped_lock lock(access.Mutex);
	_quantiser->UpdateStationHints(candidate, depth, confirmCandidate, access.Stations);
}

bool Workspace::_HasQuantisationHover(StationAccess access, const InputPresentation& presentation) const
{
	return !_selector->CurrentHover().empty() || !_hoverPath3d.empty();
}

bool Workspace::EditControlsSuppressed() const
{
	return _interaction->EditModeActive() || _interaction->PanelAlpha() > 0.001f;
}

std::optional<actions::ActionResult> Workspace::_RouteQuantisationTouch(actions::TouchAction action, StationAccess access, const InputPresentation& presentation)
{
	std::scoped_lock lock(access.Mutex);
	return _interaction->TryHandleTouchAction(action, presentation.Stream.SampleRate,
		_ctrlHeld, InteractionContextLocked(access, presentation), [this, &access, &presentation](const auto& path) { return _ChildFromPathLocked(path, access); });
}

std::optional<actions::ActionResult> Workspace::_RouteQuantisationMove(actions::TouchMoveAction action, StationAccess access, const InputPresentation& presentation)
{
	std::scoped_lock lock(access.Mutex);
	return _interaction->TryHandleTouchMove(action, presentation.Stream.SampleRate);
}

void Workspace::BindQuantisation(Quantiser& quantiser, QuantiserController& interaction)
{
	_quantiser = &quantiser;
	_interaction = &interaction;
	interaction.SetFeedbackSink([this](const std::string& text) { _quantisationFeedback = text; });
}

std::shared_ptr<GuiElement> Workspace::_ChildFromPathLocked(const std::vector<unsigned char>& path, StationAccess access)
{
	return HierarchySelection::Resolve(HierarchySelection::BorrowLocked(access.Stations), path);
}

std::shared_ptr<GuiElement> Workspace::_ChildFromPath(const std::vector<unsigned char>& path, StationAccess access)
{
	std::scoped_lock lock(access.Mutex);
	return _ChildFromPathLocked(path, access);
}

bool Workspace::_HasSelectionLocked(StationAccess access)
{
	return HierarchySelection::HasSelection(HierarchySelection::BorrowLocked(access.Stations));
}


void Workspace::InitializeEditor(std::mutex& mutex, const std::vector<std::shared_ptr<Station>>& stations, std::function<void()> onOpened)
{
	_editor = std::make_unique<LoopGridEditor>(LoopGridEditor::Host{
		_camera, _undoHistory, mutex, stations,
		[this] { return _hoverElement3d.lock(); },
		[this, &stations](float aspect) { return _camera.Projection(aspect, StationCentre(stations)) * _camera.ViewMatrix(); },
		std::move(onOpened),
		[this](int button, Position2d anchor) { return _beginRelativePointer && _beginRelativePointer(button, anchor); },
		[this](int button) { if (_endRelativePointer) _endRelativePointer(button); },
		[this] { const auto clock = _quantiser->Clock(); return clock ? clock->SeedSourceLength() : 0ul; },
		[this] { return _selector ? _selector->CurrentSelectDepth() : base::DEPTH_LOOP; },
		[this] { return _logicalPixelScale ? _logicalPixelScale() : 1.0f; } }, _size);
}


QuantisationInteractionContext Workspace::InteractionContextLocked(StationAccess access, const InputPresentation& presentation) const
{
	return _interaction->InteractionContextLocked(_cursorPos, _size, _selector->CurrentSelectDepth(),
		_selector->CurrentHover(), _hoverPath3d, presentation.Stream.SampleRate);
}

actions::ActionResult Workspace::OnKeyboard(KeyAction action, StationAccess access, const InputPresentation& presentation,
	bool contextual, bool suppressedKey, bool freshLearnKey, io::IoInputSubsystem& input, const KeyboardHandler& handleKeyboard)
{
	// Physical modifier transitions must survive contextual editor consumption.
	if (17u == action.KeyChar)
	{
		const bool wasCtrlHeld = _ctrlHeld;
		_ctrlHeld = actions::KeyAction::KEY_DOWN == action.KeyActionType;
		// The loop grid keeps its existing Ctrl/Space overlay gestures; only its
		// text editor (and true modal/text capture) suppresses those shortcuts.
		const bool contextualText = presentation.RigSaveDialogOpen || presentation.WriterHeld ||
			_popupManager.IsOpen() || _focusManager.IsEditingText() || (*_editor).IsEditingText();
		{
			std::scoped_lock lock(access.Mutex);
			_interaction->OnCtrlModifierChanged(_ctrlHeld && !contextualText, action.GetActionTime(),
				InteractionContextLocked(access, presentation), [this, &access, &presentation](const auto& path) { return _ChildFromPathLocked(path, access); });
		}
		if (_ctrlHeld && !contextualText && !(*_editor).IsEngaged())
		{
			_focusManager.ClearFocus();
			if (auto active = _pointer.Captured()) active->ClearPointerState();
			_pointer.ReleaseCapture();
			if (!wasCtrlHeld && _HasSelection(access))
			{
				// Targets were captured above; hide hover when editing the selection.
				ApplyHoverPath2d({});
				_pointer.ClearHoverPath();
				{
					std::scoped_lock lock(access.Mutex);
					_hoverPath3d.clear();
					_hoverElement3d.reset();
					_selector->UpdateCurrentHover({}, Action::MODIFIER_NONE, false,
						Tweakable::TWEAKSTATE_NONE);
					for (const auto& station : access.Stations)
						station->SetPicking3d(false);
				}
			}
		}
		if (wasCtrlHeld && !_ctrlHeld) { _stationControls.CancelBinPress(); InvalidateHover2d(); }
	}
	if (32u == action.KeyChar)
	{
		if (actions::KeyAction::KEY_UP == action.KeyActionType)
		{
			_spaceHeld = false;
			_quantiser->SetOverlayHeld(false);
		}
		else if (!_spaceHeld)
		{
			_spaceHeld = true;
			if (!_popupManager.IsOpen() && !_focusManager.IsEditingText() && !(*_editor).IsEditingText())
			{
				_quantiser->SetOverlayHeld(true);
				handleKeyboard(KeyboardCommand::TapTempo, action, false);
			}
		}
		// A text consumer still receives Space, but the physical release above
		// always unwinds a previous hold, even after focus changes.
		if (!_popupManager.IsOpen() && !_focusManager.IsEditingText() && !(*_editor).IsEditingText())
			return ActionResult::NoAction();
	}
	if (presentation.WriterHeld) return ActionResult::NoAction();
	if (suppressedKey && !contextual) return ActionResult::NoAction();
	if (27u == action.KeyChar && _interaction->OwnsPointer())
	{
		_interaction->CancelInteraction();
		return ActionResult::NoAction();
	}

	if (_popupManager.Top() == _takeDitchDialog && _takeDitchDialog &&
		action.KeyChar == 13u && action.KeyActionType == KeyAction::KEY_UP)
	{
		_popupManager.Close();
		_inputCommands.CancelTakeDitch = true;
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_popupManager.Top() == _stationDeleteDialog && _stationDeleteDialog &&
		action.KeyChar == 13u && action.KeyActionType == KeyAction::KEY_UP)
	{
		_popupManager.Close();
		_inputCommands.CancelStationRemoval = true;
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_popupManager.IsOpen())
	{
		{ std::scoped_lock lock(access.Mutex); if (_hudPanel && _hudPanel->TriggerLearnKeyboard(action, freshLearnKey)) input.ObserveTriggerLearnKey(action.KeyChar); }
		(*_editor).CancelInput();
		auto popupRes = _popupManager.OnAction(action);

		if (_takeDitchDialog && !_popupManager.IsOpen()) _inputCommands.CancelTakeDitch = true;
		if (_stationDeleteDialog && !_popupManager.IsOpen())
		_inputCommands.CancelStationRemoval = true;
		if ((*_editor).IsEngaged())
			popupRes.IsEaten = true;
		if (popupRes.IsEaten)
			return popupRes;
	}

	// Text entry owns shortcuts, including channel overrides and cable reveal.
	if (auto focus = _focusManager.CurrentFocus())
	{
		const bool editingText = _focusManager.IsEditingText();
		auto focusRes = focus->OnAction(action);
		if (focusRes.IsEaten) return focusRes;
		if (editingText) return ActionResult::NoAction();
	}

	if ((192u == action.KeyChar) || (96u == action.KeyChar))
	{
		if (_hudPanel)
		{
			std::scoped_lock lock(access.Mutex);
			_hudPanel->SetCableRevealHeld(actions::KeyAction::KEY_DOWN == action.KeyActionType);
			for (const auto& station : access.Stations)
				if (auto rack = station->GetGuiRack()->VstRack())
					rack->SetRevealHeld(actions::KeyAction::KEY_DOWN == action.KeyActionType);
		}
		return ActionResult::NoAction();
	}
	if (_hudPanel)
	{
		std::scoped_lock lock(access.Mutex);
		for (const auto& station : access.Stations)
			if (auto rack = station->GetGuiRack()->VstRack())
				if (auto result = rack->OnAction(action); result.IsEaten) return result;
		if (_hudPanel->HasCableDrag())
		{
			auto hudResult = _hudPanel->OnAction(action);
			if (hudResult.IsEaten)
				return hudResult;
		}
	}

	if (auto editorRes = (*_editor).OnAction(action))
		return *editorRes;

	if (auto editorRes = (*_editor).TryOpenFromKey(action))
		return *editorRes;
	if ((9u == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_NONE == action.Modifiers))
	{
		CycleCameraView(access, presentation);
		ActionResult result;
		result.IsEaten = true;
		return result;
	}

	if ((90 == action.KeyChar) && (actions::KeyAction::KEY_UP == action.KeyActionType) && (Action::MODIFIER_CTRL & action.Modifiers))
	{
		return { _undoHistory.Undo() };
	}

	if (auto result = _racks->OnKeyboard(action, _selector->CurrentSelectDepth(),
		_ChildFromPath(_selector->CurrentHover(), access), access.Snapshot())) return *result;

	if (const auto result = handleKeyboard(KeyboardCommand::ApplyKey, action,
		!_popupManager.IsOpen() && !_focusManager.IsEditingText() && !_editor->IsEngaged())) return *result;

	auto res = _selector->OnAction(action);

	if (res.IsEaten)
	{
		UpdateSelection(res.ResultType, access, presentation);
		return res;
	}

	return ActionResult::NoAction();

}

bool Workspace::_HasSelection(StationAccess access) const
{
	std::scoped_lock lock(access.Mutex);
	return _HasSelectionLocked(access);
}

void Workspace::ReleaseQuantisationInput(StationAccess access, const InputPresentation& presentation)
{
	std::scoped_lock lock(access.Mutex);
	ReleaseQuantisationInputLocked(access, presentation);
}

void Workspace::ReleaseQuantisationInputLocked(StationAccess access, const InputPresentation& presentation)
{
	_selector->ClearBackgroundClick();
	_spaceHeld = _ctrlHeld = false;
	_quantiser->SetOverlayHeld(false);
	_interaction->CancelInteraction(true);
	_interaction->OnCtrlModifierChanged(false, Timer::GetTime(),
		InteractionContextLocked(access, presentation), [this, &access, &presentation](const auto& path) { return _ChildFromPathLocked(path, access); });
}

void Workspace::OnLoopGridEditorOpened(StationAccess access, const InputPresentation& presentation)
{
	_selector->ClearBackgroundClick();
	ReleaseQuantisationInput(access, presentation);
	if (_hudPanel)
	{
		std::scoped_lock lock(access.Mutex);
		_hudPanel->SetLoopEditorMode((*_editor).IsEngaged());
	}
	_focusManager.ClearFocus();
	_pointer.ReleaseCapture();
	_pointer.ClearConsumedRelease();
	EndBackgroundDrag(access, presentation);
	_interaction->OnCtrlModifierChanged(false, Timer::GetTime(),
		_InteractionContext(access, presentation), [this, &access, &presentation](const std::vector<unsigned char>& path) { return _ChildFromPath(path, access); });
}

void Workspace::ApplyCameraSelectDepthChange(graphics::Camera::SelectDepthChange change, StationAccess access, const InputPresentation& presentation)
{
	if (graphics::Camera::SelectDepthChange::None == change)
		return;
	const auto depth = (graphics::Camera::SelectDepthChange::Station == change) ? base::DEPTH_STATION : base::DEPTH_LOOPTAKE;
	_viewMode = depth;
	_settingsControls->Mode()->SetCurrentValue(static_cast<unsigned int>(depth), true);
	UpdateSelectDepth(static_cast<unsigned int>(depth), access, presentation);
}

void Workspace::CycleCameraView(StationAccess access, const InputPresentation& presentation)
{
	const auto stations = access.Snapshot();
	std::optional<Position3d> hoveredStation;
	std::shared_ptr<Station> hoveredIdentity;
	const auto hoverPath = _selector->CurrentHover();
	if (!hoverPath.empty() && (hoverPath.front() < stations.size()))
	{
		hoveredStation = stations[hoverPath.front()]->ModelPosition();
		hoveredIdentity = stations[hoverPath.front()];
	}
	const auto firstStation = stations.empty()
		? std::optional<Position3d>{}
		: std::optional<Position3d>{ stations.front()->ModelPosition() };
	ApplyCameraSelectDepthChange(_camera.CycleView(StationCentre(stations), hoveredStation,
		firstStation, hoveredIdentity, stations.empty() ? nullptr : stations.front(),
		base::DEPTH_STATION == _viewMode), access, presentation);
}

void Workspace::UpdateSelectDepth(unsigned int depth, StationAccess access, const InputPresentation& presentation)
{
	if (depth > (unsigned int)DEPTH_LOOP)
		depth = (unsigned int)DEPTH_LOOP;

	auto selectDepth = (SelectDepth)depth;
	_selector->SetSelectDepth(selectDepth);

	for (auto& station : access.Snapshot())
		station->SetSelectDepth(selectDepth);

	UpdateSelection(ACTIONRESULT_DEFAULT, access, presentation);
}

void Workspace::SetRelativePointerHost(std::function<bool(int, utils::Position2d)> begin, std::function<void(int)> end, std::function<float()> logicalPixelScale, StationAccess access, const InputPresentation& presentation)
{
	ReleaseQuantisationInput(access, presentation);
	(*_editor).CancelInput();
	_beginRelativePointer = std::move(begin);
	_endRelativePointer = std::move(end);
	_logicalPixelScale = std::move(logicalPixelScale);
}

QuantisationInteractionContext Workspace::_InteractionContext(StationAccess access, const InputPresentation& presentation) const
{
	std::scoped_lock lock(access.Mutex);
	return InteractionContextLocked(access, presentation);
}

void Workspace::AttachStationLabel(const std::shared_ptr<Station>& station,
	std::function<std::string(const std::string&, std::uint64_t)> submit)
{
	if (auto label = station->GetGuiRack()->StationLabel())
	{
		label->Focus = [this, weakLabel = std::weak_ptr(label)](const auto& editor) {
			if (editor) _focusManager.RequestFocus(editor);
			else if (auto owner = weakLabel.lock(); owner && owner->OwnsFocus(_focusManager.CurrentFocus())) _focusManager.ClearFocus();
		};
		label->Submit = std::move(submit);
	}
}

void Workspace::OpenTakeDitch(std::shared_ptr<Station> station, std::shared_ptr<LoopTake> take, std::uint64_t revision,
	const std::shared_ptr<base::ActionReceiver>& receiver)
{
	_takeDitchTarget = { std::move(station), std::move(take), revision };
	if (_takeDitchDialog) _takeDitchDialog->ReleaseResources();
	_takeDitchDialog = std::make_shared<GuiPopup>();
	_takeDitchDialog->SetTitle("Ditch this take?");
	_takeDitchDialog->SetBodyLines({"The whole LoopTake and all its loops will be removed.", "Active recording will be cancelled.", "This cannot be undone."});
	_takeDitchDialog->ConfigureButtons({{{"Cancel", GuiStationControls::CancelTakeDitchControlIndex}, {"Ditch take", GuiStationControls::ConfirmTakeDitchControlIndex}}});
	_takeDitchDialog->SetButtonReceiver(receiver);
	_takeDitchDialog->FitToViewport(_size);
	_popupManager.Open(_takeDitchDialog);
}

void Workspace::OpenStationRemoval(const std::string& name, const std::shared_ptr<base::ActionReceiver>& receiver)
{
	_stationDeleteDialog = std::make_shared<GuiPopup>();
	_stationDeleteDialog->SetTitle("Delete " + name + "?");
	_stationDeleteDialog->SetBodyLines({"All takes and station configuration will be removed.", "Active recordings will be cancelled.", "This cannot be undone."});
	_stationDeleteDialog->ConfigureButtons({{{"Cancel", GuiStationControls::CancelRemovalControlIndex}, {"Delete station", GuiStationControls::DeleteRemovalControlIndex}}});
	_stationDeleteDialog->SetButtonReceiver(receiver);
	_stationDeleteDialog->FitToViewport(_size);
	_popupManager.Open(_stationDeleteDialog);
}

void Workspace::CompleteMembershipPresentationLocked(const std::vector<std::shared_ptr<Station>>& stations,
	const std::shared_ptr<Station>& removed, const std::shared_ptr<Station>& added, std::size_t affectedBindings)
{
	_selector->ClearBackgroundClick();
	if (removed)
	{
		_camera.ForgetStation(removed);
		_popupManager.CloseOwnedBy(removed);
		_undoHistory.Clear(); // Retained undo cannot resurrect outgoing hierarchy.
	}
	for (size_t i = 0u; i < stations.size(); ++i)
	{
		const auto& station = stations[i];
		const float startX = station == added
			? (-50.0f + static_cast<float>(i) * 600.0f + (i == 0u ? -600.0f : 600.0f))
			: station->PresentationModelPosition().X;
		station->SetPosition({20 + static_cast<int>(i) * 600, 20});
		station->SetModelPosition({-50.0f + static_cast<float>(i) * 600.0f, -20.0f, 0.0f});
		station->BeginPresentationMove(startX);
		_camera.RegisterStation(i, station, station->LoopTakeRevision());
	}
	_pointer.ReleaseCapture(); _hoverElement3d.reset();
	_hoverPath3d.clear(); _pointer.ClearHoverPath();
	_selector->ClearPointerState();
	_selector->UpdateCurrentHover({}, Action::MODIFIER_NONE, false, Tweakable::TWEAKSTATE_NONE);
	_interaction->CancelInteraction(true);
	_stationControls.Cancel();
	if (added) _camera.RevealPosition(added->ModelPosition(), _size, StationCentre(stations), 80);
	_quantisationFeedback = removed ? "Station deleted; " + std::to_string(affectedBindings) + " binding(s) unbound." : "Station added.";
	_pointer.InvalidateHover();
}

void Workspace::AdvanceCamera(Time now, float deltaSeconds, const Camera::Presentation& presentation,
	const CameraRecordingObservation& recordings, bool controlsAvailable, StationAccess access, const InputPresentation& input)
{
	// Release scene/membership locks before policy and physics. These are copied
	// presentation values, and focus identities must still belong to this scene.
	_camera.AdvancePresentation(now, deltaSeconds, presentation, recordings,
		controlsAvailable && _size.Width > 0u && _size.Height > 0u && !(*_editor).IsEngaged()
			&& !_popupManager.IsOpen(), _size);
	(*_editor).Tick(deltaSeconds);
	if (!(*_editor).IsEngaged())
		ApplyCameraSelectDepthChange(_camera.PendingSelectDepthChange(), access, input);
	if (_isSceneTouching && !_camera.IsBackgroundDragging())
		EndBackgroundDrag(access, input);
}

bool Workspace::PrepareKeyboard(KeyAction& action, StationAccess access)
{
	const bool keyDown = action.KeyActionType == KeyAction::KEY_DOWN;
	const bool freshLearnKey = action.KeyChar < _learnKeysDown.size() && keyDown && !_learnKeysDown[action.KeyChar] &&
		!_learnKeysDown[VK_LWIN] && !_learnKeysDown[VK_RWIN];
	if (action.KeyChar < _learnKeysDown.size()) _learnKeysDown[action.KeyChar] = keyDown;
	action.SetActionTime(Timer::GetTime());
	if ((action.KeyChar == VK_MENU || action.KeyChar == VK_LMENU || action.KeyChar == VK_RMENU) && _hudPanel)
	{
		std::scoped_lock lock(access.Mutex);
		_hudPanel->SetAltLearnHeld(keyDown && !_popupManager.IsOpen() && !_focusManager.IsEditingText());
	}
	return freshLearnKey;
}

void Workspace::ResetKeyboardObservation()
{
	const bool leftWindowsHeld = _learnKeysDown[VK_LWIN], rightWindowsHeld = _learnKeysDown[VK_RWIN];
	_learnKeysDown.fill(false);
	// Windows modifiers have no Action flag. Require their observed release,
	// including after focus loss, before admitting a plain learned key.
	_learnKeysDown[VK_LWIN] = leftWindowsHeld; _learnKeysDown[VK_RWIN] = rightWindowsHeld;
}

void Workspace::OnFocusLostLocked(StationAccess access, const InputPresentation& presentation)
{
	if (auto popup = _popupManager.Top()) popup->ClearPointerState();
	_stationControls.Cancel();
	ReleaseQuantisationInputLocked(access, presentation);
	if (_hudPanel) _hudPanel->ClearPointerState();
	_pointer.ReleaseCapture();
	_pointer.ClearConsumedRelease();
	(*_editor).CancelInput();
	ApplyHoverPath2d({});
	InvalidateHover2d();
}

void Workspace::InitReceivers(const std::shared_ptr<base::ActionReceiver>& receiver)
{
	_selector->SetReceiver(receiver);
	_mainPanel->SetCommandOwner(receiver);
	_selectionPanel->SetCommandOwner(receiver);

}

void Workspace::ClearReceivers()
{
	_selector->SetReceiver({});
	if (_stationDeleteDialog) _stationDeleteDialog->SetButtonReceiver({});
	if (_takeDitchDialog) _takeDitchDialog->SetButtonReceiver({});
	_takeDitchTarget = {};
}

void Workspace::SetViewMode(unsigned int mode, StationAccess access, const InputPresentation& presentation)
{
	if (_camera.SelectDepthChanged(base::DEPTH_STATION == mode))
	{
		mode = base::DEPTH_LOOPTAKE;
		_settingsControls->Mode()->SetCurrentValue(mode, true);
	}
	_viewMode = static_cast<base::SelectDepth>(mode);
	UpdateSelectDepth(mode, access, presentation);
}

void Workspace::ApplyControlPresentation(SessionControl::Presentation presentation, const std::shared_ptr<base::ActionReceiver>& receiver)
{
	if (presentation.SceneId != _sceneIdentity) return;
	if (presentation.PagesChanged && presentation.Midi) _mainPanel->FinalizePageSettings(SettingsPage::Audio);
	if (presentation.AudioAccepted) _settingsControls->AcceptAudioDraft(std::move(presentation.AudioAccepted->Value), presentation.AudioAccepted->Revision);
	if (presentation.MidiAccepted) _settingsControls->AcceptMidiDraft(std::move(presentation.MidiAccepted->Value), presentation.MidiAccepted->Revision);
	_settingsControls->SetSessionEnabled(presentation.SessionEnabled, presentation.DefaultEnabled);
	_settingsControls->SetDeviceStatus(presentation.DeviceStatus);
	_settingsControls->SetActiveAudioStatus(presentation.ActiveAudioStatus);
	_settingsControls->SetFileStatus(presentation.FileStatus);
	if (presentation.PagesChanged && presentation.Midi)
	{
		const auto pages = _settingsControls->BuildDevicePages(presentation.Inventory, _settingsControls->AudioDraft(),
			presentation.AcceptedBufferSizes, presentation.Midi->Connection, _settingsControls->MidiDraft(), _popupManager, receiver);
		_mainPanel->ReplacePageSettings(SettingsPage::Audio, pages.Audio);
		_mainPanel->ReplacePageSettings(SettingsPage::Midi, pages.Midi);
	}
	for (auto& notice : presentation.Notices) _runtimeDialogs.Notify(std::move(notice.Title), std::move(notice.Message));
}
