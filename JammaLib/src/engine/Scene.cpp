#include "../../../console/DiagnosticThread.h"
// Scene wires job/UI concerns and off-callback presentation; it does not reconstruct
// NINJAM timing authority or mutate Timer/loop timing at the audio boundary.
#include "Scene.h"
#include "../audio/PlaybackDelay.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "glm/ext.hpp"
#include "../utils/PathUtils.h"
#include "../utils/MathUtils.h"
#include "../midi/MidiTimestampMapper.h"
#include "../io/IoSessionExporter.h"
#include "../io/IoSessionLoader.h"
#include "../vst/Vst3Plugin.h"

using namespace engine;
using namespace base;
using namespace actions;
using namespace audio;
using namespace gui;
using namespace io;
using namespace midi;
using namespace graphics;
using namespace resources;
using namespace utils;
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
	_viewProj(glm::mat4()),
	_selector(nullptr),
	_globalMidiQuantState(io::JamFile::GlobalMidiQuantState::Off),
	_transportOffsetLoopFrac(0.0),
	_mainPanel(nullptr),
	_quantisation(),
	_loggingConfig{},
	_stations(),
	_touchDownElement(std::weak_ptr<GuiElement>()),
	_hoverElement3d(std::weak_ptr<GuiElement>()),
	_hoverPath3d(),
	_hoverPath2d(),
	_hover2dDirty(true),
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
	_vstRackControls(std::make_unique<gui::VstRackControls>(gui::VstRackControls::Host{
		_sceneMutex, _stations, *_windowSubsystem,
		[this]() { return !_isSceneQuitting.load(std::memory_order_acquire) &&
			!_sessionReplacing.load(std::memory_order_acquire) && !_controlWriterExclusion.owns_lock() &&
			_controlOperation == ControlOperation::None && _rigCoordinator.EditsEnabled(); },
		[this]() { CommitChanges(); }, {},
		[this](std::string title, std::string message, std::string accept) {
			return _runtimeDialogs.Confirm(std::move(title),std::move(message),std::move(accept)); },
		[this](const std::wstring& error) { ReportRuntimeNotice("VST",utils::EncodeUtf8(error)); }})),
	_networkService(std::make_unique<ninjam::NinjamNetworkService>()),
	_loopEditor(midi::LoopGridEditor::Host{
		_camera, _undoHistory, _sceneMutex, _stations,
		[this]() { return _hoverElement3d.lock(); },
		[this](float aspect) { return _camera.Projection(aspect, _StationCentre(_stations)) * _camera.ViewMatrix(); },
		[this]() { _OnLoopGridEditorOpened(); },
		[this](int button, utils::Position2d anchor) { return _beginRelativePointer && _beginRelativePointer(button, anchor); },
		[this](int button) { if (_endRelativePointer) _endRelativePointer(button); },
		[this]() { const auto clock = _quantisation.Clock(); return clock ? clock->SeedSourceLength() : 0ul; },
		[this]() { return static_cast<base::SelectDepth>(_selector ?
			_selector->CurrentSelectDepth() : base::DEPTH_LOOP); },
		[this]() { return _logicalPixelScale ? _logicalPixelScale() : 1.0f; } },
		params.Size)
{
	_quantisation.SetClock(std::make_shared<Timer>());
	_quantisation.SetLocalMasterTimingPublisher([this](const QuantisationTiming& timing,
		Timer::QuantisationType type, const LoopTake* take, const Loop* loop)
	{
		return _audioEngine->PublishLocalMasterTiming(timing.SeedSamps,
			timing.MasterLoopSamps, type, take, loop);
	});
	_quantisation.SetSeedUsesPowers(_userConfig.Loop.SeedUsesPowers);

	GuiHudParams hudParams;
	hudParams.Size = params.Size;
	hudParams.MinSize = params.Size;
	hudParams.PopupManager = &_popupManager;
	hudParams.FocusEditor = [this](const auto& editor, bool acquire)
	{
		if (acquire) _focusManager.RequestFocus(editor);
		else if (_focusManager.CurrentFocus() == editor) _focusManager.ClearFocus();
	};
	hudParams.RoutingEditAvailabilityState = [this]() { return _RoutingEditAvailability(); };
	hudParams.AcceptTriggerInput = [this](std::uint64_t revision)
	{
		return _inputSubsystem && _inputSubsystem->TryAcceptUiRigTriggerInput(revision);
	};
	hudParams.SubmitRigEdit = [this](const io::RigFile& candidate)
	{
		// HUD dispatch already owns the scene lock; the public entry locks for external callers.
		return _RequestRigEditLocked(candidate) == RigCoordinator::EditResult::Pending;
	};
	hudParams.SubmitTriggerRename = [this](const io::RigFile& candidate)
	{
		// Text acceptance also runs on blur/keyboard, outside HUD dispatch locking.
		return RequestRigEdit(candidate) == RigCoordinator::EditResult::Pending;
	};
	hudParams.TriggerLearnAdmission = [this]() -> std::string
	{
		if (_inputSubsystem->VstCcLearnActive()) return "Finish VST CC learn before learning a trigger";
		if (_stationManagement.Busy() || _rigSaveInProgress || _rigSaveDialogOpen || _controlOperation != ControlOperation::None ||
			_loopEditor.IsEngaged() || _isSceneQuitting.load(std::memory_order_acquire)) return "Finish the current edit or save before learning";
		return {};
	};
	hudParams.TriggerLearnInput = [this](std::optional<io::TriggerBindingLearn::Request> request)
	{
		if (request) { if (request->Session) _inputSubsystem->RequestTriggerLearn(*request); else _inputSubsystem->EndTriggerLearn(); }
		return _inputSubsystem->TriggerLearnObservation();
	};
	hudParams.TriggerLearnDevices = [this] {
		const auto inputs = _midiState.load(std::memory_order_acquire);
		return io::TriggerBindingLearn::Devices(_userConfig, inputs->Connection, inputs->ConnectedSerialIds);
	};
	_hudPanel = std::make_shared<GuiHud>(hudParams);
	AddChild(_hudPanel);

	GuiSelectorParams selectorParams;
	selectorParams.Position = { 10, 2 };
	selectorParams.Size = params.Size;
	_selector = std::make_unique<SceneSelector>(selectorParams);
	_selector->SetSelectDepth(base::DEPTH_STATION);

	_settingsControls = std::make_unique<SceneSettingsControls>(_inputSubsystem->ForcedChannelOverride(),
		_transportOffsetLoopFrac, _globalMidiQuantState);

	GuiMainPanelParams mainParams;
	mainParams.Size = params.Size;
	mainParams.PopupManager = &_popupManager;
	mainParams.BeforeHide = [this](const auto& subtree) { _OnSettingsHidden(subtree); };
	mainParams.Settings = _settingsControls->SettingsEntries();
	_audioDraft = user.Audio;
	_midiDraft = user.Midi.Devices;
	_mainPanel = std::make_shared<GuiMainPanel>(mainParams);
	AddChild(_mainPanel);
	mainParams.SelectionOnly = true;
	mainParams.Settings = _settingsControls->SelectionEntries();
	_selectionPanel = std::make_shared<GuiMainPanel>(mainParams);
	AddChild(_selectionPanel);
	_selectionPanel->SetPerformanceGroupHandler([this](bool selection, GroupCommand command, std::size_t slot) {
		_ExecuteGroupCommandLocked(selection, command, slot);
	});

	_PublishAudioStations();

	_startupJobDiagnostics = params.StartupDiagnostics;
	_jobRunner = std::thread([this, onStart = params.OnJobThreadStart]() {
		this->_JobLoop(std::move(onStart));
	});
}

void Scene::ConnectNinjam(const std::string& host)
{
	ConnectNinjam(host, _networkService->TempoJoinOptions());
}

void Scene::ConnectNinjam(const std::string& host,
	const ninjam::NinjamTempoJoinOptions& options)
{
	const auto localTiming = _quantisation.CurrentTempoTiming(_CurrentSampleRate());
	++_ninjamJoinGeneration;
	if (options.PushLocalTempoOnJoin)
		++_ninjamTempoRequestId;
	{
		std::scoped_lock lock(_sceneMutex);
		_networkService->SetTempoJoinOptions(options);
		_ApplyNinjamTimingUpdate(_networkService->PrepareTempoSyncOnConnect(localTiming));
	}
	if (_loggingConfig.Event == "verbose")
		_ninjamDiagnosticPresenter.PresentConnect(_ninjamJoinGeneration, _ninjamTempoRequestId,
			options, localTiming, std::cout);
	_lastLoggedTempoRequestState = ninjam::TempoRequestState::Idle;
	_LogNinjamTempoJoinState();
	_networkService->Connect(host);
}

void Scene::DisconnectNinjam()
{
	{
		std::scoped_lock lock(_sceneMutex);
		_ApplyNinjamTimingUpdate(_networkService->ResetTempoSyncOnDisconnect());
	}
	if (_loggingConfig.Event == "verbose")
		std::cout << "[NINJAM][TimingPolicy] changed policy=no-sync reason=disconnect\n";
	_networkService->Disconnect();
}

void Scene::_HandleRemoteTempoSnapshot(const ninjam::NinjamRemoteSnapshot& snapshot,
	const std::optional<engine::QuantisationTiming>& localTiming,
	bool hasLocalContent)
{
	const auto liveTiming = _audioEngine->LatestNinjamTiming();
	NinjamTiming timing = liveTiming.value_or(ToDeviceTiming(snapshot.Timing, true,
		_CurrentSampleRate(), 0u, 0ul, 0u, 0u, 0u));
	if (auto clock = _quantisation.Clock())
		_ApplyNinjamTimingUpdate(_networkService->ObserveTiming(timing,
			localTiming, hasLocalContent, _userConfig, *clock));
}

void Scene::_ApplyRemoteTempoPromptDecision()
{
	const auto decision = _tempoPrompt.TakeDecision(_popupManager);
	if (!decision) return;
	std::scoped_lock lock(_sceneMutex);
	if (auto clock = _quantisation.Clock())
		_ApplyNinjamTimingUpdate(_networkService->ResolveRemoteTempoPromptDecision(*decision,
			_quantisation.CurrentTempoTiming(_CurrentSampleRate()), *clock));
}

void Scene::_ApplyNinjamTimingUpdate(const ninjam::NinjamTimingUpdate& update)
{
	if (_loggingConfig.Event == "verbose"
		&& (update.DesiredTransport || update.PromptForTempoChange || update.TempoRequest))
	{
		const auto clock = _quantisation.Clock();
		_ninjamDiagnosticPresenter.PresentUpdate(update, Timer::GetTime(),
			{ clock ? clock->SeedSourceLength() : 0ul, clock ? clock->SampOffset() : 0u,
				0u, 0u, clock ? clock->SceneSamplePos() : 0u }, clock ? clock->QuantiseSamps() : 0u,
			_networkService->PendingRemoteTempoPrompt().has_value(), std::cout);
	}
	if (update.DesiredTransport.has_value())
	{
		const auto& desired = update.DesiredTransport.value();
		if (update.RemoteGrid.has_value())
			_quantisation.SetRemoteMidiGrid(update.RemoteGrid->Geometry,
				update.RemoteGrid->OriginSamps, _stations);
		else if (desired.Intent == ninjam::NinjamDesiredTimingIntent::NoSync)
			_quantisation.SetRemoteMidiGrid({}, 0, _stations);
		if (_audioEngine)
			_audioEngine->PublishDesiredTiming(desired);
		if (_loggingConfig.Event == "verbose"
			&& desired.Intent == ninjam::NinjamDesiredTimingIntent::NoSync
			&& update.NoSyncReason == ninjam::NinjamNoSyncReason::StayLocal)
		{
			std::cout << "[NINJAM][TimingPolicy] determined policy=no-sync reason=stay-local\n";
		}
	}

	if (update.TempoRequest.has_value())
		_networkService->SendTempoRequest(update.TempoRequest.value());
	_LogNinjamTempoJoinState();
}

void Scene::_LogNinjamTempoJoinState()
{
	if (_loggingConfig.Event != "verbose")
		return;
	const auto state = _networkService->TempoJoinRequestState();
	if (state == _lastLoggedTempoRequestState)
		return;

	_lastLoggedTempoRequestState = state;
	_ninjamDiagnosticPresenter.PresentTempoState(state, _networkService->TimingDiagnostics(),
		_ninjamJoinGeneration, _ninjamTempoRequestId, std::cout);
}

std::optional<std::shared_ptr<Scene>> Scene::FromFile(SceneParams sceneParams,
	io::JamFile jamStruct,
	io::RigFile rigStruct,
	std::wstring dir,
	std::function<bool(const io::RigFile&)> saveRig)
{
	std::cout << "[LOAD] Constructing scene for JAM '" << jamStruct.Name << "' with "
		<< jamStruct.Stations.size() << " station descriptor(s) and " << rigStruct.Triggers.size()
		<< " rig trigger(s)." << std::endl;
	auto scene = std::make_shared<Scene>(sceneParams, rigStruct.User);
	// Candidate restoration excludes its already-started job owner through publication.
	std::unique_lock candidateLifecycle(scene->_controlLifecycleMutex);
	scene->_saveHardwarePreferences = std::move(saveRig);

	auto loaded = IoSessionLoader::Reconstruct(jamStruct, rigStruct, dir);
	if (!scene->_rigCoordinator.BuildInitial(rigStruct,
		loaded.StationDescriptors,
		loaded.Stations,
		0u,
		{},
		loaded.TriggerParameters,
		{}))
	{
		std::cout << "[LOAD] Rig construction failed after station reconstruction." << std::endl;
		return std::nullopt;
	}
	const auto acceptedRig = scene->_rigCoordinator.Accepted();
	if (!acceptedRig)
		return std::nullopt;
	for (const auto& trigger : acceptedRig->Graph.Triggers)
	{
		const auto target = trigger.TargetName.value_or("<unbound>");
		if (trigger.Reason == io::RigFileRouting::Warning::TargetMissing)
			std::cout << "[LOAD] Warning: trigger '" << trigger.TriggerName << "' target '" << target
				<< "' was not found; leaving it unconnected." << std::endl;
		else if (trigger.Reason == io::RigFileRouting::Warning::TargetAmbiguous)
			std::cout << "[LOAD] Warning: trigger '" << trigger.TriggerName << "' target '" << target
				<< "' is ambiguous; leaving it unconnected." << std::endl;
	}
	if (loaded.Stations.empty())
	{
		std::cout << "Load: no constructible stations" << std::endl;
		return std::nullopt;
	}
	IoSessionLoader::RestoreTriggerHistories(jamStruct.TriggerHistories, *acceptedRig, loaded.Stations);
	for (auto& station : loaded.Stations)
		scene->_AddStation(std::move(station), false);
	scene->_PublishAudioStations();
	scene->_performanceGroups.Groups().Mute.Restore(jamStruct.MuteGroups,
		jamStruct.MuteActiveGroup ? std::optional<std::size_t>(*jamStruct.MuteActiveGroup + 1u) : std::nullopt);
	scene->_performanceGroups.Groups().Selection.Restore(jamStruct.SelectionGroups,
		jamStruct.SelectionActiveGroup ? std::optional<std::size_t>(*jamStruct.SelectionActiveGroup + 1u) : std::nullopt);
	scene->_audioEngine->PublishPendingRigSnapshot(acceptedRig);
	scene->_inputSubsystem->PublishRigInputDispatch(acceptedRig);

	if (scene->_hudPanel)
		scene->_hudPanel->SetRoutingConfig(loaded.HudAudioInputCount, std::move(loaded.HudMidiInputs), *acceptedRig);
	scene->_inputSubsystem->OpenRigTriggerInput(acceptedRig->Revision);

	if (!jamStruct.TransportInitialised)
	{
		// No local geometry was saved. The first completed recording seeds the
		// clock from its physical length under the active user timing policy.
		scene->_quantisation.Clear(false);
	}
	else
	{
		scene->_SetQuantisation(jamStruct.QuantiseSamps, jamStruct.Quantisation);
		if (jamStruct.Version == io::JamFile::VERSION_V)
		{
			auto clock = scene->_quantisation.Clock();
			if (!clock || !loaded.SavedMaster)
			{
				std::cout << "Load: invalid local transport state" << std::endl;
				return std::nullopt;
			}
			clock->SetSeedSourceLength(loaded.SavedMaster->LengthSamps);
			if (!clock->InitialiseAbsoluteSamplePos(loaded.SavedMaster->AbsoluteSamplePos))
			{
				std::cout << "Load: invalid local transport state" << std::endl;
				return std::nullopt;
			}
		}
	}
	scene->_quantisation.SetGlobalPhaseOffsetSamps(jamStruct.GlobalPhaseOffsetSamps, scene->_stations);
	scene->_SetGlobalMidiQuantState(jamStruct.GlobalMidiQuantStateValue, true);
	scene->_SetTransportOffsetLoopFrac(jamStruct.TransportOffsetLoopFrac);
	// Saved sessions always start locally.  NINJAM config/anchors are live
	// connection state and are intentionally never restored from a .jam.
	scene->InitReceivers();

	return scene;
}

void Scene::Draw(DrawContext& ctx)
{
	_loopEditor.UpdateUi(_viewProj);
	std::scoped_lock lock(_sceneMutex);
	const bool loopEditorEngaged = _loopEditor.IsEngaged();
	_mainPanel->SetRigSaveEnabled(_rigCoordinator.EditsEnabled() && !_stationManagement.Busy() && !_rigSaveInProgress && !_rigSaveDialogOpen);
	if (_stationManagement.Busy()) _quantisationFeedback = "Applying station change...";
	_UpdateRackVisibilityLocked();
	if (_hudPanel)
	{
		_hudPanel->SetLoopEditorMode(loopEditorEngaged);
		_hudPanel->SetQuantisationFeedback(_quantisationFeedback);
		_hudPanel->SetEditModeAlpha(_quantisationInteraction.PanelAlpha());
	}

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	_compositor.BeginOverlay(glCtx);

	_settingsControls->RefreshChannelPresentation(_inputSubsystem->ForcedChannelOverride());

	if (_hudPanel)
	{
		const auto streamParams = _audioEngine->GetStreamParams();
		const auto numSamps = std::max(1u, streamParams.BufSize);
		for (auto channel = 0u; channel < streamParams.NumInputChannels; ++channel)
			_hudPanel->SetAudioInputPeak(channel, _audioEngine->GetAdcPeak(channel), numSamps);

		unsigned int midiInput = 0u;
		for (const auto& deviceName : _midiState.load(std::memory_order_acquire)->ConnectedIds)
		{
			_hudPanel->SetMidiInputPeak(midiInput++,
				_inputSubsystem->ConsumeMidiInputPeak(deviceName),
				numSamps);
		}
	}

	if (!loopEditorEngaged)
	{
		auto opacity = ctx.WithOpacity(1.0f - _quantisationInteraction.PanelAlpha());
		for (auto& station : _stations)
			station->Draw(ctx);
	}

	if (!_loopEditor.IsEngaged())
	{
		for (const auto& station : _stations)
			if (auto label = station->GetGuiRack()->StationLabel()) label->DrawOverlay(ctx);
		_stationControls.Draw(ctx);
	}
	_selector->Draw(ctx);
	if (!loopEditorEngaged)
		_ctrlHandleOverlay.Draw(ctx);


	{
		auto opacity = ctx.WithOpacity(1.0f - _quantisationInteraction.PanelAlpha());
		for (auto& child : _guiChildren)
			if (child && child != _mainPanel && child != _selectionPanel && child != _hudPanel)
				child->Draw(ctx);
	}
	if (_hudPanel) _hudPanel->Draw(ctx);
	const auto tempo = _quantisation.CurrentTempoStatus(_CurrentSampleRate());
	_compositor.DrawStatus(ctx, _sizeParams.Size, tempo
		? std::optional<Compositor::Tempo>{ { tempo->Bpm, tempo->Bpi } } : std::nullopt);
	if (!_popupManager.IsOpen()) _loopEditor.Draw(ctx);
	{
		auto opacity = ctx.WithOpacity(1.0f - _quantisationInteraction.PanelAlpha());
		_mainPanel->Draw(ctx);
		_selectionPanel->Draw(ctx);
	}
	_popupManager.Draw(ctx);

	_compositor.EndOverlay(glCtx);
}

void Scene::DrawBackground(DrawContext& ctx)
{
	std::scoped_lock lock(_sceneMutex);
	_compositor.DrawBackground(dynamic_cast<GlDrawContext&>(ctx), _camera, _sizeParams.Size);
}

void Scene::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	std::scoped_lock lock(_sceneMutex);

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	_viewProj = _compositor.BeginWorld(glCtx, _camera, _sizeParams.Size, _StationCentre(_stations), pass);
	_UpdateHudStationAnchors();
	if (PASS_PICKER == pass && _loopEditor.IsEngaged())
	{
		_compositor.EndProjection(glCtx);
		return;
	}

	if (PASS_SCENE == pass)
	{
		const auto now = Timer::GetTime();
		_ApplyQuantisationOverlayAlpha(_QuantisationOverlayAlpha(now));
		_quantisationInteraction.Tick(now);
	}

	const bool probeBound = _compositor.SetWorldPresentation(glCtx,
		{ PASS_SCENE == pass && _HasSelection(), _loopEditor.SurroundingDim(),
			PASS_SCENE == pass && _loopEditor.IsEngaged(),
			PASS_SCENE == pass && _loopEditor.IsEngaged() ? _loopEditor.ProbeEyeLocal() : glm::vec3{} });
	_loopEditor.ApplyToModels();
	if (_selectionPanel && (!_selectionPanel->IsVisible() || !_selectionPanel->IsEnabled()
		|| _popupManager.IsOpen() || _loopEditor.IsEngaged() || _EditControlsSuppressed()))
		_selectionPanel->ClearPerformanceGroupPointerState();
	const auto hoveredGroup = PASS_SCENE == pass && _selectionPanel && !_popupManager.IsOpen()
		&& !_loopEditor.IsEngaged() && !_EditControlsSuppressed()
		? _selectionPanel->HoveredPerformanceGroup() : std::nullopt;
	const auto& groups = _performanceGroups.Groups();
	const auto mutePreview = hoveredGroup && !hoveredGroup->first ? groups.Mute.Get(hoveredGroup->second) : nullptr;
	const auto selectionPreview = hoveredGroup && hoveredGroup->first ? groups.Selection.Get(hoveredGroup->second) : nullptr;
	const bool backgroundPreview = PASS_SCENE == pass && _BackgroundPreviewActive();
	std::vector<GroupTargetId> livePreviewTargets;
	if (mutePreview || selectionPreview)
		for (const auto& station : _stations)
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
		for (const auto& station : _stations)
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
	const bool selectionTint = backgroundPreview ? _BackgroundSelectionPreviewActive() : selectionPreview != nullptr;
	glCtx.SetGroupPreview(backgroundPreview || mutePreview || selectionPreview,
		selectionTint ? glm::vec3(1.0f, 0.875f, 0.208f) : glm::vec3(0.259f, 0.588f, 1.0f), std::move(previewMembers), !backgroundPreview);
	for (auto& station : _stations)
	{
		station->Draw3d(ctx, 1, pass);
	}
	_compositor.EndWorld(glCtx, probeBound);
}

bool Scene::IsCameraTriggerKey(unsigned int key) const
{
	const auto rig = _rigCoordinator.Accepted();
	if (!rig) return false;
	for (const auto& trigger : rig->Rig.Triggers)
		for (const auto* bindings : { &trigger.ControlBindings.Activate, &trigger.ControlBindings.Ditch })
			for (const auto& binding : *bindings)
				if (binding.Source == io::RigFile::BindingSource::Keyboard &&
					(key == binding.Press.Code || key == binding.Release.Code)) return true;
	return false;
}

void Scene::OnCameraTriggerActivity()
{
	_camera.TriggerActivity(Timer::GetTime());
}

bool Scene::PreservesCameraIdleForKey(unsigned int key) const noexcept
{
	// Modifiers alone do not navigate; the eventual modified action owns activity.
	if (key == 16u || key == 17u || key == 18u) return true;
	if (_popupManager.IsOpen() || _focusManager.IsEditingText() || _loopEditor.IsEditingText()) return false;
	return key == 32u || key == 192u || key == 96u
		|| key == 8u || (key >= 112u && key <= 120u);
}

void Scene::OnHumanActivity()
{
	_camera.HumanActivity(Timer::GetTime());
}

void Scene::SetCameraInteractionEligibility(bool windowEligible, bool heldInteraction)
{
	_camera.SetInteractionEligibility(windowEligible, heldInteraction,
		*_cameraRecordings->Snapshot(), Timer::GetTime());
}

void Scene::UpdateCamera()
{
	if (!_isSceneQuitting.load(std::memory_order_acquire))
		if (const auto rig = _rigCoordinator.Accepted(); rig && _inputSubsystem->TryAcceptUiRigTriggerInput(rig->Revision))
		{
			base::Action release;
			release.SetActionTime(Timer::GetTime());
			_contextualKeyboard.Flush(*rig, release);
		}
	// Keep camera state on the window thread; audio never reads or advances it.
	const auto now = Timer::GetTime();
	const auto frameTime = _camera.BeginPresentation(now);
	const auto deltaSeconds = frameTime.DeltaSeconds;
	const bool diagnosticDue = frameTime.DiagnosticDue;
	Camera::Presentation presentation;
	std::shared_ptr<const CameraRecordingObservation> recordings;
	size_t visibleContentTakeCount = 0u;
	size_t allContentTakeCount = 0u;
	bool controlsAvailable = false;
	{
		std::scoped_lock lock(_sceneMutex);
		for (size_t index = 0u; index < _stations.size(); ++index)
		{
			const auto& station = _stations[index];
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
		_camera.CompleteStationObservation(_stations.size(), _stations.empty()
			? std::optional<Position3d>{}
			: std::optional<Position3d>{ _stations.front()->ModelPosition() });
		controlsAvailable = !_rigSaveDialogOpen && !_rigSaveInProgress && !_stationManagement.Busy()
			&& _controlOperation == ControlOperation::None && !_controlWriterExclusion.owns_lock();
		recordings = _cameraRecordings->Snapshot();
	}

	// Release scene/membership locks before policy and physics. These are copied
	// presentation values, and focus identities must still belong to this scene.
	_camera.AdvancePresentation(now, deltaSeconds, presentation, *recordings,
		controlsAvailable && Width() > 0u && Height() > 0u && !_loopEditor.IsEngaged()
			&& !_popupManager.IsOpen(), _sizeParams.Size);
	_loopEditor.Tick(deltaSeconds);
	if (!_loopEditor.IsEngaged())
		_ApplyCameraSelectDepthChange(_camera.PendingSelectDepthChange());
	if (_isSceneTouching && !_camera.IsBackgroundDragging())
		_EndBackgroundDrag();
}

bool Scene::AdvanceUiAnimations()
{
	// Native picker/editor calls can pump Windows messages; never hold the
	// scene lock while processing a captured rack gesture.
	_vstRackControls->Pump();
	_runtimeDialogs.Pump(_popupManager,_sizeParams.Size,_runtimeRendererReady);
	if (_quantisationInputResetRequested.exchange(false, std::memory_order_acq_rel))
		_ReleaseQuantisationInput();
	// Window calls once per UI frame before deferred hover and drawing.
	// This clock is independent of the transport/audio callback.
	const auto now = Timer::GetTime();
	const float elapsed = _lastPanelAnimationTime
		? static_cast<float>(Timer::GetElapsedSeconds(*_lastPanelAnimationTime, now)) : 0.0f;
	_lastPanelAnimationTime = now;
	std::scoped_lock lock(_sceneMutex);
	const bool mainChanged = _mainPanel && _mainPanel->AdvanceAnimation(elapsed);
	const bool selectionChanged = _selectionPanel && _selectionPanel->AdvanceAnimation(elapsed);
	if (mainChanged || selectionChanged) _InvalidateHover2d();
	bool stationsMoved = false;
	for (const auto& station : _stations) stationsMoved |= station->AdvancePresentationMove(elapsed);
	const auto previousViewProjection = _viewProj;
	_UpdateViewProjectionLocked();
	const bool cameraMoved = previousViewProjection != _viewProj;
	_UpdateHudStationAnchors();
	const auto depth = _selector->CurrentSelectDepth();
	const bool stationDitchVisible = depth == base::DEPTH_STATION;
	const bool takeDitchVisible = depth == base::DEPTH_LOOPTAKE || depth == base::DEPTH_LOOP;
	if (_hudPanel && (_popupManager.IsOpen() || _loopEditor.IsEngaged() || _isSceneQuitting.load(std::memory_order_acquire)))
		_hudPanel->CancelTriggerRenames();
	for (const auto& station : _stations)
		if (auto label = station->GetGuiRack()->StationLabel())
		{
			if (_popupManager.IsOpen() || _loopEditor.IsEngaged() || _isSceneQuitting.load(std::memory_order_acquire)) label->Cancel();
			const auto accepted = _rigCoordinator.Accepted();
			label->Update(station->Name(), accepted ? accepted->Revision : 0u, _ctrlHeld,
				!station->IsRemote() && !_popupManager.IsOpen() && !_loopEditor.IsEngaged() && !_stationManagement.Busy() && !_rigSaveInProgress,
				!_rigCoordinator.EditsEnabled());
		}
	_stationControls.Update(_stations, _viewProj, _sizeParams.Size, _ctrlHeld, stationDitchVisible, takeDitchVisible,
		_StationManagementAvailableLocked());
	if (stationsMoved || cameraMoved)
	{
		_InvalidateHover2d();
	}
	_UpdateRackVisibilityLocked();
	_vstRackControls->UpdateLocked(static_cast<base::SelectDepth>(depth));
	for (const auto& station : _stations)
		if (const auto rack = station->GetGuiRack()->VstRack())
		{
			rack->SetViewport(_sizeParams.Size);
			if (_popupManager.IsOpen() || _loopEditor.IsEngaged() || _focusManager.IsEditingText() ||
				_isSceneQuitting.load(std::memory_order_acquire) || _rigSaveInProgress)
			{ rack->ClearPointerState(); rack->SetRevealHeld(false); }
		}
	_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
	if (_selectionPanel) _selectionPanel->RefreshPerformanceGroups(_performanceGroups.Groups());
	if (_hudPanel)
	{
		if (_selectionPanel) _hudPanel->SetTopPanelLayout(_selectionPanel->TopPanelLayout());
		_hudPanel->SetPerformanceGroupFeedback(_performanceGroups.Error());
		if (_mainPanel) _mainPanel->SetOccupiedTopBounds(
			_selectionPanel ? _selectionPanel->TopPanelLayout().PresentedBounds : Rect2d{}, _hudPanel->OccupiedTopStripBounds());
	}
	return stationsMoved || cameraMoved;
}

void Scene::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	std::scoped_lock lock(_sceneMutex);

	_compositor.InitResources(resourceLib, forceInit);
	_loopEditor.InitResources(resourceLib, forceInit);
	_selector->InitResources(resourceLib, forceInit);
	for (auto& child : _guiChildren)
		if (child)
			child->InitResources(resourceLib, forceInit);
	_tempoPrompt.InitResources(resourceLib, forceInit);
	_ctrlHandleOverlay.InitResources(resourceLib, forceInit);

	for (auto& station : _stations)
		station->InitResources(resourceLib, forceInit);

	_InitSize();

	ResourceUser::_InitResources(resourceLib, forceInit);
}

void Scene::_ReleaseResources()
{
	_runtimeRendererReady = false;
	_runtimeDialogs.ReleaseResources();
	{ std::scoped_lock lock(_sceneMutex); for (const auto& station : _stations) station->FinishPresentationMove(); }
	_compositor.ReleaseResources();
	_loopEditor.ReleaseResources();
	_selector->ReleaseResources();
	for (auto& child : _guiChildren)
		if (child)
			child->ReleaseResources();
	_tempoPrompt.ReleaseResources();
	_ctrlHandleOverlay.ReleaseResources();
	_stationControls.ReleaseResources();
	if (_stationDeleteDialog) _stationDeleteDialog->ReleaseResources();
	if (_takeDitchDialog) _takeDitchDialog->ReleaseResources();
	for (const auto& station : _retiredStations) station->ReleaseResources();

	for (auto& station : SnapshotStations())
		station->ReleaseResources();

	Drawable::_ReleaseResources();
}

void Scene::_OnSettingsHidden(const std::shared_ptr<GuiElement>& subtree)
{
	auto belongs = [&subtree](std::shared_ptr<GuiElement> element)
	{
		for (; element; element = element->Parent())
			if (element == subtree) return true;
		return false;
	};
	if (belongs(_focusManager.CurrentFocus())) _focusManager.ClearFocus();
	if (belongs(_touchDownElement.lock()))
	{
		_touchDownElement.reset();
		_touchDownIsHud = false;
		_touchDownIsSettings = false;
		_consumeSettingsRelease = true;
	}
	_InvalidateHover2d();
}

std::optional<ActionResult> Scene::_RouteSettingsTouch(TouchAction action)
{
	if (action.State == TouchAction::TOUCH_UP && _consumeSettingsRelease)
	{
		_consumeSettingsRelease = false;
		return ActionResult{ true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_touchDownIsSettings)
	{
		auto active = _touchDownElement.lock();
		auto result = active ? active->OnAction(active->GlobalToLocal(action)) : ActionResult::NoAction();
		if (action.State == TouchAction::TOUCH_UP)
		{
			_touchDownElement.reset();
			_touchDownIsSettings = false;
			_consumeSettingsRelease = false;
		}
		result.IsEaten = true;
		return result;
	}
	// A gesture already owned elsewhere keeps its release and move stream.
	if (_touchDownElement.lock() || _camera.IsBackgroundDragging() || _isSceneTouching ||
		_loopEditor.OwnsPointer() || _loopEditor.IsOrbitDragging() || _quantisationInteraction.OwnsPointer())
		return std::nullopt;
	for (const auto& panel : { _selectionPanel, _mainPanel })
	{
		if (!panel || !panel->RouteHitTest(panel->GlobalToLocal(action.Position))) continue;
		auto result = panel->OnAction(panel->GlobalToLocal(action));
		result.IsEaten = true;
		if (action.State == TouchAction::TOUCH_DOWN &&
			!(action.Touch == TouchAction::TOUCH_MOUSE && action.Index == 4))
		{
			auto active = result.ActiveElement.lock();
			if (!active) active = panel;
			_touchDownElement = active;
			_touchDownIsSettings = true;
			_touchDownIsHud = false;
			auto focused = _focusManager.CurrentFocus();
			if (focused && focused != active) focused->FinalizeEdits();
			if (active->WantsFocusOnPress()) _focusManager.RequestFocus(active);
			else _focusManager.ClearFocus();
		}
		return result;
	}
	return std::nullopt;
}

bool Scene::_BackgroundPreviewActive() const
{
	return _backgroundGesture && !_backgroundGesture->Cancelled && !_popupManager.IsOpen()
		&& !_EditControlsSuppressed() && !_loopEditor.IsEngaged()
		&& !_quantisationInputResetRequested.load(std::memory_order_acquire);
}

bool Scene::_BackgroundSelectionPreviewActive() const
{
	return _BackgroundPreviewActive() && _backgroundGesture->Command == BackgroundCommand::SelectAll;
}

void Scene::_SelectAllLocalLocked()
{
	_CollapseRacksLocked();
	HierarchySelection::SelectAllLocal(HierarchySelection::LockedStations(_stations), _selector->CurrentSelectDepth());
}

ActionResult Scene::OnAction(TouchAction action)
{
	if (_popupManager.ConsumeDismissedRelease(action))
	{
		_touchDownElement.reset();
		_touchDownIsHud = _touchDownIsSettings = _consumeSettingsRelease = false;
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (action.State == TouchAction::TOUCH_DOWN) _cameraPointerPreservesIdle = false;
	const auto result = _OnPointerAction(action);
	if (std::dynamic_pointer_cast<gui::GuiPerformanceGroupButton>(_touchDownElement.lock()))
		_cameraPointerPreservesIdle = true;
	if (!_cameraPointerPreservesIdle) OnHumanActivity();
	return result;
}

ActionResult Scene::_OnPointerAction(TouchAction action)
{
	if (_isSceneQuitting.load(std::memory_order_acquire) || _controlWriterExclusion.owns_lock()) return ActionResult::NoAction();
	ActionResult res;
	// Wheel events are encoded as DOWN/index 4 and have no matching UP. They
	// must not become pointer captures or replace keyboard focus.
	const bool pointerPress = action.State == TouchAction::TOUCH_DOWN &&
		!(action.Touch == TouchAction::TOUCH_MOUSE && action.Index == 4);
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	_cursorPos = action.Position;
	_InvalidateHover2d();
	if (_rigSaveDialogOpen) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	if (const auto rack = std::dynamic_pointer_cast<gui::GuiVstRack>(_touchDownElement.lock()))
	{
		if (_popupManager.IsOpen() || _loopEditor.IsEngaged() || _EditControlsSuppressed())
		{ rack->ClearPointerState(); _touchDownElement.reset(); }
		else
		{
			const auto result = rack->OnAction(rack->GlobalToLocal(action));
			if (action.State == TouchAction::TOUCH_UP && action.Index == 0) _touchDownElement.reset();
			return result;
		}
	}
	if (_backgroundGesture && (_popupManager.IsOpen() || _EditControlsSuppressed() || _loopEditor.IsEngaged() ||
		_quantisationInputResetRequested.load(std::memory_order_acquire)))
		_backgroundGesture.reset();
	if (_backgroundGesture)
	{
		if (action.Touch != TouchAction::TOUCH_MOUSE)
			_backgroundGesture->Cancelled = true;
		else if (action.State == TouchAction::TOUCH_UP && action.Index == _backgroundGesture->Button)
		{
			const auto gesture = *_backgroundGesture;
			_backgroundGesture.reset();
			if (!gesture.Cancelled && !gesture.Moved && action.Position == gesture.PressPosition)
			{
				{
					std::scoped_lock lock(_sceneMutex);
					if (gesture.Command == BackgroundCommand::SelectAll) _SelectAllLocalLocked();
					else
					{
						_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
						_performanceGroups.SetAllMuted(gesture.Command == BackgroundCommand::MuteAll, _stations,
							[this](auto application) { _PublishGroupMuteApplication(std::move(application)); });
					}
				}
				if (gesture.Command == BackgroundCommand::SelectAll)
				{
					_InvalidateHover2d();
					_UpdateSelection(ACTIONRESULT_DEFAULT);
				}
			}
		}
		else if (pointerPress)
			_backgroundGesture->Cancelled = true;
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_popupManager.IsOpen())
	{
		_loopEditor.CancelInput();
		auto popupRes = _popupManager.OnAction(action);
		_ApplyRemoteTempoPromptDecision();
		if (_takeDitchDialog && !_popupManager.IsOpen()) _CancelTakeDitch();
		if (_stationDeleteDialog && !_popupManager.IsOpen())
		{ std::scoped_lock lock(_sceneMutex); if (!_stationManagement.Busy()) _stationManagement.Cancel(); }
		if (TouchAction::TOUCH_UP == action.State)
		{
			_touchDownElement.reset();
			_touchDownIsHud = false;
			_touchDownIsSettings = false;
			_consumeSettingsRelease = false;
		}
		return popupRes;
	}
	if (_hudPanel && _hudPanel->RouteRenameTouch(_hudPanel->GlobalToLocal(action)))
	{
		if (action.State == TouchAction::TOUCH_UP && _touchDownIsHud && !_hudPanel->HasLabelPress())
		{ _touchDownElement.reset(); _touchDownIsHud = false; }
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_loopEditor.IsEngaged())
	{
		const auto stations = SnapshotStations();
		for (const auto& station : stations)
			if (auto label = station->GetGuiRack()->StationLabel(); label && label->OwnsPointer())
			{
				if (label->Touch(label->GlobalToLocal(action))) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
			}
	}
	// Existing captures and drafts keep their gesture. Fresh panel/HUD hits
	// precede projected station overlays, which may lie behind these controls.
	if (!_EditControlsSuppressed() && !_quantisationInteraction.OwnsPointer())
		if (auto settingsResult = _RouteSettingsTouch(action)) return *settingsResult;
	if (_touchDownIsHud)
	{
		auto active = _touchDownElement.lock();
		auto result = ActionResult::NoAction();
		{ std::scoped_lock lock(_sceneMutex); if (active) result = active->OnAction(active->GlobalToLocal(action)); }
		if (action.State == TouchAction::TOUCH_UP) { _touchDownElement.reset(); _touchDownIsHud = false; }
		result.IsEaten = true;
		return result;
	}
	if (!_EditControlsSuppressed() && !_touchDownElement.lock() && !_camera.IsBackgroundDragging() && !_isSceneTouching &&
		!_loopEditor.OwnsPointer() && !_loopEditor.IsOrbitDragging() && !_quantisationInteraction.OwnsPointer() && _hudPanel &&
		(_hudPanel->CapturesPointer(_hudPanel->GlobalToLocal(action.Position)) || _hudPanel->HasCableDrag()))
	{
		// Routing callbacks require scene serialization; focus follows after unlock.
		auto result = ActionResult::NoAction();
		{ std::scoped_lock lock(_sceneMutex); result = _hudPanel->OnAction(_hudPanel->GlobalToLocal(action)); }
		if (result.IsEaten)
		{
			if (pointerPress)
			{
				auto active = result.ActiveElement.lock();
				if (!active) active = _hudPanel;
				_touchDownElement = active; _touchDownIsHud = true;
				if (active->WantsFocusOnPress()) _focusManager.RequestFocus(active);
				else _focusManager.ClearFocus();
			}
			return result;
		}
	}
	if (!_loopEditor.IsEngaged())
	{
		const auto stations = SnapshotStations();
		for (const auto& station : stations)
			if (auto label = station->GetGuiRack()->StationLabel(); label && station->GetGuiRack()->IsInView(_sizeParams.Size))
				if (label->Touch(label->GlobalToLocal(action))) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_loopEditor.IsEngaged() && !_quantisationInteraction.OwnsPointer() &&
		_stationControls.Touch(action, [this](auto intent) {
			if (intent.Take) _RequestTakeDitch(std::move(intent));
			else _RequestStationControl(std::move(intent));
		}))
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	if (_quantisationInteraction.OwnsPointer() || (_EditControlsSuppressed() && !_loopEditor.IsEngaged()))
	{
		if (auto overlay = _RouteQuantisationTouch(action))
			return *overlay;
		return ActionResult::NoAction();
	}
	if (!_EditControlsSuppressed() && _loopEditor.IsEngaged() && !_loopEditor.OwnsPointer() && !_loopEditor.IsOrbitDragging() && _hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		auto hudResult = _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
		if (hudResult.IsEaten) return hudResult;
	}
	if (auto editorRes = _loopEditor.OnAction(action))
		return *editorRes;
	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
	{
		// Clear the old hover now so only the capture target appears pressed.
		std::vector<std::weak_ptr<GuiElement>> none;
		_ApplyHoverPath2d(none);
		_hoverPath2d.clear();
	}

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

	if (auto overlayRes = _RouteQuantisationTouch(action);
		overlayRes.has_value())
	{
		return overlayRes.value();
	}

	if (TouchAction::TouchState::TOUCH_UP == action.State)
	{
		auto activeElement = _touchDownElement.lock();

		if (activeElement)
		{
			if (_touchDownIsHud)
			{
				std::scoped_lock lock(_sceneMutex);
				res = activeElement->OnAction(activeElement->GlobalToLocal(action));
			}
			else
				res = activeElement->OnAction(activeElement->GlobalToLocal(action));

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
			_EndBackgroundDrag();

			// Clear selection after an unhandled background drag.
			if (!wasDragged && !_camera.IsBackgroundDragging())
				_UpdateSelection(ACTIONRESULT_CLEARSELECT);
		}

		// Update selector and then react to result (selecting, deselecting, muting, unmuting)
		res = _selector->OnAction(_selector->ParentToLocal(action));

		_UpdateSelection(res.ResultType);

		_touchDownElement.reset();
		_touchDownIsHud = false;

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
			std::scoped_lock lock(_sceneMutex);
			res = child->OnAction(child->ParentToLocal(action));
		}
		else
			res = child->OnAction(child->ParentToLocal(action));
		if (res.IsEaten)
		{
			if (nullptr != res.Undo)
				_undoHistory.Add(res.Undo);

			if (pointerPress && !_touchDownElement.lock())
			{
				_touchDownElement = res.ActiveElement;
				_touchDownIsHud = isHudChild;
			}

			// Focus follows the pressed control when it wants the keyboard.
			if (pointerPress)
			{
				auto active = res.ActiveElement.lock();
				if (active && active->WantsFocusOnPress())
					_focusManager.RequestFocus(active);
				else
					_focusManager.ClearFocus();
			}

			return res;
		}
	}

	for (auto& station : SnapshotStations())
	{
		res = static_cast<std::shared_ptr<base::GuiElement>>(station)->OnAction(station->ParentToLocal(action));

		if (res.IsEaten)
		{
			if (nullptr != res.Undo)
				_undoHistory.Add(res.Undo);

			if (pointerPress && !_touchDownElement.lock())
			{
				_touchDownElement = res.ActiveElement;
				_touchDownIsHud = false;
			}

			return res;
		}
	}

	res = _selector->OnAction(_selector->ParentToLocal(action));

	_UpdateSelection(res.ResultType);

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
			_sizeParams.Size.Width,
			_sizeParams.Size.Height,
			_StationCentre(SnapshotStations()));

	// Pressing empty background clears keyboard focus.
	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
		_focusManager.ClearFocus();

	if (pointerPress && action.Touch == TouchAction::TOUCH_MOUSE && !_camera.IsBackgroundPointerHeld() && !_isSceneTouching)
	{
		// Resolve actual targets: the pass-through HUD spans the viewport but
		// must not claim its empty space as a control.
		_ResolveHoverPath2d(_hoverPath2dScratch); // Includes station racks and controls.
		const bool unmute = action.Index == 1 && action.Modifiers == Action::MODIFIER_NONE;
		const bool mute = action.Index == 1 && action.Modifiers == Action::MODIFIER_ALT;
		const bool select = action.Index == 0 && action.Modifiers == Action::MODIFIER_ALT;
		const auto buttonMask = (unmute || mute || select) ? (1u << action.Index) : 0u;
		// Chords retain their existing camera/selection meaning. An eligible
		// single-button background press owns its release even after cancellation.
		if (_hoverPath2dScratch.empty() && !_HasQuantisationHover() && (unmute || mute || select) &&
			(action.MouseButtonsDown == 0u || action.MouseButtonsDown == buttonMask))
		{
			_camera.CancelBackgroundCoast();
			_backgroundGesture = BackgroundGesture{ action.Index,
				select ? BackgroundCommand::SelectAll : mute ? BackgroundCommand::MuteAll : BackgroundCommand::UnmuteAll, action.Position };
			_cameraPointerPreservesIdle = true;
			return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
		}
	}

	return _BeginBackgroundDrag(action);
}

ActionResult Scene::OnAction(TouchMoveAction action)
{
	const auto result = _OnPointerMove(action);
	if (action.Origin == TouchMoveAction::Provenance::DeliveredPointer
		&& (!_cameraPointerPreservesIdle || action.MouseButtonsDown == 0u)) OnHumanActivity();
	return result;
}

ActionResult Scene::_OnPointerMove(TouchMoveAction action)
{
	if (_isSceneQuitting.load(std::memory_order_acquire) || _controlWriterExclusion.owns_lock()) return ActionResult::NoAction();
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	_cursorPos = action.Position;
	_InvalidateHover2d();
	if (_backgroundGesture)
	{
		const auto buttonMask = 1u << _backgroundGesture->Button;
		if (action.Touch != TouchAction::TOUCH_MOUSE || !(action.MouseButtonsDown & buttonMask) ||
			_popupManager.IsOpen() || _EditControlsSuppressed() || _loopEditor.IsEngaged() ||
			_quantisationInputResetRequested.load(std::memory_order_acquire))
			_backgroundGesture.reset();
		else if (action.MouseButtonsDown != buttonMask)
			_backgroundGesture->Cancelled = true;
		else if (action.Position != _backgroundGesture->PressPosition ||
			(action.IsRelative && action.RelativeDelta != utils::Position2d{}))
			// Moving cancels the click, but the captured background press continues
			// previewing all local items until release.
			_backgroundGesture->Moved = true;
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_popupManager.IsOpen() && _touchDownIsHud)
	{
		auto active = _touchDownElement.lock();
		const bool cancelled = action.Touch == TouchAction::TOUCH_MOUSE && action.MouseButtonsDown == 0u;
		std::scoped_lock lock(_sceneMutex);
		if (active)
		{
			if (cancelled) active->ClearPointerState();
			else active->OnAction(active->GlobalToLocal(action));
		}
		if (cancelled) { _touchDownElement.reset(); _touchDownIsHud = false; }
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_popupManager.IsOpen() && !_EditControlsSuppressed() && !_touchDownElement.lock() &&
		!_camera.IsBackgroundDragging() && !_isSceneTouching && !_loopEditor.OwnsPointer() && !_loopEditor.IsOrbitDragging() &&
		!_quantisationInteraction.OwnsPointer() && _hudPanel && _hudPanel->CapturesPointer(_hudPanel->GlobalToLocal(action.Position)))
	{
		std::scoped_lock lock(_sceneMutex);
		_hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_popupManager.IsOpen() && !_loopEditor.IsEngaged() && !_quantisationInteraction.OwnsPointer())
	{
		for (const auto& station : SnapshotStations())
		{
			if (auto label = station->GetGuiRack()->StationLabel())
				label->Move(label->GlobalToLocal(action));
			if (auto rack = station->GetGuiRack()->VstRack())
				rack->MovePointer(rack->GlobalToLocal(action));
		}
	}
	if (!_popupManager.IsOpen() && !_loopEditor.IsEngaged() && _stationControls.Move(action))
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	if (_quantisationInteraction.OwnsPointer())
	{
		if (action.MouseButtonsDown == 0u)
			_quantisationInteraction.CancelInteraction();
		else if (auto overlay = _RouteQuantisationMove(action))
			return *overlay;
		return ActionResult::NoAction();
	}
	if (_EditControlsSuppressed() && !_loopEditor.IsEngaged())
		return ActionResult::NoAction();
	if (_popupManager.IsOpen())
	{
		_loopEditor.CancelInput();
		return _popupManager.OnAction(action);
	}
	if (_consumeSettingsRelease && action.MouseButtonsDown == 0u)
	{
		_consumeSettingsRelease = false;
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_touchDownIsSettings)
	{
		if (auto active = _touchDownElement.lock())
		{
			if (action.MouseButtonsDown == 0u) active->ClearPointerState();
			else active->OnAction(active->GlobalToLocal(action));
		}
		if (action.MouseButtonsDown == 0u)
		{
			_touchDownElement.reset();
			_touchDownIsSettings = false;
			_consumeSettingsRelease = false;
			_InvalidateHover2d();
		}
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_EditControlsSuppressed() && !_touchDownElement.lock() && !_camera.IsBackgroundDragging() && !_isSceneTouching &&
		!_loopEditor.OwnsPointer() && !_loopEditor.IsOrbitDragging() && !_quantisationInteraction.OwnsPointer())
	{
		for (const auto& panel : { _selectionPanel, _mainPanel })
			if (panel && panel->RouteHitTest(panel->GlobalToLocal(action.Position)))
			{
				panel->OnAction(panel->GlobalToLocal(action));
				return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
			}
	}
	if (!_EditControlsSuppressed() && _loopEditor.IsEngaged() && !_loopEditor.OwnsPointer() && !_loopEditor.IsOrbitDragging() && _hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		auto hudResult = _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
		if (hudResult.IsEaten) return hudResult;
	}
	if (auto editorRes = _loopEditor.OnAction(action))
		return *editorRes;

	if (auto overlayRes = _RouteQuantisationMove(action);
		overlayRes.has_value())
	{
		return overlayRes.value();
	}

	if (_camera.IsBackgroundDragging() && (0u != action.MouseButtonsDown))
		return _UpdateBackgroundDrag(action);

	auto activeElement = _touchDownElement.lock();

	if (activeElement)
	{
		if (action.Touch == TouchAction::TOUCH_MOUSE && action.MouseButtonsDown == 0u)
		{
			// Native capture loss is reported as a zero-button move. Clear the
			// captured widget and owner together, including HUD captures.
			if (_touchDownIsHud)
			{
				std::scoped_lock lock(_sceneMutex);
				activeElement->ClearPointerState();
			}
			else activeElement->ClearPointerState();
			_touchDownElement.reset();
			_touchDownIsHud = false;
			return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
		}
		if (_touchDownIsHud)
		{
			std::scoped_lock lock(_sceneMutex);
			return activeElement->OnAction(activeElement->GlobalToLocal(action));
		}
		return activeElement->OnAction(activeElement->GlobalToLocal(action));
	}

	if (_isSceneTouching)
		return _UpdateBackgroundDrag(action);
	{
		auto selectionMove = _selector->OnAction(_selector->ParentToLocal(action));
		if (selectionMove.IsEaten)
		{
			_UpdateSelection(selectionMove.ResultType);
			return selectionMove;
		}
	}
	if (_hudPanel)
	{
		// Lock the HUD tree while the job thread rebuilds it.
		std::scoped_lock lock(_sceneMutex);
		return _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
	}

	return ActionResult::NoAction();
}

ActionResult Scene::OnAction(KeyAction action)
{
	const bool keyDown = action.KeyActionType == KeyAction::KEY_DOWN;
	const bool freshLearnKey = action.KeyChar < _learnKeysDown.size() && keyDown && !_learnKeysDown[action.KeyChar] &&
		!_learnKeysDown[VK_LWIN] && !_learnKeysDown[VK_RWIN];
	if (action.KeyChar < _learnKeysDown.size()) _learnKeysDown[action.KeyChar] = keyDown;
	action.SetActionTime(Timer::GetTime());
	if ((action.KeyChar == VK_MENU || action.KeyChar == VK_LMENU || action.KeyChar == VK_RMENU) && _hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		_hudPanel->SetAltLearnHeld(keyDown && !_popupManager.IsOpen() && !_focusManager.IsEditingText());
	}
	const bool contextual = _rigSaveDialogOpen || _controlWriterExclusion.owns_lock() ||
		_popupManager.IsOpen() || _focusManager.IsEditingText() || _loopEditor.IsEngaged();
	const auto inputRig = _rigCoordinator.Accepted();
	const bool admissionOpen = !_isSceneQuitting.load(std::memory_order_acquire) && inputRig &&
		_inputSubsystem->TryAcceptUiRigTriggerInput(inputRig->Revision);
	// Complete older physical releases before a fresh press can be admitted.
	// Observation follows this flush, so a current ordinary UP is queued once by
	// normal dispatch below; contextual/suppressed releases use the exact receipt.
	if (admissionOpen) _contextualKeyboard.Flush(*inputRig, action);
	const bool suppressedKey = _contextualKeyboard.Observe(action, contextual, admissionOpen);
	if (admissionOpen && (contextual || suppressedKey))
		_contextualKeyboard.Flush(*inputRig, action);
	if (_backgroundGesture && action.KeyActionType == KeyAction::KEY_DOWN && action.KeyChar == 27u)
	{
		_backgroundGesture->Cancelled = true;
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	// A consumed physical press keeps its release even when focus changes.
	if (_performanceGroups.ConsumeRelease(action)) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	if (_rigSaveDialogOpen || _controlWriterExclusion.owns_lock() || _popupManager.IsOpen() || _focusManager.IsEditingText() || _loopEditor.IsEngaged())
		_performanceGroups.BlockContextualPress(action);
	if (_isSceneQuitting.load(std::memory_order_acquire)) return ActionResult::NoAction();
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	action.SetAudioParams(_audioEngine->GetStreamParams());

	// Physical modifier transitions must survive contextual editor consumption.
	if (17u == action.KeyChar)
	{
		const bool wasCtrlHeld = _ctrlHeld;
		_ctrlHeld = actions::KeyAction::KEY_DOWN == action.KeyActionType;
		// The loop grid keeps its existing Ctrl/Space overlay gestures; only its
		// text editor (and true modal/text capture) suppresses those shortcuts.
		const bool contextualText = _rigSaveDialogOpen || _controlWriterExclusion.owns_lock() ||
			_popupManager.IsOpen() || _focusManager.IsEditingText() || _loopEditor.IsEditingText();
		{
			std::scoped_lock lock(_sceneMutex);
			_quantisationInteraction.OnCtrlModifierChanged(_ctrlHeld && !contextualText, action.GetActionTime(),
				_InteractionContextLocked(), [this](const auto& path) { return _ChildFromPathLocked(path); });
		}
		if (_ctrlHeld && !contextualText && !_loopEditor.IsEngaged())
		{
			_focusManager.ClearFocus();
			if (auto active = _touchDownElement.lock()) active->ClearPointerState();
			_touchDownElement.reset();
			_touchDownIsHud = _touchDownIsSettings = false;
			if (!wasCtrlHeld && HasSelection())
			{
				// Targets were captured above; hide hover when editing the selection.
				_ApplyHoverPath2d({});
				_hoverPath2d.clear();
				{
					std::scoped_lock lock(_sceneMutex);
					_hoverPath3d.clear();
					_hoverElement3d.reset();
					_selector->UpdateCurrentHover({}, Action::MODIFIER_NONE, false,
						Tweakable::TWEAKSTATE_NONE);
					for (const auto& station : _stations)
						station->SetPicking3d(false);
				}
			}
		}
		if (wasCtrlHeld && !_ctrlHeld) { _stationControls.CancelBinPress(); _InvalidateHover2d(); }
	}
	if (32u == action.KeyChar)
	{
		if (actions::KeyAction::KEY_UP == action.KeyActionType)
		{
			_spaceHeld = false;
			_SetQuantisationOverlayHeld(false);
		}
		else if (!_spaceHeld)
		{
			_spaceHeld = true;
			if (!_popupManager.IsOpen() && !_focusManager.IsEditingText() && !_loopEditor.IsEditingText())
			{
				_SetQuantisationOverlayHeld(true);
				_HandleTapTempo(action.GetActionTime());
			}
		}
		// A text consumer still receives Space, but the physical release above
		// always unwinds a previous hold, even after focus changes.
		if (!_popupManager.IsOpen() && !_focusManager.IsEditingText() && !_loopEditor.IsEditingText())
			return ActionResult::NoAction();
	}
	if (contextual && action.KeyActionType == KeyAction::KEY_UP)
	{
		// These shortcut owners admitted their holds before a modal/text context.
		// Cleanup is explicit and never invokes ordinary trigger matching.
		std::scoped_lock lock(_sceneMutex);
		if (action.KeyChar == VK_INSERT) _inputSubsystem->HandleAutomationKey(action, _stations, {}, {});
		if (action.KeyChar == VK_PRIOR || action.KeyChar == VK_NEXT) _inputSubsystem->HandleChannelOverrideKey(action, _stations);
		if ((action.KeyChar == 192u || action.KeyChar == 96u) && _hudPanel) _hudPanel->SetCableRevealHeld(false);
	}
	if (_controlWriterExclusion.owns_lock()) return ActionResult::NoAction();
	if (suppressedKey && !contextual) return ActionResult::NoAction();
	if (27u == action.KeyChar && _quantisationInteraction.OwnsPointer())
	{
		_quantisationInteraction.CancelInteraction();
		return ActionResult::NoAction();
	}

	if (_popupManager.Top() == _takeDitchDialog && _takeDitchDialog &&
		action.KeyChar == 13u && action.KeyActionType == KeyAction::KEY_UP)
	{
		_popupManager.Close();
		_CancelTakeDitch();
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_popupManager.Top() == _stationDeleteDialog && _stationDeleteDialog &&
		action.KeyChar == 13u && action.KeyActionType == KeyAction::KEY_UP)
	{
		_popupManager.Close();
		{ std::scoped_lock lock(_sceneMutex); _stationManagement.Cancel(); }
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_popupManager.IsOpen())
	{
		{ std::scoped_lock lock(_sceneMutex); if (_hudPanel && _hudPanel->TriggerLearnKeyboard(action, freshLearnKey)) _inputSubsystem->ObserveTriggerLearnKey(action.KeyChar); }
		_loopEditor.CancelInput();
		auto popupRes = _popupManager.OnAction(action);
		_ApplyRemoteTempoPromptDecision();
		if (_takeDitchDialog && !_popupManager.IsOpen()) _CancelTakeDitch();
		if (_stationDeleteDialog && !_popupManager.IsOpen())
		{ std::scoped_lock lock(_sceneMutex); if (!_stationManagement.Busy()) _stationManagement.Cancel(); }
		if (_loopEditor.IsEngaged())
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
			std::scoped_lock lock(_sceneMutex);
			_hudPanel->SetCableRevealHeld(actions::KeyAction::KEY_DOWN == action.KeyActionType);
			for (const auto& station : _stations)
				if (auto rack = station->GetGuiRack()->VstRack())
					rack->SetRevealHeld(actions::KeyAction::KEY_DOWN == action.KeyActionType);
		}
		return ActionResult::NoAction();
	}
	if (_hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		for (const auto& station : _stations)
			if (auto rack = station->GetGuiRack()->VstRack())
				if (auto result = rack->OnAction(action); result.IsEaten) return result;
		if (_hudPanel->HasCableDrag())
		{
			auto hudResult = _hudPanel->OnAction(action);
			if (hudResult.IsEaten)
				return hudResult;
		}
	}

	if (auto editorRes = _loopEditor.OnAction(action))
		return *editorRes;

	if (auto overrideRes = _inputSubsystem->HandleChannelOverrideKey(action, SnapshotStations());
		overrideRes.IsEaten)
	{
		_settingsControls->RefreshChannelPresentation(_inputSubsystem->ForcedChannelOverride());
		return overrideRes;
	}

	if (auto editorRes = _loopEditor.TryOpenFromKey(action))
		return *editorRes;
	if (!_popupManager.IsOpen() && !_focusManager.IsEditingText() && !_loopEditor.IsEngaged())
	{
		std::scoped_lock lock(_sceneMutex);
		_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
		if (_performanceGroups.HandleKey(action, _stations, _GroupSelectionDepthLocked(),
			[this](auto application) { _PublishGroupMuteApplication(std::move(application)); }))
		{
			if (_selectionPanel) _selectionPanel->RefreshPerformanceGroups(_performanceGroups.Groups());
			return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
		}
	}

	if ((9u == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_NONE == action.Modifiers))
	{
		_CycleCameraView();
		ActionResult result;
		result.IsEaten = true;
		return result;
	}

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
		const auto depth = static_cast<base::SelectDepth>(_selector->CurrentSelectDepth());
		const auto hovering = _ChildFromPath(_selector->CurrentHover());
		const auto pluginPath = utils::PickFile(L"Choose VST plugin");
		if (pluginPath.empty())
			return ActionResult::NoAction();

		return _windowSubsystem->HandleVstInsert(pluginPath,
			depth,
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
			SnapshotStations());
	}

	// Ctrl+S saves a numbered session; Ctrl+Shift+S chooses its directory.
	if ((83 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers))
	{
		_RequestSave((action.Modifiers & Action::MODIFIER_SHIFT) != 0);
		return ActionResult::NoAction();
	}

	// Insert + Ctrl+Shift+L/W/X/[/] - MIDI automation record, learn, wire, delete, lane cycle.
	{
		auto hovered = _ChildFromPath(_selector->CurrentHover());
		auto hoveredTake = std::dynamic_pointer_cast<LoopTake>(hovered);
		std::scoped_lock lock(_sceneMutex);
		auto automationRes = _inputSubsystem->HandleAutomationKey(action,
			_stations,
			_selector->CurrentHover(),
			hoveredTake);
		if (automationRes.IsEaten)
			return automationRes;
	}

	bool checkReset = false;
	auto result = ActionResult::NoAction();

	const auto acceptedRig = _rigCoordinator.Accepted();
	if (!contextual && !suppressedKey && acceptedRig && _inputSubsystem->TryAcceptUiRigTriggerInput(acceptedRig->Revision))
	{
		static const std::string EmptyDevice;
		const auto keyState = action.KeyActionType == KeyAction::KEY_DOWN ? 1u : 0u;
		const auto& keyboardTriggers = acceptedRig->InputDispatch.KeyboardTriggers;
		for (const auto& trigger : keyboardTriggers)
		{
			if (!trigger)
				continue;
			std::optional<KeyboardTriggerReceipt> admitted, released;
			auto res = trigger->QueueInputEvent(TRIGGER_INPUT_UI,
				acceptedRig->Revision, TriggerSource::TRIGGER_KEY,
				action.KeyChar, keyState, action, EmptyDevice, std::nullopt, std::nullopt, &admitted, &released);
			if (released) _contextualKeyboard.Retire(trigger, *released);
			if (admitted)
			{
				const auto runtime = std::find_if(acceptedRig->Triggers.begin(), acceptedRig->Triggers.end(),
					[&](const auto& entry) { return entry.Instance == trigger; });
				if (runtime != acceptedRig->Triggers.end()) _contextualKeyboard.Record(*acceptedRig, *runtime, *admitted);
			}

			if (!res.IsEaten)
				continue;

		switch (res.ResultType)
		{
		case ACTIONRESULT_ACTIVATE:
			_isSceneReset.store(false, std::memory_order_relaxed);
			checkReset = true;
			// Propagate any grain the clock just acquired (e.g. from first-loop seed).
			// _SetQuantisation is not called by Station's TrySeedClockFromFirstLoop, so
			// do it here after every activation so all LoopTakes see the current grain.
			if (auto clock = _quantisation.Clock())
			{
				std::scoped_lock lock(_sceneMutex);
				_SetMidiQuantisationGrain(clock->QuantiseSamps(), "loop activated");
			}
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
	}

	if (checkReset)
	{
		std::scoped_lock lock(_sceneMutex);
		_ResetIfEmpty();
	}

	if (result.IsEaten)
		return result;

	auto res = _selector->OnAction(action);

	if (res.IsEaten)
	{
		_UpdateSelection(res.ResultType);
		return res;
	}

	return ActionResult::NoAction();
}

void Scene::_HandleReclockArm()
{
	std::scoped_lock lock(_sceneMutex);
	if (_quantisation.ActiveGrid().Source == QuantisationGridSource::Remote)
	{
		std::cout << "[Quantisation] reclock rejected: remote authority; choose Stay local first" << std::endl;
		return;
	}
	std::cout << ">> Reclock armed (Ctrl+Shift+R) <<" << std::endl;
	_quantisation.ArmReclock(_stations);
	_quantisation.SetRemoteMidiGrid({}, 0, _stations);
	_quantisation.SetMidiGrain(0u, "reclock arm", _stations);
	for (const auto& station : _stations)
		if (station && !station->IsRemote())
			for (const auto& take : station->GetLoopTakes()) take->SetMidiBaseGrid(0u, 0u);
}

ActionResult Scene::_HandleUndo()
{
	std::cout << ">> Undo <<" << std::endl;
	auto res = _undoHistory.Undo();
	return { res };
}

ActionResult Scene::OnAction(GuiAction action)
{
	if (_isSceneQuitting.load(std::memory_order_acquire)) return ActionResult::NoAction();
	if (_controlWriterExclusion.owns_lock() && !SceneSettingsControls::IsOperationControl(action.Index)) return ActionResult::NoAction();
	if (action.Index == GuiMainPanel::LoadRigControlIndex) { _panelRigLoadRequested = true; return ActionResult::NoAction(); }
	const auto deviceChange = _settingsControls->DecodeDeviceChange(action);
	if (deviceChange)
	{
		if (const auto save = std::get_if<SceneSettingsControls::SaveSession>(&*deviceChange))
		{ _RequestSave(save->ChooseFolder, save->AsDefault); return ActionResult::NoAction(); }
		if (std::holds_alternative<SceneSettingsControls::LoadSession>(*deviceChange))
		{ if (_controlOperation == ControlOperation::None) _panelLoadRequested = true; return ActionResult::NoAction(); }
		if (std::holds_alternative<SceneSettingsControls::RetryAudio>(*deviceChange))
		{ _audioStartRequested = true; _audioSchedule.Changed(DeviceSelectionSchedule::Edit::Format); return ActionResult::NoAction(); }
		if (std::holds_alternative<SceneSettingsControls::RefreshMidi>(*deviceChange))
		{ _devicePageDirty = true; RequestMidiRefresh(); return ActionResult::NoAction(); }
		if (std::holds_alternative<SceneSettingsControls::RetryPreferences>(*deviceChange))
		{
			{ std::scoped_lock lock(_sceneMutex); if (_stationManagement.Busy() || _rigSaveInProgress || _rigSaveDialogOpen) return ActionResult::NoAction(); }
			if (_controlOperation != ControlOperation::None || !_rigCoordinator.EditsEnabled()) { _settingsControls->SetDeviceStatus("Preferences retry waits for other operations"); return ActionResult::NoAction(); }
			std::unique_lock lifecycle(_controlLifecycleMutex, std::try_to_lock);
			if (!lifecycle.owns_lock() || !_rigCoordinator.EditsEnabled()) return ActionResult::NoAction();
			const bool merged = _rigCoordinator.MergeHardwarePreferences(_pendingAudioPersistence, _pendingMidiPersistence);
			const auto accepted = AcceptedRigSnapshot();
			if (merged && accepted && SaveHardwarePreferences(accepted->Rig)) { _pendingAudioPersistence.reset(); _pendingMidiPersistence.reset(); _hardwarePersistencePending = false; _settingsControls->SetDeviceStatus("Preferences saved"); }
			else { _settingsControls->SetDeviceStatus("Preferences not saved; retry available"); ReportRuntimeNotice("Device preferences","Preferences not saved; retry available"); }
			return ActionResult::NoAction();
		}
	}
	if (action.ElementType == GuiAction::ACTIONELEMENT_BUTTON &&
		(action.Index == GuiMainPanel::SaveRigDefaultControlIndex || action.Index == GuiMainPanel::SaveRigControlIndex || action.Index == GuiMainPanel::SaveRigAsControlIndex))
	{
		_rigSaveMode = action.Index == GuiMainPanel::SaveRigControlIndex ? RigSaveMode::Save :
			action.Index == GuiMainPanel::SaveRigAsControlIndex ? RigSaveMode::SaveAs : RigSaveMode::StartupDefault;
		_rigSaveRequested = true; return ActionResult::NoAction();
	}
	if (action.ElementType == GuiAction::ACTIONELEMENT_BUTTON &&
		(action.Index == GuiStationControls::CancelTakeDitchControlIndex || action.Index == GuiStationControls::ConfirmTakeDitchControlIndex))
	{
		_popupManager.Close(_takeDitchDialog);
		if (action.Index == GuiStationControls::ConfirmTakeDitchControlIndex) _ConfirmTakeDitch();
		else _CancelTakeDitch();
		return ActionResult::NoAction();
	}
	if (action.ElementType == GuiAction::ACTIONELEMENT_BUTTON &&
		(action.Index == GuiStationControls::CancelRemovalControlIndex || action.Index == GuiStationControls::DeleteRemovalControlIndex))
	{
		_popupManager.Close(_stationDeleteDialog);
		if (action.Index == GuiStationControls::DeleteRemovalControlIndex) _AcceptStationRemoval(true);
		else { std::scoped_lock lock(_sceneMutex); _stationManagement.Cancel(); }
		return ActionResult::NoAction();
	}
	if (const auto value = deviceChange ? std::get_if<SceneSettingsControls::AudioDeviceChange>(&*deviceChange) : nullptr)
	{
		_audioDraft.Name = value->Name;
		_audioSchedule.Changed(DeviceSelectionSchedule::Edit::Device);
		const auto found = std::find_if(_deviceInventory.Devices.begin(), _deviceInventory.Devices.end(), [this](const auto& d){ return d.Name == _audioDraft.Name; });
		if (found != _deviceInventory.Devices.end() && found->Probed)
		{
			_audioDraft.NumChannelsIn = std::min(_audioDraft.NumChannelsIn, found->InputChannels);
			_audioDraft.NumChannelsOut = std::min(_audioDraft.NumChannelsOut, found->OutputChannels);
			if (!_audioDraft.NumChannelsIn && !_audioDraft.NumChannelsOut) { _audioDraft.NumChannelsOut = std::min(2u, found->OutputChannels); if (!_audioDraft.NumChannelsOut) _audioDraft.NumChannelsIn = std::min(2u, found->InputChannels); }
			if (!_SessionHasTakes() && std::find(found->SampleRates.begin(), found->SampleRates.end(), _audioDraft.SampleRate) == found->SampleRates.end() && !found->SampleRates.empty()) _audioDraft.SampleRate = found->PreferredSampleRate ? found->PreferredSampleRate : found->SampleRates.front();
		}
		_devicePageDirty = true;
		return ActionResult::NoAction();
	}
	if (deviceChange && std::holds_alternative<SceneSettingsControls::AudioValueChange>(*deviceChange))
	{
		const auto& change = std::get<SceneSettingsControls::AudioValueChange>(*deviceChange);
		using Field = SceneSettingsControls::AudioField;
		if (change.Field == Field::Inputs) _audioDraft.NumChannelsIn = change.Value;
		if (change.Field == Field::Outputs) _audioDraft.NumChannelsOut = change.Value;
		if (change.Field == Field::SampleRate) _audioDraft.SampleRate = change.Value;
		if (change.Field == Field::BufferSize) _audioDraft.BufSize = change.Value;
		_audioSchedule.Changed(change.Field == Field::Inputs || change.Field == Field::Outputs
			? DeviceSelectionSchedule::Edit::Channels : DeviceSelectionSchedule::Edit::Format);
		return ActionResult::NoAction();
	}
	if (deviceChange && std::holds_alternative<SceneSettingsControls::MidiDeviceChange>(*deviceChange))
	{
		const auto& choice = std::get<SceneSettingsControls::MidiDeviceChange>(*deviceChange);
		const bool enabled = choice.Enabled;
		auto found = std::find_if(_midiDraft.begin(), _midiDraft.end(), [&choice](const auto& d){ return d.Id == choice.RegistryId; });
		if (found == _midiDraft.end())
		{
			std::string id;
			const auto rig = _rigCoordinator.Accepted();
			for (size_t suffix = 1u;; ++suffix)
			{
				id = "midi-" + std::to_string(suffix);
				const bool midiCollision = std::any_of(_midiDraft.begin(), _midiDraft.end(), [&](const auto& device) { return device.Id == id; });
				const bool serialCollision = rig && std::any_of(rig->Rig.User.Serial.Devices.begin(), rig->Rig.User.Serial.Devices.end(),
					[&](const auto& device) { return device.Name == id; });
				if (id != io::RigFile::KeyboardDevice && !midiCollision && !serialCollision) break;
			}
			_settingsControls->SetMidiChoiceRegistryId(choice.Choice, id);
			_midiDraft.push_back({choice.Name, enabled, std::move(id)});
		}
		else found->Enabled = enabled;
		_midiDraftPending = true;
		return ActionResult::NoAction();
	}
	if (action.ElementType == GuiAction::ACTIONELEMENT_MIDIQUANTISATION)
	{
		_ForceGlobalMidiQuantStateMixedOnLocalEdit();
		return ActionResult::NoAction();
	}
	using Settings = SceneSettingsControls;
	const auto change = _settingsControls->Decode(action, _transportOffsetLoopFrac);
	if (!change) return ActionResult::NoAction();
	if (auto view = std::get_if<Settings::ViewModeChange>(&*change))
	{
		auto mode = view->Value;
		if (_camera.SelectDepthChanged(VIEW_STATION == mode))
		{
			mode = VIEW_LOOPTAKE;
			_settingsControls->Mode()->SetCurrentValue(mode, true);
		}
		_viewMode = static_cast<ViewMode>(mode);
		_UpdateSelectDepth(mode);
	}
	else if (auto quantisation = std::get_if<Settings::GlobalQuantisationChange>(&*change))
		_SetGlobalMidiQuantState(quantisation->Value);
	else if (auto subdivision = std::get_if<Settings::SubdivisionChange>(&*change))
	{
		std::scoped_lock lock(_sceneMutex);
		for (const auto& station : _stations)
			if (station && !station->IsRemote())
				for (const auto& take : station->GetLoopTakes())
				{
					auto settings = take->MidiQuantisation();
					settings.Fraction = subdivision->Value;
					take->SetMidiQuantisation(settings);
				}
	}
	else if (auto metronome = std::get_if<Settings::MetronomeChange>(&*change))
		_audioEngine->SetNinjamMetronomeEnabled(metronome->Enabled);
	else if (auto channel = std::get_if<Settings::ChannelChange>(&*change))
	{
		_inputSubsystem->SetForcedChannelOverride(channel->Value, SnapshotStations());
		_settingsControls->RefreshChannelPresentation(_inputSubsystem->ForcedChannelOverride());
	}
	else if (auto offset = std::get_if<Settings::OffsetChange>(&*change))
	{
		_SetTransportOffsetLoopFrac(offset->Value, false);
		_settingsControls->Offset()->SynchronizeValueFromOwner(_transportOffsetLoopFrac);
	}
	else if (std::holds_alternative<Settings::TapTempo>(*change))
	{
		_PulseQuantisationOverlay();
		_HandleTapTempo(Timer::GetTime());
	}
	return ActionResult::NoAction();
}

void Scene::OnTick(Time curTime,
	unsigned int samps,
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	_audioEngine->AdvanceBlock(_quantisation.Clock().get(), curTime, samps, cfg, params);
}

void Scene::_QueueJobs(std::vector<actions::JobAction> jobs)
{
	std::scoped_lock lock(_jobMutex);
	// Stamp origin before publication. Active and queued jobs never change origin.
	for (auto& job : jobs) job.StartupDiagnostic = _startupJobDiagnostics;
	_jobList.insert(_jobList.end(), std::make_move_iterator(jobs.begin()), std::make_move_iterator(jobs.end()));
}

void Scene::CompleteStartupWhenIdle(std::function<void()> complete)
{
	std::scoped_lock lock(_jobMutex);
	_startupJobDiagnostics = false;
	_startupComplete = std::move(complete);
}

void Scene::OnJobTick(Time curTime)
{
	std::scoped_lock lifecycle(_controlLifecycleMutex);
	if (_sessionReplacing.load(std::memory_order_acquire) || _isSceneQuitting.load(std::memory_order_acquire)) return;
	const auto verboseTiming = _loggingConfig.Event == "verbose";
	if (verboseTiming) _ninjamDiagnosticPresenter.ObserveJobTick(curTime);
	std::function<void()> save;
	{ std::scoped_lock lock(_sceneMutex); save = std::move(_explicitRigSaveJob); }
	if (save) save();
	_audioEngine->CollectRetiredSnapshots();
	if (!_isSceneQuitting.load(std::memory_order_acquire))
		_inputSubsystem->AcknowledgeRigTriggerInputCloseFromJob();
	_ReconcileMidiDeviceRequestOnJob();
	if (!_controlAdmissionPending.load(std::memory_order_acquire)) _RefreshMidiIfNeeded();
	_PumpTriggerStructuralActions();
	_PumpSourceLossRecovery();
	_AdvanceRigPublication();
	_PumpMidi();
	_PumpSerial();
	_ConsumeTriggerOutcomes();
	_UpdateRecordedPlaybackCompensation();

	const auto pumpStartedAt = verboseTiming ? Timer::GetTime() : Time{};
	auto pumpResult = _networkService->GetController()->Pump();
	if (verboseTiming)
		_ninjamDiagnosticPresenter.ObservePumpDuration(Timer::GetTime() - pumpStartedAt);
	{
		// Keep the station clock synced when local content seeds it without NINJAM.
		std::scoped_lock lock(_sceneMutex);
		if (_quantisation.LocalMasterTimingPending())
		{
			const auto* target = _quantisation.PendingLocalMasterTake();
			bool present = false;
			for (const auto& station : _stations)
				if (station && !station->IsRemote())
					for (const auto& take : station->GetLoopTakes())
						if (take.get() == target && !station->MembershipRemoving()) present = true;
			if (!present && _audioEngine->LastActivatedLocalMasterTimingVersion() <
				_quantisation.PendingLocalMasterTimingVersion())
			{
				const auto version = _quantisation.PendingLocalMasterTimingVersion();
				_audioEngine->CancelLocalMasterTiming(version);
				for (const auto& station : _stations)
					if (station && !station->IsRemote())
						for (const auto& take : station->GetLoopTakes())
							if (take) take->CancelLocalMasterQuantisationOnOwner();
				_quantisation.CancelLocalMasterTiming(version);
			}
		}
		if (const auto receipt = _audioEngine->LastPreparedLocalMasterTiming();
			receipt && _quantisation.LocalMasterTimingPending() &&
			receipt->Version == _quantisation.PendingLocalMasterTimingVersion() &&
			(receipt->Version > _lastLocalMasterTimingVersion ||
			(_audioEngine->LocalMasterPreparationRetryVersion() == receipt->Version &&
				_audioEngine->LastActivatedLocalMasterTimingVersion() < receipt->Version)))
		{
			const auto length = static_cast<std::uint64_t>(receipt->NewMasterLengthSamps);
			if (_preparingLocalMasterTimingVersion != receipt->Version)
			{
				_preparingLocalMasterTimingVersion = receipt->Version;
				_preparingLocalMasterTimingDelta =
					(static_cast<std::uint64_t>(receipt->NewPhaseSamps) + length
						- receipt->OldAbsoluteSamplePos % length) % length;
			}
			bool prepared = true;
			for (const auto& station : _stations)
			{
				if (!station || station->IsRemote()) continue;
				for (const auto& take : station->GetLoopTakes())
					if (take)
						prepared &= take->PrepareLocalMasterQuantisation(
							_quantisation.EffectiveSamps(),
							(take->MidiQuantisationTransportStartSamps() % length +
								_preparingLocalMasterTimingDelta) % length);
			}
			if (prepared)
			{
				_lastLocalMasterTimingVersion = receipt->Version;
				_audioEngine->CommitLocalMasterTiming(receipt->Version);
			}
		}
		if (_quantisation.LocalMasterTimingPending() &&
			_audioEngine->LastActivatedLocalMasterTimingVersion() >=
				_quantisation.PendingLocalMasterTimingVersion() &&
			_quantisation.PendingLocalMasterTimingVersion() != 0u)
		{
			for (const auto& station : _stations)
				if (station && !station->IsRemote())
					for (const auto& take : station->GetLoopTakes())
						if (take)
							take->CompleteLocalMasterQuantisationOnOwner();
			_quantisation.CompleteLocalMasterTiming();
		}
		if (auto clock = _quantisation.Clock(); clock && !_quantisation.LocalMasterTimingPending() &&
			clock->QuantiseSamps() != _quantisation.EffectiveSamps())
			_SetMidiQuantisationGrain(clock->QuantiseSamps(), "clock geometry changed");
		const auto localTiming = _quantisation.CurrentTempoTiming(_CurrentSampleRate());
		bool hasLocalContent = false;
		for (const auto& station : _stations)
		{
			if (!station || station->IsRemote())
				continue;
			hasLocalContent = !station->GetLoopTakeSnapshot().empty();
			if (hasLocalContent)
				break;
		}
		if (pumpResult.TimingStatus.Changed)
		{
			if (verboseTiming)
				_ninjamDiagnosticPresenter.PresentSessionEdge(pumpResult.TimingStatus,
					hasLocalContent, localTiming, std::cout);
			_ApplyNinjamTimingUpdate(_networkService->ObserveSessionStatus(
				pumpResult.TimingStatus, localTiming));
			if (!pumpResult.TimingStatus.IsAvailable)
				_UpdateRemoteStationsFromSnapshot({});
		}
		if (pumpResult.Snapshot.has_value())
			_HandleRemoteTempoSnapshot(pumpResult.Snapshot.value(), localTiming, hasLocalContent);
		else if (auto clock = _quantisation.Clock())
			_ApplyNinjamTimingUpdate(_networkService->TickTiming(
				localTiming, hasLocalContent, *clock));
		if (_loggingConfig.Event == "verbose" && _audioEngine)
		{
			const auto receipt = _audioEngine->LastAppliedDesiredTiming();
			_ninjamDiagnosticPresenter.Present(_networkService->ObserveAppliedTimingReceipt(receipt), std::cout);
			const auto liveTiming = _audioEngine->LatestNinjamTiming();
			if (receipt && liveTiming && liveTiming->IsConnected
				&& _ninjamDiagnosticPresenter.PlaybackTraceDue(curTime))
			{
				_ninjamDiagnosticPresenter.PresentJobTrace(hasLocalContent, localTiming,
					_networkService->TempoJoinRequestState(), _networkService->PendingRemoteTempoPrompt().has_value(),
					pumpResult.Snapshot ? &pumpResult.Snapshot->Timing : nullptr, std::cout);
				// Job-owned hierarchy sampling; keep verbose output bounded to four takes.
				std::array<ninjam::NinjamPlaybackTakeDiagnostic, 4u> takes;
				std::size_t count = 0u;
				for (const auto& station : _stations)
				{
					if (!station || station->IsRemote()) continue;
					for (const auto& take : station->GetLoopTakes())
					{
						if (!take) continue;
						if (count == takes.size()) break;
						const auto restore = take->MappedMidiRestoreDiagnostic();
						takes[count++] = { station->Name(), take->Id(), take->MidiPlayIndex(),
							take->MidiLoopLengthSamps(), take->MidiAnchorCorrection(), take->NumRecordedSamps(),
							restore.BeforeSamps, restore.TargetSamps, restore.Count, restore.ChangedCount,
							restore.LargestBackstepSamps };
					}
					if (count == takes.size()) break;
				}
				_ninjamDiagnosticPresenter.PresentPlayback(*liveTiming, *receipt,
					std::span<const ninjam::NinjamPlaybackTakeDiagnostic>{ takes.data(), count }, std::cout);
			}
		}
		_HandleAudioLocalContentState(hasLocalContent);
	}

	actions::JobAction job;
	job.SetActionTime(Timer::GetTime());
	job.SetUserConfig(_userConfig);
	job.SetAudioParams(_audioEngine->GetStreamParams());

	{
		std::scoped_lock lock(_jobMutex);
		_ninjamDiagnosticPresenter.ObserveJobQueueDepth(_jobList.size());

		if (_jobList.empty())
		{
			if (auto complete = std::exchange(_startupComplete, {})) complete();
			return;
		}

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

	console::DiagnosticThread::StartupScope startupDiagnostic(job.StartupDiagnostic);
	try
	{
		std::scoped_lock lock(_sceneMutex);
		auto receiver = job.Receiver.lock();
		if (receiver && _JobReceiverAvailableLocked(receiver)) receiver->OnAction(job);
	}
	catch (const std::exception& error)
	{
		_FailJobThread("receiver", error.what(), &job);
	}
	catch (...)
	{
		_FailJobThread("receiver", "unknown exception", &job);
	}

	// Destroy PreInit'd VST plugins on the UI thread that initialized them.
	if (job.PreInitPlugin)
		vst::QueueForUiThreadDestroy(std::move(job.PreInitPlugin));
}

void Scene::_UpdateRecordedPlaybackCompensation()
{
	std::scoped_lock lock(_sceneMutex);
	unsigned int reference = 0u;
	for (const auto& station : _stations)
	{
		if (!station) continue;
		station->PollPlaybackPluginChanges();
		station->RefreshRackLatency();
		reference = std::max(reference, station->RecordedPlaybackLatencySamps());
	}
	if (reference > audio::PlaybackDelay::MaxDelaySamps)
	{
		if (_lastRejectedRecordedLatency != reference)
			std::cerr << "Recorded playback latency exceeds the supported compensation budget: "
				<< reference << " samples" << std::endl;
		_lastRejectedRecordedLatency = reference;
		return;
	}
	// Preflight the whole graph before publishing a newer reference. A running
	// audio transition may defer it; MIDI must keep the current reference too.
	for (const auto& station : _stations)
		if (station && !station->CanConfigureRecordedPlaybackCompensation(reference)) return;
	bool published = true;
	for (const auto& station : _stations)
		if (station) published &= station->ConfigureRecordedPlaybackCompensation(reference);
	if (published) _lastRejectedRecordedLatency.reset();
}

void Scene::_RefreshMidiIfNeeded()
{
	if (!_midiActive.load(std::memory_order_acquire) ||
		_isSceneQuitting.load(std::memory_order_acquire))
		return;
	const auto now = std::chrono::steady_clock::now();
	const bool requested = _midiRefreshRequested.exchange(false, std::memory_order_acq_rel);
	if (!_midiRefreshPolicy.ShouldPoll(now, requested)) return;
	const auto inventory = _inputSubsystem->InventoryMidiInputs();
	const auto previous = _midiState.load(std::memory_order_acquire);
	if (!inventory.Error.empty())
	{
		auto failed = std::make_shared<MidiState>(*previous); failed->Connection.Inventory.Error = inventory.Error;
		_midiState.store(failed, std::memory_order_release); return;
	}
	if (!midi::MidiRefreshPolicy::ShouldRefresh(previous->Connection, inventory, requested))
	{
		if (!previous->Connection.Inventory.Error.empty())
		{
			auto recovered = std::make_shared<MidiState>(*previous); recovered->Connection.Inventory.Error.clear();
			_midiState.store(recovered, std::memory_order_release);
		}
		return;
	}
	std::scoped_lock midiLock(_midiLifecycleMutex);
	if (!_midiActive.load(std::memory_order_acquire) ||
		_isSceneQuitting.load(std::memory_order_acquire))
		return;
	auto result = _inputSubsystem->RefreshMidi(_audioEngine->GetMidiClockAnchor_Ref(),
		_audioEngine->GetStreamParams().SampleRate, inventory, requested);
	const auto connectedIds = result.ConnectedRegistryIds;
	const auto serialIds = _inputSubsystem->ConnectedSerialDeviceIds();
	_ConfirmMidiConnectionChange(previous->Connection, result, connectedIds);
	_midiState.store(std::make_shared<const MidiState>(MidiState{ std::move(result), connectedIds, serialIds }),
		std::memory_order_release);
	const auto inputs = _audioEngine->GetStreamParams().NumInputChannels;
	std::scoped_lock sceneLock(_sceneMutex);
	if (const auto runtime = _rigCoordinator.RefreshRuntimeAvailability(inputs, connectedIds, serialIds))
	{
		_audioEngine->PublishPendingRigSnapshot(runtime);
		_inputSubsystem->PublishRigInputDispatch(runtime);
		_inputSubsystem->OpenRigTriggerInput(runtime->Revision);
		if (_hudPanel)
		{
			_hudPanel->PublishRoutingConfig(inputs, connectedIds, runtime);
		}
	}
}

void Scene::_ConfirmMidiConnectionChange(const midi::MidiConnectionResult& previous,
	const midi::MidiConnectionResult& refreshed, const std::vector<std::string>& connectedIds)
{
	if (!refreshed.Inventory.Error.empty()) return;
	const auto lostIds = midi::MidiDevice::LostConnectedInputIds(previous, refreshed);
	std::scoped_lock lock(_sceneMutex);
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted) return;
	for (const auto& runtime : accepted->Triggers)
	{
		if (!runtime.Instance || runtime.RigTriggerIndex >= accepted->Rig.Triggers.size()) continue;
		const auto& configured = accepted->Rig.Triggers[runtime.RigTriggerIndex];
		const auto isLost = [&lostIds](const std::string& id) {
			return std::find(lostIds.begin(), lostIds.end(), id) != lostIds.end();
		};
		const auto lostControl = [&](const std::vector<io::RigFile::Binding>& bindings) {
			return std::any_of(bindings.begin(), bindings.end(), [&](const io::RigFile::Binding& binding) {
				return binding.Source == io::RigFile::BindingSource::Midi && isLost(binding.Device);
			});
		};
		const bool controlLost = lostControl(configured.ControlBindings.Activate) ||
			lostControl(configured.ControlBindings.Ditch);
		const bool captureLost = std::any_of(configured.CaptureInputs.Midi.begin(),
			configured.CaptureInputs.Midi.end(), isLost);
		runtime.Instance->ConfirmMidiAvailabilityOnJob(connectedIds, lostIds, controlLost, captureLost);
	}
	for (const auto& station : _stations)
		if (station)
			for (const auto& name : lostIds) station->FlushLiveHeldMidiNotesForDevice(name);
}

void Scene::_PumpSourceLossRecovery()
{
	std::scoped_lock lock(_sceneMutex);
	if (_isSceneQuitting.load(std::memory_order_acquire)) return;
	for (const auto& station : _stations)
		if (station) station->FlushPendingLostLiveMidiNotes();
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted)
	{
		// Reset can remove the rig while this job owns admission closure.
		if (_sourceLossRecoveryGateClosed && _audioEngine->CallbackAdmissionIsQuiescent() &&
			_audioEngine->ReopenCallbackAdmission()) _sourceLossRecoveryGateClosed = false;
		return;
	}
	const auto pending = [this, &accepted]() {
		return _stationManagement.Busy() || std::any_of(accepted->Triggers.begin(), accepted->Triggers.end(),
			[](const auto& runtime) { return runtime.Instance && runtime.Instance->HasPendingSourceLoss(); });
	};
	const auto publicationNeedsAcknowledgement = [this]() {
		return std::any_of(_stations.begin(), _stations.end(),
			[](const auto& station) { return station && station->HasUnacknowledgedAudioState(); });
	};
	const auto heartbeat = _audioEngine->AudioCallbackHeartbeat();
	const auto now = std::chrono::steady_clock::now();
	if (_sourceRecoveryHeartbeatAt == std::chrono::steady_clock::time_point{} || heartbeat != _sourceRecoveryHeartbeat)
	{
		_sourceRecoveryHeartbeat = heartbeat;
		_sourceRecoveryHeartbeatAt = now;
	}
	if (!_sourceLossRecoveryGateClosed)
	{
		if ((!pending() && !publicationNeedsAcknowledgement()) ||
			(heartbeat != 0u && now - _sourceRecoveryHeartbeatAt < std::chrono::milliseconds(500))) return;
		// Heartbeat is only a stalled-reader hint. Admission closure plus the
		// active-reader acknowledgement below is the authority to borrow fields.
		_audioEngine->CloseCallbackAdmission();
		_sourceLossRecoveryGateClosed = true;
	}
	if (!_audioEngine->CallbackAdmissionIsQuiescent()) return;
	_audioEngine->ApplyPendingRigWhileQuiescent();
	const auto revision = _audioEngine->AppliedRigRevision();
	const auto streamParams = _audioEngine->GetStreamParams();
	for (const auto& runtime : accepted->Triggers)
		if (runtime.Instance)
		{
			runtime.Instance->OnTick(Timer::GetTime(), 0u, _userConfig, streamParams, revision);
			runtime.Instance->ProcessStructuralActionsOnJob(_userConfig, streamParams);
			runtime.Instance->OnTick(Timer::GetTime(), 0u, _userConfig, streamParams, revision);
		}
	_audioEngine->ApplyPendingRigWhileQuiescent();
	// The gate is the reader acknowledgement when a stopped driver cannot
	// observe newly published removals. External target pins do not hold it shut.
	for (const auto& station : _stations)
		if (station)
		{
			station->AcknowledgeAudioBoundary();
			station->ReleaseRetiredAudioStates();
		}
	if (!pending() && _audioEngine->ReopenCallbackAdmission()) _sourceLossRecoveryGateClosed = false;
}

void Scene::_PumpMidi()
{
	std::scoped_lock midiLock(_midiLifecycleMutex);
	if (!_midiActive.load(std::memory_order_acquire)) return;
	auto stations = SnapshotStations();
	_inputSubsystem->PumpMidi(stations, _audioEngine->GetAudioSampleCounter(),
		_audioEngine->GetStreamParams(), _sceneMutex);
}

void Scene::_PumpTriggerStructuralActions()
{
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted)
		return;
	const auto streamParams = _audioEngine->GetStreamParams();
	std::scoped_lock lock(_sceneMutex);
	for (const auto& runtime : accepted->Triggers)
		if (runtime.Instance)
			runtime.Instance->ProcessStructuralActionsOnJob(_userConfig, streamParams);
	// Deferred punch transitions publish only a bounded audio revision. Refresh
	// current capture truth here so even a complete between-frame punch is retained.
	for (const auto& station : _stations)
		for (const auto& take : station->GetLoopTakeSnapshot())
			take->PublishCameraCaptureObservation();
}

void Scene::_AdvanceRigPublication()
{
	std::scoped_lock sceneLock(_sceneMutex);
	const auto staged = _rigCoordinator.Staged();
	if (staged)
	{
		const auto revision = staged->Revision;
		if (_rigTransitionRequestedRevision != revision)
		{
			const auto accepted = _rigCoordinator.Accepted();
			if (!accepted || !_inputSubsystem->RigTriggerInputReadyForAudioBoundary(accepted->Revision))
				return;
			if (staged->RemovedStation && !_stationManagement.Confirmed &&
				!staged->RemovedStation->GetLoopTakeSnapshot().empty() && !_stationManagement.CancellationStarted)
			{
				_rigCoordinator.CompleteTransition(revision, false, {});
				_stationManagement.Candidate.reset();
				_stationManagement.NeedsConfirmation = true;
				_inputSubsystem->OpenRigTriggerInput(accepted->Revision);
				return;
			}
			if (staged->RemovedStation && !_stationManagement.CancellationStarted)
			{
				_quantisation.DetachStationMaster(_stationManagement.Target);
				staged->RemovedStation->BeginMembershipRemoval();
				staged->RemovedStation->FlushLiveHeldMidiNotes();
				for (const auto& trigger : accepted->Triggers)
				{
					const auto history = trigger.Instance->GetTakes();
					if (trigger.BoundReceiver.get() == staged->RemovedStation || std::any_of(history.begin(), history.end(),
						[&](const auto& take) { return take.Receiver.get() == staged->RemovedStation; }))
						trigger.Instance->RequestReceiverRemoval(staged->RemovedStation);
				}
				_stationManagement.CancellationStarted = true;
			}
			if (_audioEngine->CallbackAdmissionIsQuiescent())
			{
				for (const auto& trigger : accepted->Triggers)
					trigger.Instance->OnTick(Timer::GetTime(), 0u, _userConfig, _audioEngine->GetStreamParams(), accepted->Revision);
			}
			if (staged->RemovedStation)
				for (const auto& trigger : accepted->Triggers)
					if (trigger.Instance->ReceiverRemovalPending() && !trigger.Instance->ReceiverRemovalReady()) return;
			for (const auto& trigger : accepted->Triggers)
				if (trigger.Instance->ReceiverRemovalReady()) trigger.Instance->CompleteReceiverRemovalOnJob();
			_audioEngine->RequestRigTriggerTransition(revision, accepted, staged);
			_rigTransitionRequestedRevision = revision;
		}
		if (_audioEngine->CallbackAdmissionIsQuiescent()) _audioEngine->ApplyPendingRigWhileQuiescent();
		if (_audioEngine->RejectedRigRevision() == revision)
		{
			_rigCoordinator.CompleteTransition(revision, false, {});
			if (_stationManagement.Busy()) _stationManagement.Failed = true;
			_audioEngine->ClearRigTriggerTransition();
			_rigTransitionRequestedRevision = 0u;
			if (const auto accepted = _rigCoordinator.Accepted())
				_inputSubsystem->OpenRigTriggerInput(accepted->Revision);
			return;
		}
		if (_audioEngine->TransitionReadyRigRevision() != revision)
			return;
		const auto result = _rigCoordinator.CompleteTransition(revision, true, {});
		_audioEngine->ClearRigTriggerTransition();
		_rigTransitionRequestedRevision = 0u;
		if (result != RigCoordinator::EditResult::Pending)
		{
			if (const auto accepted = _rigCoordinator.Accepted())
				_inputSubsystem->OpenRigTriggerInput(accepted->Revision);
			return;
		}
		_audioEngine->PublishPendingRigSnapshot(_rigCoordinator.Pending());
		return;
	}

	const auto pending = _rigCoordinator.Pending();
	if (!pending)
		return;
	if (_audioEngine->CallbackAdmissionIsQuiescent()) _audioEngine->ApplyPendingRigWhileQuiescent();
	const auto audioRevision = _audioEngine->AppliedRigRevision();
	if (audioRevision != pending->Revision ||
		!_rigCoordinator.ObserveAudioAcknowledgement(audioRevision))
		return;
	if (_rigCoordinator.InputAcknowledgement() != pending->Revision)
	{
		_inputSubsystem->PublishRigInputDispatch(pending);
		if (!_rigCoordinator.AcknowledgeInput(pending->Revision))
			return;
	}
	for (const auto& update : pending->TriggerRouteUpdates)
		if (update.CandidateIndex < pending->Triggers.size()) pending->Triggers[update.CandidateIndex].Receiver.reset();
	if (!_rigCoordinator.PromoteAcknowledged())
		return;
	if (pending->RenamedStation)
		_quantisationFeedback = "Station renamed. Save both JAM session and rig explicitly for consistent reload.";
	if (pending->MembershipChange) _stationManagement.Committed = true;
	_audioEngine->ReleaseRigSnapshotsBefore(pending->Revision);
	_rigCoordinator.ReleaseRetired();
	if (_hudPanel)
	{
		// Routing is published under the surrounding Scene lock; UI owns widgets.
		unsigned int audioInputs = _audioEngine->GetStreamParams().NumInputChannels;
		const auto midiState = _midiState.load(std::memory_order_acquire);
		_hudPanel->PublishRoutingConfig(audioInputs, midiState->ConnectedIds, pending);
	}
	if (!pending->MembershipChange) _inputSubsystem->OpenRigTriggerInput(pending->Revision);
}

RigCoordinator::EditResult Scene::RequestRigEdit(const io::RigFile& candidateRig)
{
	std::scoped_lock lock(_sceneMutex);
	return _RequestRigEditLocked(candidateRig);
}

std::string Scene::_RequestStationRenameLocked(const std::shared_ptr<Station>& station,
	std::uint64_t revision, const std::string& name)
{
	if (_controlOperation != ControlOperation::None || _stationManagement.Busy() ||
		_controlWriterExclusion.owns_lock() || _sessionReplacing.load(std::memory_order_acquire) ||
		_isSceneQuitting.load(std::memory_order_acquire) || _rigSaveDialogOpen || _rigSaveInProgress ||
		_RoutingEditAvailability() != gui::RoutingEditAvailability::Ready)
		return "Busy; keep the draft and retry";
	// Like station membership staging, metadata staging needs the Scene writer
	// lock, not the job lifecycle lock held throughout ordinary job ticks.
	const auto result = _rigCoordinator.SubmitStationRename(station, revision, name);
	if (result != RigCoordinator::EditResult::Pending) return "Invalid, duplicate, reserved or stale name";
	const auto staged = _rigCoordinator.Staged();
	const auto accepted = _rigCoordinator.Accepted();
	if (_audioEngine->AppliedRigRevision() != accepted->Revision ||
		!_inputSubsystem->RequestCloseRigTriggerInputFromUi(accepted->Revision))
	{
		_rigCoordinator.CompleteTransition(staged->Revision, false, {});
		return "Input transition busy; retry";
	}
	_rigTransitionRequestedRevision = 0u;
	return {};
}

RigCoordinator::EditResult Scene::_RequestRigEditLocked(const io::RigFile& candidateRig)
{
	if (_controlOperation != ControlOperation::None || _stationManagement.Busy() || _rigSaveDialogOpen || _rigSaveInProgress) return RigCoordinator::EditResult::EditsDisabled;
	switch (_RoutingEditAvailability())
	{
	case gui::RoutingEditAvailability::Applying: return RigCoordinator::EditResult::EditsDisabled;
	case gui::RoutingEditAvailability::AudioCallbackInactive: return RigCoordinator::EditResult::AudioCallbackInactive;
	case gui::RoutingEditAvailability::TriggerBusy: return RigCoordinator::EditResult::TriggerBusy;
	case gui::RoutingEditAvailability::Ready: break;
	}
	std::unique_lock lifecycle(_controlLifecycleMutex, std::try_to_lock);
	if (!lifecycle.owns_lock() || !_rigCoordinator.EditsEnabled()) return RigCoordinator::EditResult::EditsDisabled;
	const auto accepted = _rigCoordinator.Accepted();
	const auto result = _rigCoordinator.SubmitCandidate(candidateRig);
	if (result == RigCoordinator::EditResult::ReinitialisationRequired)
		_quantisationFeedback = "Device setup and defaults require scene reinitialisation; reload or restart with the updated rig.";
	if (result == RigCoordinator::EditResult::Pending)
	{
		const auto staged = _rigCoordinator.Staged();
		if (!accepted || !staged || _audioEngine->AppliedRigRevision() != accepted->Revision)
		{
			if (staged)
				_rigCoordinator.CompleteTransition(staged->Revision, false, {});
			return RigCoordinator::EditResult::TransitionRejected;
		}
		if (!_inputSubsystem->RequestCloseRigTriggerInputFromUi(accepted->Revision))
		{
			_rigCoordinator.CompleteTransition(staged->Revision, false, {});
			return RigCoordinator::EditResult::TransitionRejected;
		}
		_rigTransitionRequestedRevision = 0u;
	}
	return result;
}

gui::RoutingEditAvailability Scene::_RoutingEditAvailability()
{
	if (!_rigCoordinator.EditsEnabled())
		return gui::RoutingEditAvailability::Applying;
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted || !_inputSubsystem->TryAcceptUiRigTriggerInput(accepted->Revision))
		return gui::RoutingEditAvailability::Applying;

	const auto heartbeat = _audioEngine->AudioCallbackHeartbeat();
	const auto now = std::chrono::steady_clock::now();
	if (heartbeat != _lastAudioCallbackHeartbeat)
	{
		_lastAudioCallbackHeartbeat = heartbeat;
		_lastAudioCallbackHeartbeatAt = now;
	}
	constexpr auto heartbeatTimeout = std::chrono::milliseconds(500);
	if (heartbeat == 0u || _lastAudioCallbackHeartbeatAt == std::chrono::steady_clock::time_point{} ||
		now - _lastAudioCallbackHeartbeatAt > heartbeatTimeout)
		return gui::RoutingEditAvailability::AudioCallbackInactive;
	return gui::RoutingEditAvailability::Ready;
}

void Scene::_PumpSerial()
{
	std::scoped_lock midiLock(_midiLifecycleMutex);
	if (!_midiActive.load(std::memory_order_acquire)) return;
	_inputSubsystem->PumpSerial(_stations, _audioEngine->GetStreamParams(), _sceneMutex);
}

void Scene::_ConsumeTriggerOutcomes()
{
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted)
		return;
	for (auto observed = _triggerOutcomeCounts.begin(); observed != _triggerOutcomeCounts.end();)
	{
		const auto stillPublished = std::any_of(accepted->Triggers.begin(), accepted->Triggers.end(),
			[instance = observed->first](const RigSnapshotTrigger& runtime)
			{
				return runtime.Instance.get() == instance;
			});
		if (!stillPublished)
			observed = _triggerOutcomeCounts.erase(observed);
		else
			++observed;
	}
	for (const auto& runtime : accepted->Triggers)
	{
		if (!runtime.Instance)
			continue;
		const auto activation = runtime.Instance->ActivationOutcomeCount();
		const auto ditch = runtime.Instance->DitchOutcomeCount();
		auto& observed = _triggerOutcomeCounts[runtime.Instance.get()];
		if (activation > observed.first)
		{
			_isSceneReset.store(false, std::memory_order_relaxed);
			if (auto clock = _quantisation.Clock())
				_SetMidiQuantisationGrain(clock->QuantiseSamps(), "loop activated");
		}
		if (ditch > observed.second)
		{
			std::scoped_lock lock(_sceneMutex);
			_ResetIfEmpty();
		}
		observed = { activation, ditch };
	}
}

void Scene::InitReceivers()
{
	_selector->SetReceiver(ActionReceiver::shared_from_this());
	_quantisationInteraction.SetFeedbackSink([this](const std::string& text) {
		// UI-owned text; apply to the HUD during the next locked draw.
		_quantisationFeedback = text;
	});
	_mainPanel->SetCommandOwner(ActionReceiver::shared_from_this());
	_selectionPanel->SetCommandOwner(ActionReceiver::shared_from_this());
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
		_InvalidateHover2d();
	}
}

void Scene::SetHover3d(std::vector<unsigned char> path, Action::Modifiers modifiers,
	std::optional<utils::Position2d> pointerPosition)
{
	if (_ctrlHeld || _loopEditor.IsEngaged() || _backgroundGesture)
		return;
	if (pointerPosition)
	{
		_cursorPos = *pointerPosition;
		_InvalidateHover2d();
	}
	// Resolve GUI ownership before taking the scene lock: station snapshots also lock it.
	std::vector<std::weak_ptr<base::GuiElement>> guiPath;
	_ResolveHoverPath2d(guiPath);
	std::unique_lock lock(_sceneMutex);
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
	_hoverElement3d = _ChildFromPathLocked(fullElementPath);
	elementPath = fullElementPath;
	// Keep the picked stream identity; MIDI layers are independent loop targets.

	elementPath = TrimPath(elementPath, _selector->CurrentSelectDepth() + 1);
	// Rack controls own the pointer even when the 3D picker sees a model behind them.
	if (std::any_of(guiPath.begin(), guiPath.end(), [](const auto& element) {
		return nullptr != std::dynamic_pointer_cast<GuiRack>(element.lock());
	}))
		elementPath.clear();

	if (elementPath != _lastLoggedHoverPath)
	{
		if (_loggingConfig.Ui == "verbose")
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

	auto hovering = _ChildFromPathLocked(elementPath);
	if (nullptr != hovering)
	{
		isSelected = hovering->IsSelected();

		tweakState = _SelectionTweakStateLocked(hovering);
	}

	_selector->UpdateCurrentHover(elementPath,
		modifiers,
		isSelected,
		tweakState);
	_quantisationInteraction.RefreshOverlay(_InteractionContextLocked(),
		[this](const std::vector<unsigned char>& path) { return _ChildFromPathLocked(path); });

	lock.unlock();
	_UpdateSelection(ACTIONRESULT_DEFAULT);

	auto candidate = (Action::MODIFIER_SHIFT & modifiers) ? _ChildFromPath(elementPath) : nullptr;
	_UpdateStationQuantisation(candidate, _selector->CurrentSelectDepth(), false);
}

void Scene::Reset()
{
	// Reset also runs on the job thread; the controller/editor belong to UI.
	_quantisationInputResetRequested.store(true, std::memory_order_release);
	std::cout << "Reset" << std::endl;
	_ClearTimingState(true);
	_ClearStationQuantisation();
	_quantisation.ClearOverlay();
	_isSceneReset.store(true, std::memory_order_relaxed);
	// Reset callers share the structural scene lock, including trigger outcomes.
	_cameraRecordings->Reset(Timer::GetTime());
}

void Scene::InitGui()
{
	_selector->Init();
}

void Scene::InitAudio(bool generatedRig, const audio::AsioInventory* inventory, bool exactSelection)
{
	std::scoped_lock lifecycle(_controlLifecycleMutex);
	if (inventory) _deviceInventory = *inventory;
	_devicePageDirty = true;
	const auto previousMidi = _midiState.load(std::memory_order_acquire);
	// Setup audio engine which starts device
	bool started = false;
	{
		std::scoped_lock lock(_sceneMutex);
		_audioEngine->CloseCallbackAdmission();
		_sourceLossRecoveryGateClosed = true;
		started = _audioEngine->InitWithBlockAdvancement(_networkService->GetController(),
			_quantisation.Clock(), generatedRig, inventory, exactSelection);
		if (!started)
		{
			const auto& report = _audioEngine->GetAsioOpenReport();
			ReportRuntimeNotice("Audio device",report.Attempts.empty() ? "Audio device could not be started; retry in settings" : report.Attempts.back().Error);
		}
		if (_audioEngine->GetAsioInventory().EnumerationError.empty()) _deviceInventory = _audioEngine->GetAsioInventory();
		if (!started && !_audioEngine->LastInitReplacedStream())
		{
			// Stop failure is unknown availability, not a confirmed replacement.
			// Preserve the existing capture identities and MIDI connections.
			if (_audioEngine->CallbackAdmissionIsQuiescent() &&
				_audioEngine->ReopenCallbackAdmission()) _sourceLossRecoveryGateClosed = false;
			return;
		}
		// Replacing the stream invalidates the common sample clock even when its
		// display name/channels remain unchanged. New callbacks stay gated until
		// prior captures have settled through the usual structural result protocol.
		const auto actual = _audioEngine->GetStreamParams();
		if (started) _SetAudioDraftFromStream(actual);
		++_audioStreamEpoch;
		if (const auto accepted = _rigCoordinator.Accepted())
			for (const auto& runtime : accepted->Triggers)
				if (runtime.Instance) runtime.Instance->ConfirmAudioAvailabilityOnJob(
					_audioStreamEpoch, started ? actual.Name : std::string(), started ? actual.NumInputChannels : 0u);
	}

	if (started) {
		InitMidi(generatedRig);
		InitSerial();
	}
	else
	{
		_midiInputInventory.reset();
		CloseMidi();
		_midiState.store( std::make_shared<const MidiState>(), std::memory_order_release);
	}
	const auto midiState = _midiState.load(std::memory_order_acquire);
	const auto actualInputs = started ? _audioEngine->GetStreamParams().NumInputChannels : 0u;
	_ConfirmMidiConnectionChange(previousMidi->Connection, midiState->Connection, midiState->ConnectedIds);
	std::optional<io::UserConfig::AudioSettings> confirmedGeneratedAudio;
	if (started && generatedRig)
	{
		const auto actual = _audioEngine->GetStreamParams();
		auto settings = _userConfig.Audio;
		settings.Name = actual.Name;
		settings.SampleRate = actual.SampleRate;
		settings.BufSize = actual.BufSize;
		settings.NumBuffers = actual.NumBuffers;
		settings.LatencyIn = actual.InputLatency;
		settings.LatencyOut = actual.OutputLatency;
		settings.NumChannelsIn = actual.NumInputChannels;
		settings.NumChannelsOut = actual.NumOutputChannels;
		confirmedGeneratedAudio = std::move(settings);
	}
	if (const auto runtime = _rigCoordinator.RefreshRuntimeAvailability(actualInputs, midiState->ConnectedIds,
		midiState->ConnectedSerialIds, std::move(confirmedGeneratedAudio)))
	{
		_audioEngine->PublishPendingRigSnapshot(runtime);
		_inputSubsystem->PublishRigInputDispatch(runtime);
		_inputSubsystem->OpenRigTriggerInput(runtime->Revision);
		if (_hudPanel)
		{
			std::scoped_lock lock(_sceneMutex);
			_hudPanel->PublishRoutingConfig(actualInputs, midiState->ConnectedIds, runtime);
		}
	}

	CommitChanges();
	_PumpSourceLossRecovery();
}

void Scene::SetLogging(io::LoggingConfig config) noexcept
{
	_loggingConfig = config;
	if (_networkService)
		_networkService->SetTimingDiagnosticsEnabled(_loggingConfig.Event == "verbose");
	if (_inputSubsystem)
		_inputSubsystem->SetLogging(_loggingConfig);
	if (_windowSubsystem)
		_windowSubsystem->SetLogging(_loggingConfig);
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

bool Scene::PauseAudio()
{
	auto* device = _audioEngine ? _audioEngine->GetDevice() : nullptr;
	return device && device->Pause();
}

bool Scene::ResumeAudio()
{
	auto* device = _audioEngine ? _audioEngine->GetDevice() : nullptr;
	return device && device->Resume();
}

bool Scene::InitGlobalKeyCapture()
{
	return _inputSubsystem->InitGlobalKeyCapture();
}

void Scene::CloseGlobalKeyCapture()
{
	_inputSubsystem->CloseGlobalKeyCapture();
}

bool Scene::PumpGlobalKeyCapture(actions::KeyAction& action) noexcept
{
	return _inputSubsystem->PumpGlobalKeyCapture(action);
}

void Scene::Shutdown()
{
	if (_hudPanel) _hudPanel->CancelTriggerRenames();
	for (const auto& station : _stations)
		if (auto label = station->GetGuiRack()->StationLabel()) label->Cancel();
	_vstRackControls->Reset();
	_sessionReplacing.store(true, std::memory_order_release);
	if (_controlWriterExclusion.owns_lock()) _controlWriterExclusion.unlock();
	// Finish any admitted job tick before borrowing lifecycle state. No scene,
	// MIDI or application gate mutex is held while acquiring this owner boundary.
	std::unique_lock lifecycle(_controlLifecycleMutex);
	_backgroundGesture.reset();
	{
		// Recovery borrows callback-owned fields under this same lock. Finish
		// that borrow before teardown, and prevent a later job reopening admission.
		std::scoped_lock lock(_sceneMutex);
		// Publish the permanent close token before the job loop can exit and make
		// its final acknowledgement. Releasing quit first can strand that token.
		_inputSubsystem->CloseRigTriggerInputForever();
		_inputSubsystem->EndTriggerLearn();
		if (_hudPanel) _hudPanel->CancelTriggerLearn();
		_sourceLossRecoveryGateClosed = true; // Permanent admission closure outlives recovery.
		_isSceneQuitting.store(true, std::memory_order_release);
	}
	_rigCoordinator.Shutdown();
	_audioEngine->ClearRigTriggerTransition();
	// RtAudio::Stop() waits for an in-flight callback to return.  Do this before
	// closing an editor or releasing a plugin: the callback can be dispatching
	// VST processing, MIDI, or recorded parameter automation.
	CloseAudio();
	CloseAllVstEditorWindows();
	lifecycle.unlock(); // Final job acknowledgement must run before join.

	if (_jobRunner.joinable())
		_jobRunner.join();
	_runtimeRendererReady = false;
	_runtimeDialogs.Pump(_popupManager,_sizeParams.Size,false);
	assert(_inputSubsystem->RigTriggerInputReadyForShutdown());
	_inputSubsystem->PublishEmptyRigInputDispatch();
	CloseGlobalKeyCapture();
	CloseSerial();
	CloseMidi();

	// No work from the outgoing session may survive a session replacement.
	// In particular, queued VST loads retain UI-thread-created plugin objects;
	// hand those objects back to the UI destroy queue before dropping the jobs.
	std::list<actions::JobAction> abandonedJobs;
	// The consumer thread is joined and Shutdown is called by the UI owner, so
	// there can be no concurrent queue reader or producer at this point.
	abandonedJobs.swap(_jobList);
	for (auto& job : abandonedJobs)
	{
		if (job.PreInitPlugin)
			vst::QueueForUiThreadDestroy(std::move(job.PreInitPlugin));
	}

	ForceUnloadAllVstPlugins();
	if (_stationManagement.Added) { _stationManagement.Added->ReleaseResources(); _stationManagement.Added->DetachGuiOwnershipForRetirement(); }
	_stationManagement.Finish();
	for (const auto& station : _retiredStations) { station->ReleaseResources(); station->ForceUnloadAllVstPlugins(); }
	_retiredStations.clear();
	_audioEngine->Close();
	_contextualKeyboard.ClearAfterReadersStopped();
	_rigCoordinator.ReleaseAfterReadersStopped();

	// Action senders own their receivers.  These Scene-owned senders therefore
	// keep the Scene alive unless their receiver links are cleared after all
	// input and job readers have stopped.
	_selector->SetReceiver({});
	if (_stationDeleteDialog) _stationDeleteDialog->SetButtonReceiver({});
	if (_takeDitchDialog) _takeDitchDialog->SetButtonReceiver({});
	for (const auto& station : _stations)
		if (station)
		{
			station->SetReceiver({});
			station->DetachGuiOwnershipForRetirement();
		}
}

void Scene::ForceUnloadAllVstPlugins()
{
	// Shutdown() stops the audio device before reaching here.  Releasing a VST
	// while its callback may call SetParameter/ProcessBlock is not safe.
	std::scoped_lock lock(_sceneMutex);
	for (auto& station : _stations)
	{
		if (station)
			station->ForceUnloadAllVstPlugins();
	}
}

void Scene::CommitChanges()
{
	std::vector<JobAction> syncJobs = {};
	std::vector<JobAction> jobList = {};
	std::shared_ptr<Station> closeStationEditors;
	std::vector<std::shared_ptr<Station>> reclaimStations;
	bool hoverChanged = false;

	{
		// Non-blocking: avoid holding up the audio thread. If the lock is
		// contended, pending snapshots remain unconsumed until next frame.
		if (!_sceneMutex.try_lock())
			return;

		std::lock_guard<std::mutex> lock(_sceneMutex, std::adopt_lock);

		if (_stationManagement.Committed) closeStationEditors = _stationManagement.Target;
		_CommitStationMembershipLocked();
		for (auto it = _retiredStations.begin(); it != _retiredStations.end();)
		{
			if (it->use_count() == 1u)
			{ reclaimStations.push_back(std::move(*it)); it = _retiredStations.erase(it); }
			else ++it;
		}
		std::optional<ninjam::NinjamRemoteSnapshot> pendingRemoteSnapshot = _stationManagement.Busy() ? std::nullopt : _networkService->GetController()->TakePendingSnapshot();
		if (!_stationManagement.Busy() && _deferredRemoteSnapshot)
		{
			if (!pendingRemoteSnapshot) pendingRemoteSnapshot = std::move(_deferredRemoteSnapshot);
			_deferredRemoteSnapshot.reset();
		}
		hoverChanged = pendingRemoteSnapshot.has_value();
		// CommitChanges is UI-owned: reconcile before resource traversal, VUs and drawing.
		if (_hudPanel && _hudPanel->ApplyPendingRoutingConfig())
			hoverChanged = true;
		if (_hudPanel) _hudPanel->UpdateTriggerLearn();

		if (pendingRemoteSnapshot.has_value())
			_UpdateRemoteStationsFromSnapshot(pendingRemoteSnapshot.value());

		for (auto& station : _stations)
		{
			if (station->MembershipRemoving()) continue;
			station->ReleaseRetiredAudioStates();
			auto jobs = station->CommitChanges();
			if (!jobs.empty())
			{
				hoverChanged = true;
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

		_tempoPrompt.Synchronize(_networkService->PendingRemoteTempoPrompt(), _popupManager, _sizeParams.Size);

		// Completion/update payloads share structural ownership with start/end/ditch.
		// Keep validation and mutation under the same off-callback scene lock.
		for (auto& job : syncJobs)
		{
			auto receiver = job.Receiver.lock();
			if (receiver && _JobReceiverAvailableLocked(receiver))
				receiver->OnAction(job);
		}
	}

	// Native/plugin teardown may synchronously dispatch focus messages. Keep strong
	// pins, but hold neither the scene nor editor registry mutex across that call.
	if (closeStationEditors) _windowSubsystem->CloseVstEditorsForStation(closeStationEditors);
	for (const auto& station : reclaimStations)
	{ station->ReleaseResources(); station->ForceUnloadAllVstPlugins(); }
	bool needsConfirmation;
	{ std::scoped_lock lock(_sceneMutex); needsConfirmation = std::exchange(_stationManagement.NeedsConfirmation, false); }
	if (needsConfirmation) _AcceptStationRemoval(false);
	// Initialize VSTs on the UI thread after releasing _sceneMutex.
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

	if (!jobList.empty()) _QueueJobs(std::move(jobList));

	if (hoverChanged)
		_InvalidateHover2d();
}

void Scene::ApplyDeferredHoverUpdates()
{
	// Ctrl edits the captured targets, so pointer motion must not change their feedback.
	if (_ctrlHeld)
		return;
	// Keep active hover in sync with both deferred 2D hit-testing and latest 3D picker result.
	if (!_hover2dDirty)
		return;

	if (_popupManager.IsOpen())
	{
		if (!_hoverPath2d.empty())
		{
			std::vector<std::weak_ptr<base::GuiElement>> none;
			_ApplyHoverPath2d(none);
			_hoverPath2d.clear();
		}

		_hover2dDirty = false;
		return;
	}

	if (_touchDownElement.lock() || _isSceneTouching || _backgroundGesture)
		return;

	_hoverPath2dScratch.clear();
	_ResolveHoverPath2d(_hoverPath2dScratch);
	auto& nextPath = _hoverPath2dScratch;
	_ApplyHoverPath2d(nextPath);

	bool stationHoverPromotedFrom2d = false;
	const bool rackOwnsHover = std::any_of(nextPath.begin(), nextPath.end(), [](const auto& element) {
		return nullptr != std::dynamic_pointer_cast<GuiRack>(element.lock());
	});

	if (!nextPath.empty() && !rackOwnsHover)
	{
		_hoverPath2dNextSharedScratch.clear();
		_LockHoverPath(nextPath, _hoverPath2dNextSharedScratch);
		auto& nextShared = _hoverPath2dNextSharedScratch;
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

			auto stationPath = TrimPath(stationPathRaw, _selector->CurrentSelectDepth() + 1u);
			bool isSelected = false;
			auto tweakState = base::Tweakable::TweakState::TWEAKSTATE_NONE;
			auto hovering = _ChildFromPath(stationPath);
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
			_UpdateSelection(ACTIONRESULT_DEFAULT);
			stationHoverPromotedFrom2d = true;
		}
	}

	if (!stationHoverPromotedFrom2d && (rackOwnsHover || _hoverPath3d.empty()) && !_selector->CurrentHover().empty())
	{
		_selector->UpdateCurrentHover({ },
			Action::MODIFIER_NONE,
			false,
			base::Tweakable::TweakState::TWEAKSTATE_NONE);
		_UpdateSelection(ACTIONRESULT_DEFAULT);
	}
	else if (!stationHoverPromotedFrom2d && !rackOwnsHover && !_hoverPath3d.empty())
	{
		// The picker may keep the same model ID while the pointer leaves a control.
		auto pickPath = _hoverPath3d;
		for (auto& segment : pickPath)
			++segment;
		SetHover3d(std::move(pickPath), Action::MODIFIER_NONE);
	}

	_hoverPath2d = std::move(nextPath);
	_hover2dDirty = false;
}

void Scene::_InvalidateHover2d()
{
	_hover2dDirty = true;
}

void Scene::_ResolveHoverPath2d(std::vector<std::weak_ptr<base::GuiElement>>& outPath)
{
	if (_EditControlsSuppressed() && !_loopEditor.IsEngaged())
	{
		outPath.clear();
		return;
	}
	outPath.clear();

	auto resolveTop = [this](const std::shared_ptr<base::GuiElement>& root) {
		if (!root)
			return std::shared_ptr<base::GuiElement>();

		return root->FindTopmostDescendant(root->ParentToLocal(_cursorPos));
	};

	auto leaf = std::shared_ptr<base::GuiElement>();
	for (auto it = _guiChildren.rbegin(); it != _guiChildren.rend(); ++it)
	{
		leaf = resolveTop(*it);
		if (leaf)
			break;
	}

	if (!leaf)
	{
		for (auto& station : SnapshotStations())
		{
			leaf = resolveTop(std::static_pointer_cast<base::GuiElement>(station));
			if (leaf)
				break;
		}
	}

	while (leaf)
	{
		outPath.push_back(leaf);
		leaf = leaf->Parent();
	}

	std::reverse(outPath.begin(), outPath.end());
}

void Scene::_ApplyHoverPath2d(const std::vector<std::weak_ptr<base::GuiElement>>& nextPath)
{
	_hoverPath2dPrevSharedScratch.clear();
	_hoverPath2dNextSharedScratch.clear();
	_LockHoverPath(_hoverPath2d, _hoverPath2dPrevSharedScratch);
	_LockHoverPath(nextPath, _hoverPath2dNextSharedScratch);

	// Only the leaf is a pointer target. Opted-in panel backgrounds also follow
	// the resolved ownership path, without participating in control routing.
	for (const auto& element : _hoverPath2dPrevSharedScratch)
	{
		element->ApplyHoverState(false);
		if (auto panel = std::dynamic_pointer_cast<GuiPanel>(element))
			panel->ApplyPanelHover(false);
	}

	if (!_hoverPath2dNextSharedScratch.empty())
	{
		auto& leaf = _hoverPath2dNextSharedScratch.back();
		leaf->ApplyHoverPoint(leaf->GlobalToLocal(_cursorPos));
	}
	for (const auto& element : _hoverPath2dNextSharedScratch)
		if (auto panel = std::dynamic_pointer_cast<GuiPanel>(element))
			panel->ApplyPanelHover(true);
}

void Scene::_LockHoverPath(const std::vector<std::weak_ptr<base::GuiElement>>& path,
	std::vector<std::shared_ptr<base::GuiElement>>& outPath) const
{
	outPath.clear();
	outPath.reserve(path.size());

	for (const auto& element : path)
	{
		auto locked = element.lock();
		if (!locked)
			break;

		outPath.push_back(std::move(locked));
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
	_loopEditor.SetSize(_sizeParams.Size);
	_tempoPrompt.FitToViewport(_sizeParams.Size);
	_runtimeDialogs.FitToViewport(_sizeParams.Size);
	if (_mainPanel) _mainPanel->SetViewportSize(_sizeParams.Size);
	if (_selectionPanel) _selectionPanel->SetViewportSize(_sizeParams.Size);
	auto ar = _sizeParams.Size.Height > 0 ?
		(float)_sizeParams.Size.Width / (float)_sizeParams.Size.Height :
		1.0f;
	auto projection = _camera.Projection(ar, _StationCentre(_stations));
	_viewProj = projection * _camera.ViewMatrix();
	_compositor.Resize(_sizeParams.Size);

	if (_hudPanel)
	{
		if (_selectionPanel) _hudPanel->SetTopPanelLayout(_selectionPanel->TopPanelLayout());
		_hudPanel->SetSize(_sizeParams.Size);
		if (_mainPanel) _mainPanel->SetOccupiedTopBounds(
			_selectionPanel ? _selectionPanel->TopPanelLayout().PresentedBounds : Rect2d{}, _hudPanel->OccupiedTopStripBounds());
	}

	_UpdateHudStationAnchors();
}

void Scene::_OnLoopGridEditorOpened()
{
	_backgroundGesture.reset();
	_ReleaseQuantisationInput();
	if (_hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		_hudPanel->SetLoopEditorMode(_loopEditor.IsEngaged());
	}
	_focusManager.ClearFocus();
	_touchDownElement.reset();
	_touchDownIsHud = false;
	_touchDownIsSettings = false;
	_consumeSettingsRelease = false;
	_EndBackgroundDrag();
	_quantisationInteraction.OnCtrlModifierChanged(false, Timer::GetTime(),
		_InteractionContext(), [this](const std::vector<unsigned char>& path) { return _ChildFromPath(path); });
}

void Scene::_UpdateViewProjectionLocked()
{
	const float aspect = _sizeParams.Size.Height > 0
		? static_cast<float>(_sizeParams.Size.Width) / static_cast<float>(_sizeParams.Size.Height) : 1.0f;
	_viewProj = _camera.Projection(aspect, _StationCentre(_stations)) * _camera.ViewMatrix();
}

void Scene::_UpdateHudStationAnchors()
{
	if (!_hudPanel || (_sizeParams.Size.Width == 0) || (_sizeParams.Size.Height == 0))
		return;

	std::vector<gui::GuiHud::StationAnchor> anchors;
	anchors.reserve(_stations.size());

	for (size_t stationIndex = 0u; stationIndex < _stations.size(); ++stationIndex)
	{
		const auto& station = _stations[stationIndex];
		const auto modelPos = station->TopCapModelPosition();
		auto clip = _viewProj * glm::vec4(modelPos.X, modelPos.Y, 0.0f, 1.0f);
		anchors.push_back({ stationIndex, station->Name(),
			gui::CableInteraction::ProjectAnchor(clip, _sizeParams.Size), glm::vec4(0.85f, 0.90f, 0.95f, 0.45f) });
	}

	_hudPanel->SetStationAnchors(std::move(anchors));
}

base::Tweakable::TweakState Scene::_SelectionTweakStateLocked(const std::shared_ptr<GuiElement>& target) const
{
	return HierarchySelection::TweakState(HierarchySelection::LockedStations(_stations), target);
}

void Scene::_UpdateRackVisibilityLocked()
{
	const auto depth = _selector->CurrentSelectDepth();
	if (depth == base::DEPTH_LOOPTAKE || depth == base::DEPTH_LOOP)
		for (const auto& station : _stations)
			if (station->LayoutTakeRacks(_viewProj, _sizeParams.Size)) _InvalidateHover2d();

	// Presentation stays on the UI thread under the existing scene lock.
	const auto forEachRack = [this](auto&& visit) {
		for (const auto& station : _stations)
		{
			const auto position = station->PresentationModelPosition();
			const auto clip = _viewProj * glm::vec4(position.X, position.Y, position.Z, 1.0f);
			const bool stationInView = station->IsVisible() && !_loopEditor.IsEngaged() &&
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
		if (!ownerInView || !rack->IsInView(_sizeParams.Size) || expanded)
		{
			rack->SetRackState(GuiRackParams::RACK_MASTER, true);
			_InvalidateHover2d();
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
			_InvalidateHover2d();
		}
	});
}

void Scene::_CollapseRacksLocked()
{
	const auto collapse = [](const std::shared_ptr<GuiRack>& rack) {
		if (!rack) return;
		rack->SetRackState(GuiRackParams::RACK_MASTER, true);
		rack->SetMasterControlsVisible(true);
	};
	for (const auto& station : _stations)
	{
		collapse(station->GetGuiRack());
		for (const auto& take : station->GetLoopTakes()) collapse(take->GetGuiRack());
	}
	_InvalidateHover2d();
}

void Scene::_UpdateSelection(ActionResultType res)
{
	std::scoped_lock lock(_sceneMutex);
	const HierarchySelection::LockedStations view(_stations);
	const auto update = _selector->SelectionChanges(res);
	if (update.ClearSelection)
	{
		_CollapseRacksLocked();
		HierarchySelection::Clear(view);
	}
	for (std::size_t index = 0u; index < update.ChangeCount; ++index)
	{
		const auto& change = update.Changes[index];
		using Edit = SceneSelector::HierarchyEdit;
		if (change.Edit == Edit::Select || change.Edit == Edit::Deselect)
		{
			if (auto target = HierarchySelection::Resolve(view, change.Path))
			{
				_CollapseRacksLocked();
				HierarchySelection::SetSelected(view, target, _selector->CurrentSelectDepth(), change.Edit == Edit::Select);
			}
		}
		else HierarchySelection::SetMuted(view, change.Path, change.Edit == Edit::Mute);
	}
	HierarchySelection::ApplyAppearance(view, { update.HoverPath, update.ClearPicking, update.HighlightHover,
		update.HighlightMidiSiblings, update.ClickPressed, update.MutePressed });
	// The picker may keep the same ID after a click or paint stroke, so update
	// the selector's cached starting state without waiting for another pick.
	if (auto hovered = _ChildFromPathLocked(_selector->CurrentHover()))
	{
		const auto tweakState = _SelectionTweakStateLocked(hovered);
		_selector->UpdateCurrentHover(_selector->CurrentHover(),
			Action::MODIFIER_NONE, hovered->IsSelected(), tweakState);
	}

	_quantisationInteraction.RefreshOverlay(_InteractionContextLocked(),
		[this](const std::vector<unsigned char>& path) { return _ChildFromPathLocked(path); });
}

void Scene::InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	ResourceUser::InitResources(resourceLib, forceInit);
	_runtimeRendererReady = _runtimeDialogs.InitResources(resourceLib,forceInit);
	if (!_runtimeRendererReady) _runtimeDialogs.Pump(_popupManager,_sizeParams.Size,false);
	// Initialize controls added by rig edits on the render thread before drawing them.
	{
		std::scoped_lock lock(_sceneMutex);
		if (_stationManagement.Added) _stationManagement.Added->InitResources(resourceLib, false);
		if (_stationDeleteDialog) _stationDeleteDialog->InitResources(resourceLib, false);
		if (_takeDitchDialog) _takeDitchDialog->InitResources(resourceLib, false);
		// Motion and camera updates precede this one control layout/init pass.
		_UpdateViewProjectionLocked();
		const auto depth = _selector->CurrentSelectDepth();
		const bool stationDitchVisible = depth == base::DEPTH_STATION;
		const bool takeDitchVisible = depth == base::DEPTH_LOOPTAKE || depth == base::DEPTH_LOOP;
		_stationControls.Update(_stations, _viewProj, _sizeParams.Size, _ctrlHeld, stationDitchVisible, takeDitchVisible,
			_StationManagementAvailableLocked());
		_stationControls.InitResources(resourceLib);
		if (_hudPanel)
			_hudPanel->InitResources(resourceLib, false);
	}

	// Stations can be added after scene resources are initialised.
	auto stations = SnapshotStations();
	for (auto& station : stations)
	{
		if (station)
			station->InitResources(resourceLib, forceInit);
	}
}

Position3d Scene::_StationCentre(const std::vector<std::shared_ptr<Station>>& stations)
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

void Scene::_ApplyCameraSelectDepthChange(graphics::Camera::SelectDepthChange change)
{
	if (graphics::Camera::SelectDepthChange::None == change)
		return;
	_viewMode = (graphics::Camera::SelectDepthChange::Station == change) ? VIEW_STATION : VIEW_LOOPTAKE;
	_settingsControls->Mode()->SetCurrentValue(static_cast<unsigned int>(_viewMode), true);
	_UpdateSelectDepth(static_cast<unsigned int>(_viewMode));
}

void Scene::_CycleCameraView()
{
	const auto stations = SnapshotStations();
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
	_ApplyCameraSelectDepthChange(_camera.CycleView(_StationCentre(stations), hoveredStation,
		firstStation, hoveredIdentity, stations.empty() ? nullptr : stations.front(),
		VIEW_STATION == _viewMode));
}

std::vector<std::shared_ptr<Station>> Scene::SnapshotStations() const
{
	std::lock_guard<std::mutex> lock(_sceneMutex);
	return _stations;
}

void Scene::_PrepareStation(const std::shared_ptr<Station>& station)
{
	station->SetCameraRecordingPublisher(_cameraRecordings);
	station->SetReceiver(ActionReceiver::shared_from_this());
	station->SetLogging(_loggingConfig);
	station->SetClock(_quantisation.Clock());
	station->SetTransportOffsetLoopFrac(_transportOffsetLoopFrac);
	const auto stream = _audioEngine->GetStreamParams();
	station->SetupBuffers(stream.BufSize ? stream.BufSize : ChannelMixer::DefaultBufferSize);
	station->SetSampleRate(static_cast<float>(_CurrentSampleRate()));
	station->SetNumAdcChannels(_audioEngine->GetChannelMixer()->Source()->NumOutputChannels(Audible::AUDIOSOURCE_ADC));
	station->SetNumDacChannels(_audioEngine->GetChannelMixer()->Sink()->NumInputChannels(Audible::AUDIOSOURCE_LOOPS));
	station->Init();
	if (auto label = station->GetGuiRack()->StationLabel())
	{
		label->Focus = [this, weakLabel = std::weak_ptr(label)](const auto& editor) {
			if (editor) _focusManager.RequestFocus(editor);
			else if (auto owner = weakLabel.lock(); owner && owner->OwnsFocus(_focusManager.CurrentFocus())) _focusManager.ClearFocus();
		};
		label->Submit = [this, target = std::weak_ptr(station)](const auto& name, auto revision) {
			std::scoped_lock lock(_sceneMutex);
			return _RequestStationRenameLocked(target.lock(), revision, name);
		};
	}

	auto selectDepth = (unsigned int)_viewMode;
	if (selectDepth > (unsigned int)DEPTH_LOOP)
		selectDepth = (unsigned int)DEPTH_LOOP;

	station->SetSelectDepth((SelectDepth)selectDepth);
	station->SetGlobalMidiQuantState(_globalMidiQuantState);
	_quantisation.InitialiseStationMidiGrid(station);

}

void Scene::_AddStation(std::shared_ptr<Station> station, bool publishAudioStations)
{
	_PrepareStation(station);
	{
		std::lock_guard<std::mutex> lock(_sceneMutex);
		_stations.push_back(station);
		_camera.RegisterStation(_stations.size() - 1u, station, station->LoopTakeRevision());
		if (publishAudioStations)
			_PublishAudioStations();
	}
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

void Scene::_SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState state, bool fromLocalEdit)
{
	if ((_globalMidiQuantState == state) && !fromLocalEdit)
		return;

	_globalMidiQuantState = state;
	if (_settingsControls->GlobalQuantisation())
		_settingsControls->GlobalQuantisation()->SetCurrentValue(static_cast<unsigned int>(state), true);

	_ApplyGlobalMidiQuantStateToAllLoopTakes();
}

void Scene::_SetTransportOffsetLoopFrac(double loopFrac, bool updateInput)
{
	loopFrac = std::isfinite(loopFrac) ? std::clamp(loopFrac, -1.0, 1.0) : 0.0;
	const auto previousLoopFrac = _transportOffsetLoopFrac;
	_transportOffsetLoopFrac = loopFrac;

	for (auto& station : SnapshotStations())
	{
		if (station)
			station->SetTransportOffsetLoopFrac(loopFrac);
	}
	if (_audioEngine && previousLoopFrac != loopFrac)
		_audioEngine->PublishLocalTransportOffsetLoopFrac(loopFrac);

	if (updateInput && _settingsControls->Offset())
		_settingsControls->Offset()->SetValue(loopFrac, false);
}

void Scene::_ApplyGlobalMidiQuantStateToAllLoopTakes()
{
	for (auto& station : SnapshotStations())
	{
		if (station)
			station->SetGlobalMidiQuantState(_globalMidiQuantState);
	}
}

void Scene::_ForceGlobalMidiQuantStateMixedOnLocalEdit()
{
	_SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed, true);
}

void Scene::_UpdateRemoteStationsFromSnapshot(const NinjamRemoteSnapshot& snapshot)
{
	if (_stationManagement.Busy()) { _deferredRemoteSnapshot = snapshot; return; }
	if (_networkService->UpdateRemoteStationsFromSnapshot(snapshot, _stations))
		_PublishAudioStations();
}

bool Scene::_IsMidiPhaseDragModifier(base::Action::Modifiers modifiers) const noexcept
{
	return (Action::MODIFIER_CTRL & modifiers);
}

QuantisationInteractionContext Scene::_InteractionContext() const
{
	std::scoped_lock lock(_sceneMutex);
	return _InteractionContextLocked();
}

QuantisationInteractionContext Scene::_InteractionContextLocked() const
{
	QuantisationInteractionContext context;
	context.CursorPos = _cursorPos;
	context.ViewportSize = _sizeParams.Size;
	context.SelectDepth = _selector->CurrentSelectDepth();
	context.HoverPath = _selector->CurrentHover();
	context.HoverPath3d = _hoverPath3d;
	context.SampleRate = _CurrentSampleRate();
	if (context.SelectDepth == base::DEPTH_LOOP)
	{
		bool anySelected = false;
		auto hovered = _ChildFromPathLocked(context.HoverPath);
		if (!hovered) hovered = _ChildFromPathLocked(context.HoverPath3d);
		for (const auto& station : _stations)
			if (station && !station->IsRemote())
				for (const auto& take : station->GetLoopTakes())
				{
					for (const auto& loop : take->GetLoops())
						if (loop)
						{
							anySelected |= loop->IsSelected();
							if (loop->IsSelected() || loop == hovered) context.LoopDepthHasAudioTarget = true;
						}
					for (const auto& loop : take->GetMidiLoops())
						if (auto model = loop->Model())
						{
							if (model->IsSelected()) { context.SelectedMidiLoops.push_back(loop); anySelected = true; }
							if (model == hovered) context.HoveredMidiLoop = loop;
						}
				}
		if (anySelected) context.HoveredMidiLoop.reset();
	}
	return context;
}

ActionResult Scene::_BeginBackgroundDrag(actions::TouchAction action)
{
	// A background click only deselects. Activity begins when it actually pans.
	_cameraPointerPreservesIdle = true;
	auto res = _camera.HandleBackgroundDrag(action);
	_isSceneTouching = _camera.IsBackgroundDragging();
	return res;
}

ActionResult Scene::_UpdateBackgroundDrag(actions::TouchMoveAction action)
{
	auto res = _camera.UpdateBackgroundDrag(action);
	if (_camera.BackgroundDragWasDragged()) _cameraPointerPreservesIdle = false;
	if (!_camera.IsBackgroundDragging())
		_EndBackgroundDrag();

	return res;
}

void Scene::_EndBackgroundDrag()
{
	_isSceneTouching = false;
}

void Scene::_ClearTimingState(bool clearTapTempo)
{
	const auto hasConnectedTiming = _networkService->HasConnectedTiming();
	_quantisation.Clear(clearTapTempo, hasConnectedTiming);
	if (!hasConnectedTiming)
		_quantisation.SetMidiGrain(0u, "timing clear", _stations);
}

void Scene::_HandleAudioLocalContentState(bool hasLocalContent)
{
	if (hasLocalContent)
		return;

	// A connected empty scene still follows the accepted remote transport. Keep
	// the edge armed so a later physical-loss visit can clear local timing once.
	if (_networkService->HasConnectedTiming())
	{
		_isSceneReset.store(false, std::memory_order_relaxed);
		return;
	}

	if (_isSceneReset.exchange(true, std::memory_order_relaxed))
		return;

	// No local takes remain, so there is no station hierarchy to update. Keep
	// the destructive Quantiser cleanup on this job-owned edge and off OnTick.
	if (auto clock = _quantisation.Clock())
		std::cout << "MIDI timing reset: reason=empty-local clockLength="
			<< clock->SeedSourceLength() << " scene=" << clock->SceneSamplePos() << '\n';
	_quantisation.Clear(false);
}

void Scene::_ResetIfEmpty()
{
	if (_isSceneReset.load(std::memory_order_relaxed))
		return;
	for (const auto& station : _stations)
	{
		if (station && !station->IsRemote()
			&& !station->GetLoopTakeSnapshot().empty())
		{
			return;
		}
	}

	if (_networkService->HasConnectedTiming())
	{
		_isSceneReset.store(false, std::memory_order_relaxed);
		return;
	}
	Reset();
}

void Scene::_FailJobThread(const char* stage, const char* detail, const actions::JobAction* job) noexcept
{
	// Only the job owner writes the record. UI owns device teardown and joining.
	_sessionReplacing.store(true, std::memory_order_release);
	if (JobThreadFailed()) return;
	int formatted = std::snprintf(_jobThreadFailureDetail.data(), _jobThreadFailureDetail.size(),
		"[ERROR][job] stage=%s; worker stopped; shutdown requested; exception=%s", stage, detail);
	if (job)
	{
		try
		{
			const auto path = utils::EncodeUtf8(job->VstPath);
			formatted = std::snprintf(_jobThreadFailureDetail.data(), _jobThreadFailureDetail.size(),
				"[ERROR][job] stage=%s action=%d plugin='%s'; worker stopped; shutdown requested; exception=%s",
				stage, static_cast<int>(job->JobActionType), path.c_str(), detail);
		}
		catch (...) {} // Keep the bounded stage/detail record if path conversion fails.
	}
	if (formatted < 0 || static_cast<std::size_t>(formatted) >= _jobThreadFailureDetail.size())
	{
		constexpr std::string_view marker = " [detail truncated]";
		auto end = _jobThreadFailureDetail.size() - 1 - marker.size();
		while (end && (static_cast<unsigned char>(_jobThreadFailureDetail[end]) & 0xc0) == 0x80) --end;
		std::copy(marker.begin(), marker.end(), _jobThreadFailureDetail.data() + end);
		_jobThreadFailureDetail[end + marker.size()] = 0;
	}
	// Publish after all writes; this record never changes and survives ring loss/join.
	_jobThreadFailed.store(true, std::memory_order_release);
	try
	{
		std::optional<console::DiagnosticThread::StartupScope> origin;
		if (job) origin.emplace(job->StartupDiagnostic);
		std::cerr << _jobThreadFailureDetail.data() << std::endl;
	}
	catch (...) {} // The owner's modal/direct-file fallback never uses this stream.
}

void Scene::_JobLoop(std::function<void()> onStart) noexcept
{
	const char* stage = "initialize";
	try
	{
		if (onStart) onStart();
		stage = "pump";
		while (!_isSceneQuitting.load(std::memory_order_acquire) && !JobThreadFailed())
		{
			OnJobTick(Timer::GetTime());
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		if (!JobThreadFailed())
		{
			stage = "final-save";
			std::function<void()> save;
			{ std::scoped_lock lock(_sceneMutex); save = std::move(_explicitRigSaveJob); }
			if (save) save();
		}
	}
	catch (const std::exception& error) { _FailJobThread(stage, error.what()); }
	catch (...) { _FailJobThread(stage, "unknown exception"); }
	// A failed worker cannot pump structural work. Wait only for UI Shutdown's
	// permanent input-close publication, then preserve the final producer barrier.
	while (!_isSceneQuitting.load(std::memory_order_acquire))
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	_inputSubsystem->AcknowledgeRigTriggerInputCloseFromJob();
}

void Scene::_PublishAudioStations()
{
	_performanceGroups.CancelPending();
	_audioEngine->PublishGroupMuteApplication(nullptr);
	auto stations = std::make_shared<const std::vector<std::shared_ptr<Station>>>(_stations.begin(), _stations.end());
	_audioEngine->SetStations(stations, _rigCoordinator.Accepted() ? _rigCoordinator.Accepted()->TopologyRevision : 0u);
}

GroupTargetKind Scene::_GroupSelectionDepthLocked() const
{
	const auto depth = _selector->CurrentSelectDepth();
	return depth == base::DEPTH_STATION ? GroupTargetKind::Station :
		depth == base::DEPTH_LOOP ? GroupTargetKind::AudioLoop : GroupTargetKind::Take;
}

void Scene::_RestoreGroupSelectionDepthLocked()
{
	const auto active = _performanceGroups.Groups().Selection.ActiveSlot();
	if (!active) return;
	const auto snapshot = _performanceGroups.Groups().Selection.Get(*active);
	if (!snapshot) return;
	const auto depth = snapshot->Depth == GroupTargetKind::Station ? base::DEPTH_STATION :
		snapshot->Depth == GroupTargetKind::Take ? base::DEPTH_LOOPTAKE : base::DEPTH_LOOP;
	_selector->SetSelectDepth(depth);
	for (const auto& station : _stations) station->SetSelectDepth(depth);
}

void Scene::_ExecuteGroupCommandLocked(bool selection, GroupCommand command, std::size_t slot)
{
	_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
	_performanceGroups.Execute(selection, command, slot, _stations, _GroupSelectionDepthLocked(),
		[this](auto application) { _PublishGroupMuteApplication(std::move(application)); });
	if (selection && command == GroupCommand::RecallOrStore) _RestoreGroupSelectionDepthLocked();
	if (_selectionPanel) _selectionPanel->RefreshPerformanceGroups(_performanceGroups.Groups());
	_InvalidateHover2d();
}

std::shared_ptr<GuiElement> Scene::_ChildFromPath(std::vector<unsigned char> path)
{
	std::scoped_lock lock(_sceneMutex);
	return _ChildFromPathLocked(path);
}

std::shared_ptr<GuiElement> Scene::_ChildFromPathLocked(const std::vector<unsigned char>& path) const
{
	return HierarchySelection::Resolve(HierarchySelection::LockedStations(_stations), path);
}

void Scene::_UpdateSelectDepth(unsigned int depth)
{
	if (depth > (unsigned int)DEPTH_LOOP)
		depth = (unsigned int)DEPTH_LOOP;

	auto selectDepth = (SelectDepth)depth;
	_selector->SetSelectDepth(selectDepth);

	for (auto& station : SnapshotStations())
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
	const auto streamRate = _audioEngine->GetStreamParams().SampleRate;
	if (streamRate > 0u)
		return streamRate;

	if (_userConfig.Audio.SampleRate > 0u)
		return _userConfig.Audio.SampleRate;

	return constants::DefaultSampleRate;
}

std::uint64_t Scene::_EstimatedAudioSampleAt(Time actionTime) const
{
	const auto sampleRate = _CurrentSampleRate();
	const auto anchor = midi::ReadMidiClockAnchor(_audioEngine->GetMidiClockAnchor(), {});
	const auto actionMicros = std::chrono::duration_cast<std::chrono::microseconds>(
		actionTime.time_since_epoch()).count();

	return MapMidiTimestampToAudioSample(sampleRate,
		anchor.Sample,
		anchor.SteadyMicros,
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
	std::scoped_lock lock(_sceneMutex);
	const auto handled = _quantisation.HandleTapTempo(_EstimatedAudioSampleAt(actionTime),
		_CurrentSampleRate(),
		_stations,
		_userConfig);
	if (handled)
	{
		_quantisation.UpdateStationHints(nullptr, _selector->CurrentSelectDepth(), false, _stations);
		for (const auto& station : _stations)
			if (station && !station->IsRemote())
				for (const auto& take : station->GetLoopTakes())
					if (take && _settingsControls->Subdivision())
					{
						_settingsControls->Subdivision()->SetCurrentValue(
							midi::MidiQuantisation::FractionDisplayIndex(take->MidiQuantisation().Fraction), true);
						return handled;
					}
	}
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

bool Scene::HasSelection() const
{
	std::scoped_lock lock(_sceneMutex);
	return _HasSelection();
}

bool Scene::_HasSelection() const
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
			for (const auto& loop : take->GetMidiLoops())
			{
				if (loop && loop->Model() && loop->Model()->IsSelected())
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
	std::scoped_lock lock(_sceneMutex);
	return _quantisation.TrySetMasterFromHover(_ChildFromPathLocked(_selector->CurrentHover()),
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
	std::scoped_lock lock(_sceneMutex);
	_quantisation.UpdateStationHints(candidate, depth, confirmCandidate, _stations);
}

void Scene::_ClearStationQuantisation()
{
	_quantisation.ClearStationHints(_stations);
}

void Scene::SetRelativePointerHost(std::function<bool(int, utils::Position2d)> begin,
	std::function<void(int)> end, std::function<float()> logicalPixelScale)
{
	_ReleaseQuantisationInput();
	_loopEditor.CancelInput();
	_beginRelativePointer = std::move(begin);
	_endRelativePointer = std::move(end);
	_logicalPixelScale = std::move(logicalPixelScale);
}

bool Scene::_EditControlsSuppressed() const
{
	return _quantisationInteraction.EditModeActive() || _quantisationInteraction.PanelAlpha() > 0.001f;
}

std::optional<ActionResult> Scene::_RouteQuantisationTouch(TouchAction action)
{
	std::scoped_lock lock(_sceneMutex);
	return _quantisationInteraction.TryHandleTouchAction(action, _CurrentSampleRate(),
		_ctrlHeld, _InteractionContextLocked(), [this](const auto& path) { return _ChildFromPathLocked(path); });
}

std::optional<ActionResult> Scene::_RouteQuantisationMove(TouchMoveAction action)
{
	std::scoped_lock lock(_sceneMutex);
	return _quantisationInteraction.TryHandleTouchMove(action, _CurrentSampleRate());
}

void Scene::_ReleaseQuantisationInput()
{
	std::scoped_lock lock(_sceneMutex);
	_ReleaseQuantisationInputLocked();
}

void Scene::_ReleaseQuantisationInputLocked()
{
	_backgroundGesture.reset();
	_spaceHeld = _ctrlHeld = false;
	_quantisation.SetOverlayHeld(false);
	_quantisationInteraction.CancelInteraction(true);
	_quantisationInteraction.OnCtrlModifierChanged(false, Timer::GetTime(),
		_InteractionContextLocked(), [this](const auto& path) { return _ChildFromPathLocked(path); });
}

void Scene::OnInputFocusLost()
{
	const bool leftWindowsHeld = _learnKeysDown[VK_LWIN], rightWindowsHeld = _learnKeysDown[VK_RWIN];
	_learnKeysDown.fill(false);
	// Windows modifiers have no Action flag. Require their observed release,
	// including after focus loss, before admitting a plain learned key.
	_learnKeysDown[VK_LWIN] = leftWindowsHeld; _learnKeysDown[VK_RWIN] = rightWindowsHeld;
	_contextualKeyboard.FocusLost();
	if (const auto rig = _rigCoordinator.Accepted(); rig && _inputSubsystem->TryAcceptUiRigTriggerInput(rig->Revision))
	{
		base::Action release;
		release.SetActionTime(Timer::GetTime());
		_contextualKeyboard.Flush(*rig, release);
	}
	// Window-owned focus finalization synchronously notifies the settings owner,
	// which takes its own scene lock (e.g. SnapshotStations). Commit before locking.
	_focusManager.ClearFocus();
	std::scoped_lock lock(_sceneMutex);
	if (auto popup = _popupManager.Top()) popup->ClearPointerState();
	_stationControls.Cancel();
	for (const auto key : {VK_INSERT, VK_PRIOR, VK_NEXT})
	{
		actions::KeyAction release; release.KeyChar = key; release.KeyActionType = KeyAction::KEY_UP;
		if (key == VK_INSERT) _inputSubsystem->HandleAutomationKey(release, _stations, {}, {});
		else _inputSubsystem->HandleChannelOverrideKey(release, _stations);
	}
	_performanceGroups.ResetKeys();
	_ReleaseQuantisationInputLocked();
	if (_hudPanel) _hudPanel->ClearPointerState();
	_touchDownElement.reset();
	_touchDownIsHud = _touchDownIsSettings = false;
	_consumeSettingsRelease = false;
	_loopEditor.CancelInput();
	_ApplyHoverPath2d({});
	_InvalidateHover2d();
}

bool Scene::_SessionHasTakes() const
{
	std::scoped_lock lock(_sceneMutex);
	for (const auto& station : _stations)
		if (station && !station->GetLoopTakeSnapshot().empty()) return true;
	return false;
}

bool Scene::_CaptureIsBusy() const
{
	std::scoped_lock lock(_sceneMutex);
	if (_stationManagement.Busy() || _rigSaveDialogOpen || _rigSaveInProgress) return true;
	const auto rig = AcceptedRigSnapshot();
	if (rig) for (const auto& runtime : rig->Triggers)
		if (runtime.Instance && !runtime.Instance->CanApplyCaptureRouting()) return true;
	for (const auto& station : _stations)
		if (station && !station->IsRemote()) for (const auto& take : station->GetLoopTakeSnapshot())
			if (take && take->HasPendingTriggerCapture()) return true;
	return false;
}

bool Scene::_ValidateAudioDraft(std::string& error) const
{
	const auto found = std::find_if(_deviceInventory.Devices.begin(), _deviceInventory.Devices.end(),
		[this](const auto& device) { return device.Name == _audioDraft.Name; });
	if (found == _deviceInventory.Devices.end() || !found->Probed) { error = "Selected audio device is unavailable"; return _audioStartRequested; }
	if ((!_audioDraft.NumChannelsIn && !_audioDraft.NumChannelsOut) || _audioDraft.NumChannelsIn > found->InputChannels || _audioDraft.NumChannelsOut > found->OutputChannels) { error = "Invalid input/output channel counts"; return false; }
	if (!_audioDraft.BufSize || !_audioDraft.SampleRate || std::find(found->SampleRates.begin(), found->SampleRates.end(), _audioDraft.SampleRate) == found->SampleRates.end()) { error = "Unsupported sample rate or invalid buffer request"; return false; }
	const auto actual = _audioEngine->GetStreamParams();
	if (_SessionHasTakes() && _audioDraft.SampleRate != (actual.SampleRate ? actual.SampleRate : _userConfig.Audio.SampleRate)) { error = "Sample rate cannot change while any takes exist"; return false; }
	return true;
}

void Scene::_SetAudioDraftFromStream(const audio::AudioStreamParams& params)
{
	// Startup may resolve a legacy/default request or negotiate stream geometry.
	// Show the running configuration without rewriting the accepted rig on disk.
	_audioDraft.Name = params.Name;
	_audioDraft.SampleRate = params.SampleRate;
	_audioDraft.NumChannelsIn = params.NumInputChannels;
	_audioDraft.NumChannelsOut = params.NumOutputChannels;
	_audioDraft.BufSize = params.BufSize;
	_audioDraft.NumBuffers = params.NumBuffers;
	_devicePageDirty = true;
}

void Scene::_RebuildDevicePage()
{
	_mainPanel->FinalizePageSettings(SettingsPage::Audio);
	const auto midi = _midiState.load(std::memory_order_acquire);
	const auto pages = _settingsControls->BuildDevicePages(_deviceInventory, _audioDraft,
		_acceptedBufferSizes, midi->Connection, _midiDraft, _popupManager, ActionReceiver::shared_from_this());
	_mainPanel->ReplacePageSettings(SettingsPage::Audio, pages.Audio);
	_mainPanel->ReplacePageSettings(SettingsPage::Midi, pages.Midi);
	_displayedMidiState = midi; _devicePageDirty = false;
}

void Scene::ReportFileStatus(const std::string& status)
{
	_settingsControls->SetFileStatus(status);
	std::cout << "[SESSION scene=" << _sceneIdentity << " operation=" << _controlOperationId << "] " << status << std::endl;
}

void Scene::_RequestSave(bool chooseFolder, bool asDefault)
{
	if (_controlOperation != ControlOperation::None || !_rigCoordinator.EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	if (_CaptureIsBusy()) { ReportFileOutcome("Save unavailable during capture or pending actions"); return; }
	if (asDefault && (_defaultJamPath.empty() || !_defaultJamSaver)) { ReportFileOutcome("Startup jam path unavailable"); return; }
	_controlOperation = ControlOperation::Save;
	auto folder = asDefault && _currentJamPath.empty() ? utils::GetParentDirectory(_defaultJamPath) : _sessionSaveFolder;
	if (chooseFolder || folder.empty()) folder = utils::PickDirectory(L"Choose session save folder");
	if (folder.empty()) { _controlOperation = ControlOperation::None; return; }
	_pendingSaveAsDefault = asDefault; _pendingSaveFolder = std::move(folder); _controlOperation = ControlOperation::Save;
	ReportFileStatus("Waiting to capture session");
}

bool Scene::ConsumePanelLoadRequest()
{
	return std::exchange(_panelLoadRequested, false);
}

void Scene::_CompleteSessionSave(bool success, const std::wstring& path, const std::string& error)
{
	if (!success) { ReportFileOutcome(error); return; }
	_sessionSaveFolder = _pendingSaveFolder;
	// The manifest is already committed; updating the startup default is a
	// separate operation and cannot revoke this session's successful destination.
	_currentJamPath = path;
	if (_pendingSaveAsDefault)
	{
		if (_defaultJamSaver(path, true)) ReportFileOutcome("Saved startup jam " + utils::EncodeUtf8(path));
		else ReportFileOutcome("Jam saved at " + utils::EncodeUtf8(path) + "; startup default unchanged");
	}
	else ReportFileOutcome("Saved " + utils::EncodeUtf8(path));
}

void Scene::PickSessionForLoad()
{
	if (_controlOperation != ControlOperation::None || !_rigCoordinator.EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	if (_CaptureIsBusy()) { ReportFileOutcome("Load unavailable during capture or pending actions"); return; }
	_controlOperation = ControlOperation::Load;
	try { _pendingLoadPath = utils::PickJamFile(); }
	catch (...) { _controlOperation = ControlOperation::None; throw; }
	if (_pendingLoadPath.empty()) _controlOperation = ControlOperation::None;
}

void Scene::RequestSessionLoad(std::wstring path)
{
	if (path.empty()) return;
	if (_controlOperation != ControlOperation::None || !_rigCoordinator.EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	if (_CaptureIsBusy()) { ReportFileOutcome("Load unavailable during capture or pending actions"); return; }
	_pendingLoadPath = std::move(path); _controlOperation = ControlOperation::Load;
}

std::wstring Scene::TakeReadySessionLoad()
{
	return _loadReady ? std::exchange(_pendingLoadPath, {}) : std::wstring();
}

bool Scene::_FinishControlOperation(std::string* restorationError)
{
	bool restored = true;
	if (_controlOperation == ControlOperation::Audio) _audioSchedule.NextChange = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
	std::string error;
	if (_controlPaused && !_audioEngine->EndQuiescence(_controlPause, error))
	{
		restored = false;
		if (restorationError) *restorationError = error;
		_settingsControls->SetDeviceStatus("Stopped: " + error); ReportFileOutcome("Audio restoration failed: " + error);
	}
	if (_controlPaused && restored && _controlPause.AdmissionWasOpen)
	{
		std::scoped_lock lock(_sceneMutex);
		_sourceLossRecoveryGateClosed = false;
	}
	_controlPaused = false;
	try { _inputSubsystem->ResumeMidiDispatch(); }
	catch (const std::exception& dispatchError)
	{
		restored = false;
		if (restorationError) *restorationError += "; MIDI resume failed: " + std::string(dispatchError.what());
		_settingsControls->SetDeviceStatus("MIDI dispatch stopped: " + std::string(dispatchError.what()));
		ReportRuntimeNotice("MIDI", "MIDI dispatch stopped: " + std::string(dispatchError.what()));
	}
	if (_controlInputClosed)
	{
		const auto accepted = AcceptedRigSnapshot();
		if (accepted) _inputSubsystem->OpenRigTriggerInput(accepted->Revision);
	}
	_controlInputClosed = false;
	if (_controlWriterExclusion.owns_lock()) _controlWriterExclusion.unlock();
	_controlOperation = ControlOperation::None; _loadReady = false;
	_controlAdmissionPending.store(false, std::memory_order_release);
	return restored;
}

void Scene::FinishSessionLoad(bool replaced, const std::string& error)
{
	if (replaced)
	{
		_sessionReplacing.store(true, std::memory_order_release);
		_controlPaused = false; _loadReady = false;
		if (_controlWriterExclusion.owns_lock()) _controlWriterExclusion.unlock();
		_controlOperation = ControlOperation::None;
	}
	else { ReportFileOutcome(error.empty() ? "Load failed; current session retained" : error); _FinishControlOperation(); }
}

void Scene::PumpDeviceAndFileControls()
{
	if (_isSceneQuitting.load(std::memory_order_acquire)) return;
	bool inventoryBusy = false;
	{ std::scoped_lock lock(_sceneMutex); inventoryBusy = _stationManagement.Busy() || _rigSaveDialogOpen || _rigSaveInProgress; }
	const bool filesEnabled = !inventoryBusy && _controlOperation == ControlOperation::None && _rigCoordinator.EditsEnabled();
	_settingsControls->SetSessionEnabled(filesEnabled, filesEnabled && !_defaultJamPath.empty() && static_cast<bool>(_defaultJamSaver));
	const auto now = std::chrono::steady_clock::now();
	const auto actual = _audioEngine->GetStreamParams();
	if (actual.BufSize && !_acceptedBufferSizes.contains(actual.Name)) { _acceptedBufferSizes[actual.Name] = {actual.BufSize}; _devicePageDirty = true; }
	if (_devicePageDirty || _displayedMidiState != _midiState.load(std::memory_order_acquire)) _RebuildDevicePage();
	std::ostringstream text;
	text << actual.Name << " | " << actual.SampleRate << " Hz | " << actual.NumInputChannels << " in / " << actual.NumOutputChannels << " out | buffer " << actual.BufSize;
	_settingsControls->SetActiveAudioStatus(text.str());
	if (_controlOperation == ControlOperation::None)
	{
		if (inventoryBusy || !_rigCoordinator.EditsEnabled()) return;
		if (_midiDraftPending)
		{
			_controlOperation = ControlOperation::Midi; _midiDraftPending = false;
			++_controlOperationId;
			_pendingMidiDeviceRequest.store(std::make_shared<const MidiDeviceRequest>(MidiDeviceRequest{_sceneIdentity, _controlOperationId, _midiDraft}), std::memory_order_release);
		}
		else if (_audioSchedule.Revision != _audioSchedule.AttemptedRevision && now >= _audioSchedule.NextChange)
		{
			if (now < _audioSchedule.ChannelDeadline) { _settingsControls->SetDeviceStatus("Settling channel counts (3 seconds)"); return; }
			if (_CaptureIsBusy()) { _settingsControls->SetDeviceStatus("Waiting for recording/pending capture"); return; }
			if (!_audioStartRequested && _audioDraft.Name == actual.Name && _audioDraft.SampleRate == actual.SampleRate && _audioDraft.BufSize == actual.BufSize && _audioDraft.NumChannelsIn == actual.NumInputChannels && _audioDraft.NumChannelsOut == actual.NumOutputChannels) { _audioSchedule.AttemptedRevision = _audioSchedule.Revision; return; }
			std::string error;
			if (!_ValidateAudioDraft(error)) { _settingsControls->SetDeviceStatus(error); ReportRuntimeNotice("Audio device",error); _audioSchedule.AttemptedRevision = _audioSchedule.Revision; return; }
			_controlOperation = ControlOperation::Audio;
		}
		else return;
	}
	if (_controlOperation == ControlOperation::Midi)
	{
		if (const auto completion = _completedMidiDeviceRequest.exchange({}, std::memory_order_acq_rel))
		{
			if (completion->SceneId != _sceneIdentity || completion->OperationId != _controlOperationId) return;
			if (completion->Accepted)
			{
				if (completion->PreferencesSaved) { _pendingAudioPersistence.reset(); _pendingMidiPersistence.reset(); }
				else _pendingMidiPersistence = completion->Desired;
			}
			_hardwarePersistencePending = _pendingAudioPersistence.has_value() || _pendingMidiPersistence.has_value();
			_settingsControls->SetDeviceStatus(completion->Status); _controlOperation = ControlOperation::None; _devicePageDirty = true;
			ReportRuntimeNotice("MIDI devices",completion->Status);
		}
		return;
	}
	if (_loadReady) return;
	if (_controlOperation == ControlOperation::Audio && now < _audioSchedule.ChannelDeadline) { _FinishControlOperation(); return; }
	if (!_controlInputClosed)
	{
		const auto accepted = AcceptedRigSnapshot();
		if (!accepted || !_rigCoordinator.EditsEnabled()) return;
		_controlRevision = accepted->Revision;
		if (!_inputSubsystem->RequestCloseRigTriggerInputFromUi(_controlRevision)) return;
		_controlInputClosed = true; _controlAdmissionPending.store(true, std::memory_order_release); ++_controlOperationId;
		return; // Job acknowledges on its next turn; never wait under an application lock.
	}
	if (!_inputSubsystem->RigTriggerInputReadyForAudioBoundary(_controlRevision)) return;
	_controlWriterExclusion = std::unique_lock<std::mutex>(_controlLifecycleMutex, std::try_to_lock);
	if (!_controlWriterExclusion.owns_lock()) return;
	try
	{
	std::string error;
	if (_controlOperation == ControlOperation::Save)
	{
		ReportFileStatus("Capturing/saving session");
		const auto result = io::IoSessionExporter::ExportSessionVersionToDirectory(_stations, AcceptedRigSnapshot(), _quantisation, _globalMidiQuantState, _transportOffsetLoopFrac, _userConfig, actual, _audioEngine.get(), _sceneMutex, _networkService->GetController(), _pendingSaveFolder,
			[this](std::string& restoreError) { return _FinishControlOperation(&restoreError); },
			[this](std::string& eligibilityError)
			{
				_inputSubsystem->SuspendMidiDispatch();
				for (const auto& station : _stations) if (station) station->ResetLiveMidiWhileQuiescent();
				if (_CaptureIsBusy()) { eligibilityError = "Save unavailable during capture or pending actions"; return false; }
				return true;
			}, &_performanceGroups.Groups());
		_CompleteSessionSave(result.Success, result.Path, result.Error);
		if (!result.AudioResumed) ReportFileOutcome("Save outcome: " + utils::EncodeUtf8(result.Path) + "; audio restoration failed: " + result.Error);
		if (_controlOperation != ControlOperation::None) _FinishControlOperation();
		return;
	}
	if (!_audioEngine->BeginQuiescence(_controlPause, error))
	{
		_settingsControls->SetDeviceStatus("Stopped/error: " + error); ReportFileOutcome(error);
		if (_controlOperation == ControlOperation::Audio) _audioSchedule.AttemptedRevision = _audioSchedule.Revision;
		_FinishControlOperation(); return;
	}
	_controlPaused = true;
	_inputSubsystem->SuspendMidiDispatch();
	for (const auto& station : _stations) if (station) station->ResetLiveMidiWhileQuiescent();
	// With producers excluded and callbacks drained, capture-start wins deterministically.
	if (_controlOperation != ControlOperation::Midi && _CaptureIsBusy())
	{
		if (_controlOperation == ControlOperation::Audio) _settingsControls->SetDeviceStatus("Waiting for recording");
		else ReportFileOutcome("File operation rejected during capture/pending actions");
		_FinishControlOperation(); return;
	}
	if (_controlOperation == ControlOperation::Load) { _loadReady = true; ReportFileStatus("Loading session"); return; }

	if (_controlOperation == ControlOperation::Audio)
	{
		const auto admittedRevision = _audioSchedule.Revision;
		const auto requested = _audioDraft;
		_audioSchedule.Attempted(admittedRevision, now);
		if (!_ValidateAudioDraft(error)) { _settingsControls->SetDeviceStatus(error); ReportRuntimeNotice("Audio device",error); }
		else
		{
			_settingsControls->SetDeviceStatus("Changing audio device");
			const auto oldRate = actual.SampleRate;
			const bool applied = _audioEngine->Reconfigure(requested, error);
			const auto acceptedParams = _audioEngine->GetStreamParams();
			if (applied)
			{
				auto settings = requested; settings.Name = acceptedParams.Name; settings.SampleRate = acceptedParams.SampleRate; settings.BufSize = acceptedParams.BufSize; settings.NumBuffers = acceptedParams.NumBuffers; settings.NumChannelsIn = acceptedParams.NumInputChannels; settings.NumChannelsOut = acceptedParams.NumOutputChannels;
				if (_audioStartRequested) { _controlPause.WasRunning = true; _controlPause.AdmissionWasOpen = true; }
				_userConfig.Audio = settings;
				auto& sizes = _acceptedBufferSizes[acceptedParams.Name];
				if (std::find(sizes.begin(), sizes.end(), acceptedParams.BufSize) == sizes.end()) { if (sizes.size() == 16u) sizes.erase(sizes.begin()); sizes.push_back(acceptedParams.BufSize); }
				const auto freshInventory = _audioEngine->GetAsioInventory();
				if (freshInventory.EnumerationError.empty()) _deviceInventory = freshInventory;
				const auto midiState = _midiState.load(std::memory_order_acquire);
				// No capture is active and all producers are excluded at this boundary.
				++_audioStreamEpoch;
				if (const auto currentRig = AcceptedRigSnapshot())
					for (const auto& runtime : currentRig->Triggers) if (runtime.Instance)
						runtime.Instance->ConfirmAudioAvailabilityOnJob(_audioStreamEpoch, acceptedParams.Name, acceptedParams.NumInputChannels);
				// Reprepare timestamp mappers for accepted device-rate geometry.
				const auto refreshedMidi = _inputSubsystem->RefreshMidi(_audioEngine->GetMidiClockAnchor_Ref(), acceptedParams.SampleRate, _inputSubsystem->InventoryMidiInputs());
				const auto newMidiNames = refreshedMidi.ConnectedRegistryIds;
				const auto serialIds = _inputSubsystem->ConnectedSerialDeviceIds();
				_ConfirmMidiConnectionChange(midiState->Connection, refreshedMidi, newMidiNames);
				_midiState.store(std::make_shared<const MidiState>(MidiState{refreshedMidi, newMidiNames, serialIds}), std::memory_order_release);
				_midiActive.store(true, std::memory_order_release);
				if (const auto runtime = _rigCoordinator.RefreshRuntimeAvailability(acceptedParams.NumInputChannels, newMidiNames, serialIds))
				{
					_audioEngine->PublishPendingRigSnapshot(runtime); _audioEngine->ApplyPendingRigWhileQuiescent();
					_inputSubsystem->PublishRigInputDispatch(runtime);
					_hudPanel->PublishRoutingConfig(acceptedParams.NumInputChannels, newMidiNames, runtime);
				}
				if (oldRate != acceptedParams.SampleRate)
				{
					_quantisation.Clear(true); _quantisation.Set(0u, Timer::QUANTISE_OFF);
					_quantisation.SetGlobalPhaseOffsetSamps(0, _stations);
					for (const auto& station : _stations) if (station) station->SetStationPhaseOffsetSamps(0);
					_transportOffsetLoopFrac = 0.0; _audioEngine->PublishLocalTransportOffsetLoopFrac(0.0);
					// Replace pending old-rate authority before admission reopens. The
					// session owner will publish its fresh format epoch on the job turn.
					_ApplyNinjamTimingUpdate(_networkService->ResetTempoSyncOnDisconnect());
				}
				_pendingAudioPersistence = settings;
				const bool merged = _rigCoordinator.MergeHardwarePreferences(settings, std::nullopt);
				const auto rig = AcceptedRigSnapshot(); _hardwarePersistencePending = !merged || !rig || !SaveHardwarePreferences(rig->Rig);
				if (!_hardwarePersistencePending) { _pendingAudioPersistence.reset(); _pendingMidiPersistence.reset(); }
				_settingsControls->SetDeviceStatus(_hardwarePersistencePending ? "Applied, preferences not saved" : (acceptedParams.NumOutputChannels < actual.NumOutputChannels ? "Ready; unavailable output routes disconnected" : "Ready"));
				ReportRuntimeNotice("Audio device",_hardwarePersistencePending ? "Applied, preferences not saved; retry available" :
					(acceptedParams.NumOutputChannels < actual.NumOutputChannels ? "Ready; unavailable output routes disconnected" : "Audio device ready"));
				if (_audioSchedule.Revision == admittedRevision) _audioDraft = settings;
				_devicePageDirty = true;
			}
			else { _settingsControls->SetDeviceStatus(error); ReportRuntimeNotice("Audio device",error); }
			_audioStartRequested = false;
			std::cout << "[DEVICES scene=" << _sceneIdentity << " operation=" << _controlOperationId << "] audio " << (applied ? "applied " : "failed ")
				<< "requested='" << requested.Name << "' in=" << requested.NumChannelsIn << " out=" << requested.NumChannelsOut << " rate=" << requested.SampleRate << " buffer=" << requested.BufSize
				<< " actual='" << acceptedParams.Name << "' in=" << acceptedParams.NumInputChannels << " out=" << acceptedParams.NumOutputChannels << " rate=" << acceptedParams.SampleRate << " buffer=" << acceptedParams.BufSize << " " << error << std::endl;
		}
	}
	if (_controlOperation != ControlOperation::None) _FinishControlOperation();
	}
	catch (const std::exception& error)
	{
		ReportFileOutcome(std::string("Operation failed: ") + error.what());
		if (_controlOperation == ControlOperation::Audio) _audioSchedule.AttemptedRevision = _audioSchedule.Revision;
		_FinishControlOperation();
	}
}


void Scene::_ReconcileMidiDeviceRequestOnJob()
{
	// OnJobTick owns _controlLifecycleMutex; complete values are handed off by
	// UI atomics. Only this job owner mutates MIDI desired selection/endpoints.
	const auto desired = _pendingMidiDeviceRequest.exchange({}, std::memory_order_acq_rel);
	if (!desired) return;
	const auto complete = [this, &desired](std::string status, bool accepted = false, bool saved = false) {
		std::cout << "[MIDI scene=" << desired->SceneId << " operation=" << desired->OperationId << "] " << status << std::endl;
		_completedMidiDeviceRequest.store(std::make_shared<const MidiDeviceCompletion>(MidiDeviceCompletion{
			desired->SceneId, desired->OperationId, std::move(status), accepted, saved, desired->Desired}),
			std::memory_order_release);
	};
	if (_isSceneQuitting.load(std::memory_order_acquire))
	{
		complete("MIDI change cancelled: session is stopping");
		return;
	}
	try
	{
		std::scoped_lock midiLock(_midiLifecycleMutex);
		const auto previous = _midiState.load(std::memory_order_acquire);
		const auto inventory = _inputSubsystem->InventoryMidiInputs();
		auto result = _inputSubsystem->ReconcileMidi(desired->Desired, _audioEngine->GetMidiClockAnchor_Ref(),
			(_audioEngine->GetStreamParams().SampleRate ? _audioEngine->GetStreamParams().SampleRate : _userConfig.Audio.SampleRate), inventory);
		if (!result.Accepted)
		{
			const auto error = result.Error.empty() ? (result.Inventory.Error.empty() ? "MIDI reconciliation failed" : result.Inventory.Error) : result.Error;
			const auto surviving = result.ConnectedRegistryIds;
			const auto serialIds = _inputSubsystem->ConnectedSerialDeviceIds();
			_ConfirmMidiConnectionChange(previous->Connection, result, surviving);
			_midiState.store(std::make_shared<const MidiState>(MidiState{std::move(result), surviving, serialIds}), std::memory_order_release);
			if (surviving != previous->ConnectedIds)
			{
				std::scoped_lock sceneLock(_sceneMutex);
				if (const auto runtime = _rigCoordinator.RefreshRuntimeAvailability(_audioEngine->GetStreamParams().NumInputChannels, surviving, serialIds))
				{ _audioEngine->PublishPendingRigSnapshot(runtime); _inputSubsystem->PublishRigInputDispatch(runtime); _inputSubsystem->OpenRigTriggerInput(runtime->Revision); }
			}
			complete(error);
			return;
		}
		_midiActive.store(true, std::memory_order_release);
		const auto names = result.ConnectedRegistryIds;
		const auto serialIds = _inputSubsystem->ConnectedSerialDeviceIds();
		_ConfirmMidiConnectionChange(previous->Connection, result, names);
		_midiState.store(std::make_shared<const MidiState>(MidiState{std::move(result), names, serialIds}), std::memory_order_release);
		bool merged = false;
		std::shared_ptr<const RigSnapshot> rig;
		{
			std::scoped_lock sceneLock(_sceneMutex);
			merged = _rigCoordinator.MergeHardwarePreferences(std::nullopt, desired->Desired);
			rig = AcceptedRigSnapshot();
		}
		const bool saved = merged && rig && SaveHardwarePreferences(rig->Rig);
		const auto inputs = _audioEngine->GetStreamParams().NumInputChannels;
		{
		std::scoped_lock sceneLock(_sceneMutex);
		if (const auto runtime = _rigCoordinator.RefreshRuntimeAvailability(inputs, names, serialIds))
		{
			_audioEngine->PublishPendingRigSnapshot(runtime);
			_inputSubsystem->PublishRigInputDispatch(runtime);
			_inputSubsystem->OpenRigTriggerInput(runtime->Revision);
			if (_hudPanel)
			{
					_hudPanel->PublishRoutingConfig(inputs, names, runtime);
			}
		}
		}
		complete(saved ? "MIDI desired selection accepted" : "MIDI applied, preferences not saved", true, saved);
	}
	catch (const std::exception& error)
	{
		complete(std::string("MIDI change failed: ") + error.what());
	}
}


// Membership presentation is committed only by the window owner.
bool Scene::_StationManagementAvailableLocked() const
{
	return _controlOperation == ControlOperation::None && !_controlWriterExclusion.owns_lock() && !_stationManagement.Busy() && _rigCoordinator.EditsEnabled() && !_rigSaveDialogOpen && !_rigSaveInProgress &&
		std::none_of(_stations.begin(), _stations.end(), [](const auto& station) { return station->IsPresentationMoving(); }) &&
		!_popupManager.IsOpen() && !_loopEditor.IsEngaged() && !_focusManager.IsEditingText() &&
		!_quantisationInteraction.OwnsPointer() && !_camera.IsBackgroundDragging() && !_isSceneTouching &&
		!_touchDownElement.lock() && (!_hudPanel || !_hudPanel->HasCableDrag());
}

void Scene::_RequestStationControl(gui::GuiStationControls::Intent intent)
{
	if (intent.Remove)
	{
		{ std::scoped_lock lock(_sceneMutex);
			if (!_StationManagementAvailableLocked()) return;
			_stationManagement.Target = intent.Remove;
			_stationManagement.Revision = _rigCoordinator.Accepted()->Revision;
		}
		_AcceptStationRemoval(false);
		return;
	}
	std::scoped_lock lock(_sceneMutex);
	if (!_StationManagementAvailableLocked() || StationManagement::LocalCount(_stations) >= io::JamFile::MaxStations) return;
	StationParams params;
	params.Size = {136u, 300u}; params.FadeSamps = std::min(_userConfig.Loop.FadeSamps, constants::MaxLoopFadeSamps);
	for (size_t suffix = 1u;; ++suffix)
	{
		params.Name = "Station" + std::to_string(suffix);
		if (std::none_of(_stations.begin(), _stations.end(), [&](const auto& station) { return station->Name() == params.Name; }) &&
			std::none_of(_rigCoordinator.Accepted()->Rig.Triggers.begin(), _rigCoordinator.Accepted()->Rig.Triggers.end(),
				[&](const auto& trigger) { return trigger.StationTarget == params.Name; })) break;
	}
	auto station = std::make_shared<Station>(params, Station::GetMixerParams(params.Size, MergeMixBehaviourParams{}));
	station->SetAllowedMidiChannels({1});
	_PrepareStation(station);
	auto candidate = _stations;
	auto firstRemote = std::find_if(candidate.begin(), candidate.end(), [](const auto& entry) { return entry->IsRemote(); });
	candidate.insert(intent.Prepend ? candidate.begin() : firstRemote, station);
	const auto accepted = _rigCoordinator.Accepted();
	if (_rigCoordinator.SubmitInventory(candidate) != RigCoordinator::EditResult::Pending)
	{ station->DetachGuiOwnershipForRetirement(); return; }
	if (!_inputSubsystem->RequestCloseRigTriggerInputFromUi(accepted->Revision))
	{ _rigCoordinator.CompleteTransition(_rigCoordinator.Staged()->Revision, false, {}); station->DetachGuiOwnershipForRetirement(); return; }
	_stationManagement.Candidate = _rigCoordinator.Staged();
	_stationManagement.Added = std::move(station);
	_rigTransitionRequestedRevision = 0u;
}

void Scene::_RequestTakeDitch(gui::GuiStationControls::Intent intent)
{
	std::scoped_lock lock(_sceneMutex);
	if (!_StationManagementAvailableLocked() || _popupManager.IsOpen() || !intent.TakeStation || !intent.Take ||
		std::find(_stations.begin(), _stations.end(), intent.TakeStation) == _stations.end()) return;
	const auto& takes = intent.TakeStation->GetLoopTakes();
	if (std::find(takes.begin(), takes.end(), intent.Take) == takes.end()) return;
	_takeDitchStation = std::move(intent.TakeStation);
	_takeDitchTake = std::move(intent.Take);
	if (_takeDitchDialog) _takeDitchDialog->ReleaseResources();
	_takeDitchDialog = std::make_shared<GuiPopup>();
	_takeDitchDialog->SetTitle("Ditch this take?");
	_takeDitchDialog->SetBodyLines({"The whole LoopTake and all its loops will be removed.", "Active recording will be cancelled.", "This cannot be undone."});
	_takeDitchDialog->ConfigureButtons({{{"Cancel", GuiStationControls::CancelTakeDitchControlIndex}, {"Ditch take", GuiStationControls::ConfirmTakeDitchControlIndex}}});
	_takeDitchDialog->SetButtonReceiver(ActionReceiver::shared_from_this());
	_takeDitchDialog->FitToViewport(_sizeParams.Size);
	_popupManager.Open(_takeDitchDialog);
}

void Scene::_CancelTakeDitch()
{
	std::scoped_lock lock(_sceneMutex);
	_takeDitchTake.reset();
	_takeDitchStation.reset();
}

void Scene::_ConfirmTakeDitch()
{
	std::shared_ptr<LoopTake> ditched;
	{
		std::scoped_lock lock(_sceneMutex);
		auto station = std::move(_takeDitchStation);
		auto take = std::move(_takeDitchTake);
		if (!station || !take || std::find(_stations.begin(), _stations.end(), station) == _stations.end()) return;
		if (station->DitchTake(take))
		{
			take->DeSelect();
			_CollapseRacksLocked();
			_InvalidateHover2d();
			ditched = std::move(take);
		}
	}
	if (ditched)
	{
		// Native window teardown may synchronously reenter scene focus handling.
		_windowSubsystem->CloseVstEditorsForTake(ditched);
	}
}

void Scene::_AcceptStationRemoval(bool consent)
{
	std::scoped_lock lock(_sceneMutex);
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted || _controlOperation != ControlOperation::None || _controlWriterExclusion.owns_lock() || !_rigCoordinator.EditsEnabled() || _rigSaveDialogOpen || _rigSaveInProgress) return;
	const auto validation = _stationManagement.Validate(_stations, accepted->Revision, consent);
	if (validation == StationManagement::Validation::Confirm)
	{
		_stationDeleteDialog = std::make_shared<GuiPopup>();
		_stationDeleteDialog->SetTitle("Delete " + _stationManagement.Target->Name() + "?");
		_stationDeleteDialog->SetBodyLines({"All takes and station configuration will be removed.", "Active recordings will be cancelled.", "This cannot be undone."});
		_stationDeleteDialog->ConfigureButtons({{{"Cancel", GuiStationControls::CancelRemovalControlIndex}, {"Delete station", GuiStationControls::DeleteRemovalControlIndex}}});
		_stationDeleteDialog->SetButtonReceiver(ActionReceiver::shared_from_this());
		_stationDeleteDialog->FitToViewport(_sizeParams.Size);
		_popupManager.Open(_stationDeleteDialog);
		return;
	}
	if (validation != StationManagement::Validation::Ready) { _stationManagement.Cancel(); return; }
	auto candidate = _stations;
	std::erase(candidate, _stationManagement.Target);
	if (_rigCoordinator.SubmitInventory(candidate, _stationManagement.Target) != RigCoordinator::EditResult::Pending)
	{ _stationManagement.Cancel(); return; }
	if (!_inputSubsystem->RequestCloseRigTriggerInputFromUi(accepted->Revision))
	{ _rigCoordinator.CompleteTransition(_rigCoordinator.Staged()->Revision, false, {}); _stationManagement.Cancel(); return; }
	_stationManagement.Candidate = _rigCoordinator.Staged();
	_stationManagement.Confirmed = consent;
	_rigTransitionRequestedRevision = 0u;
}

void Scene::_CommitStationMembershipLocked()
{
	if (_stationManagement.Failed)
	{
		if (_stationManagement.Added)
		{ _stationManagement.Added->ReleaseResources(); _stationManagement.Added->DetachGuiOwnershipForRetirement(); }
		if (_stationManagement.Target) _stationManagement.Target->SetMembershipRemoving(false);
		_stationManagement.Finish();
		_quantisationFeedback = "Station change failed. Please try again.";
		return;
	}
	if (!_stationManagement.Committed || !_stationManagement.Candidate) return;
	_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
	_performanceGroups.CancelPending();
	_audioEngine->PublishGroupMuteApplication(nullptr);
	_backgroundGesture.reset();
	const auto removed = _stationManagement.Target;
	const auto affectedBindings = _stationManagement.Candidate->TriggerRouteUpdates.size();
	if (removed)
	{
		_quantisation.DetachStationMaster(removed);
		removed->ClearPointerState();
		removed->DeSelect();
		for (const auto& take : removed->GetLoopTakeSnapshot()) { take->DeSelect(); take->DetachGuiOwnershipForRetirement(); }
		removed->DetachGuiOwnershipForRetirement();
		_camera.ForgetStation(removed);
		_popupManager.CloseOwnedBy(removed);
		_retiredStations.push_back(removed);
		_undoHistory.Clear(); // Outgoing hierarchy must not be resurrected by a retained undo.
	}
	_stations = *_stationManagement.Candidate->Stations;
	for (size_t i = 0u; i < _stations.size(); ++i)
	{
		auto& station = _stations[i];
		const float startX = station == _stationManagement.Added
			? (-50.0f + static_cast<float>(i) * 600.0f + (i == 0u ? -600.0f : 600.0f))
			: station->PresentationModelPosition().X;
		station->SetIndex(static_cast<unsigned int>(i));
		station->SetPosition({20 + static_cast<int>(i) * 600, 20});
		station->SetModelPosition({-50.0f + static_cast<float>(i) * 600.0f, -20.0f, 0.0f});
		station->BeginPresentationMove(startX);
		_camera.RegisterStation(i, station, station->LoopTakeRevision());
	}
	// Selections reside on object identities; survivors retain them without selecting a neighbour.
	_touchDownElement.reset(); _hoverElement3d.reset();
	_hoverPath3d.clear(); _hoverPath2d.clear();
	_selector->ClearPointerState();
	_selector->UpdateCurrentHover({}, Action::MODIFIER_NONE, false, Tweakable::TWEAKSTATE_NONE);
	_quantisationInteraction.CancelInteraction(true);
	_stationControls.Cancel();
	if (_stationManagement.Added)
	{
		_camera.RevealPosition(_stationManagement.Added->ModelPosition(), _sizeParams.Size,
			_StationCentre(_stations), 80);
	}
	_PublishAudioStations();
	_inputSubsystem->OpenRigTriggerInput(_rigCoordinator.Accepted()->Revision);
	_stationManagement.Finish();
	_quantisationFeedback = removed ? "Station deleted; " + std::to_string(affectedBindings) + " binding(s) unbound." : "Station added.";
	_InvalidateHover2d();
	_ResetIfEmpty();
}

bool Scene::ConsumeRigSaveRequest()
{
	const bool requested = _rigSaveRequested; _rigSaveRequested = false;
	std::scoped_lock lock(_sceneMutex);
	return requested && _StationManagementAvailableLocked() && !_rigSaveInProgress;
}

std::optional<io::RigFile> Scene::CaptureCommittedRig() const
{
	const auto capture = CaptureCommittedRigVersion();
	return capture ? std::optional(capture->Rig) : std::nullopt;
}

std::optional<Scene::CapturedRig> Scene::CaptureCommittedRigVersion() const
{
	std::scoped_lock lock(_sceneMutex);
	if (!_rigCoordinator.EditsEnabled() || _stationManagement.Busy()) return std::nullopt;
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted) return std::nullopt;
	auto rig = accepted->Rig;
	rig.User = _userConfig;
	return CapturedRig{std::move(rig), accepted->Revision, _sceneIdentity};
}

std::optional<Scene::CapturedRig> Scene::BeginExplicitRigSave()
{
	std::scoped_lock lock(_sceneMutex);
	if (!_rigCoordinator.EditsEnabled() || _stationManagement.Busy() || _rigSaveInProgress ||
		_controlOperation != ControlOperation::None || _popupManager.IsOpen() || _isSceneQuitting.load(std::memory_order_acquire)) return {};
	const auto accepted = _rigCoordinator.Accepted(); if (!accepted) return {};
	auto rig = accepted->Rig; rig.User = _userConfig; _rigSaveInProgress = true;
	return CapturedRig{std::move(rig), accepted->Revision, _sceneIdentity};
}

void Scene::SetRigSaveDialogOpen(bool open)
{
	std::scoped_lock lock(_sceneMutex);
	// Captured global keyboard edges from filename typing must not reach performance controls.
	actions::KeyAction discarded;
	while (_inputSubsystem->PumpGlobalKeyCapture(discarded)) {}
	_rigSaveDialogOpen = open;
}

void Scene::QueueExplicitRigSave(std::function<void()> work)
{
	std::scoped_lock lock(_sceneMutex);
	_rigSaveInProgress = true;
	_explicitRigSaveJob = std::move(work);
}

void Scene::FinishExplicitRigSave()
{
	std::scoped_lock lock(_sceneMutex); _rigSaveInProgress = false;
}

bool Scene::_JobReceiverAvailableLocked(const std::shared_ptr<base::ActionReceiver>& receiver) const
{
	for (auto element = std::dynamic_pointer_cast<base::GuiElement>(receiver); element; element = element->Parent())
		if (const auto station = std::dynamic_pointer_cast<Station>(element))
			return !station->MembershipRemoving() && std::find(_stations.begin(), _stations.end(), station) != _stations.end();
	// A detached engine child no longer has a membership owner. Late plugin or
	// structural jobs may release their values, but cannot mutate that hierarchy.
	return !std::dynamic_pointer_cast<LoopTake>(receiver) && !std::dynamic_pointer_cast<Loop>(receiver);
}

void Scene::_PublishGroupMuteApplication(std::shared_ptr<const GroupMuteApplication> application)
{
	if (!application) { _audioEngine->PublishGroupMuteApplication(nullptr); return; }
	auto stamped = std::make_shared<GroupMuteApplication>(*application);
	if (const auto rig = AcceptedRigSnapshot()) stamped->TopologyRevision = rig->TopologyRevision;
	_audioEngine->PublishGroupMuteApplication(std::move(stamped));
}
