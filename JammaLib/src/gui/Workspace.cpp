#include "Workspace.h"
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

Workspace::Workspace(utils::Size2d size) : _size(size),
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
	_viewProj = _compositor.BeginWorld(glCtx, _camera, _size, _StationCentre(stations), pass);
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
	auto projection = _camera.Projection(ar, _StationCentre(stations));
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
	_viewProj = _camera.Projection(aspect, _StationCentre(stations)) * _camera.ViewMatrix();
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

Position3d Workspace::_StationCentre(const std::vector<std::shared_ptr<Station>>& stations)
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
