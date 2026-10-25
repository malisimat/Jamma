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
	_isSceneQuitting(false),
	_isSceneReset(true),
	_globalMidiQuantState(io::JamFile::GlobalMidiQuantState::Off),
	_transportOffsetLoopFrac(0.0),
	_quantisation(),
	_loggingConfig{},
	_stations(),
	_workspace(std::make_unique<gui::Workspace>(params.Size)),
	_quantisationInteraction(_workspace->CtrlOverlay(), _quantisation, _stations),
	_userConfig(user),
	_audioEngine(std::make_unique<audio::AudioHost>(user)),
	_inputSubsystem(std::make_unique<io::IoInputSubsystem>(user, io::LoggingConfig{})),
	_windowSubsystem(std::make_unique<vst::VstEditorWindowManager>()),
	_networkService(std::make_unique<ninjam::NinjamNetworkService>())
{
	_routingEditNow = std::move(params.RoutingEditNow);
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
		if (_stationManagement.Busy() || _rigSaveInProgress || _rigSaveDialogOpen || _operation.Current() != ControlOperation::None ||
			_workspace->Editor().IsEngaged() || _isSceneQuitting.load(std::memory_order_acquire)) return "Finish the current edit or save before learning";
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
	_workspace->InitializeRacks(gui::VstRackControls::Host{
		_sceneMutex, _stations, *_windowSubsystem,
		[this]() { return !_isSceneQuitting.load(std::memory_order_acquire) &&
			!_sessionReplacing.load(std::memory_order_acquire) && !_operation.HoldsWriter() &&
			_operation.Current() == ControlOperation::None && _rigCoordinator.EditsEnabled(); },
		[this]() { CommitChanges(); }, {},
		[this](std::string title, std::string message, std::string accept) {
			return _workspace->Notices().Confirm(std::move(title),std::move(message),std::move(accept)); },
		[this](const std::wstring& error) { ReportRuntimeNotice("VST",utils::EncodeUtf8(error)); }});
	_workspace->BindQuantisation(_quantisation, _quantisationInteraction);
	_workspace->Initialize(std::move(hudParams), _inputSubsystem->ForcedChannelOverride(),
		_transportOffsetLoopFrac, _globalMidiQuantState,
		[this](const auto& subtree) { _workspace->OnSettingsHidden(subtree); },
		[this](bool selection, GroupCommand command, std::size_t slot) { _ExecuteGroupCommandLocked(selection, command, slot); });
	_workspace->InitializeEditor(_sceneMutex, _stations,
		[this]() { _workspace->OnLoopGridEditorOpened({ _sceneMutex, _stations }, _WorkspaceInputPresentation()); });
	_workspace->Settings().SetAudioDraft(user.Audio);
	_workspace->Settings().SetMidiDraft(user.Midi.Devices);

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
	const auto decision = _workspace->TakeTempoDecision();
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
	std::unique_lock candidateLifecycle(scene->_operation.LifecycleMutex());
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

	if (scene->_workspace->Hud())
		scene->_workspace->Hud()->SetRoutingConfig(loaded.HudAudioInputCount, std::move(loaded.HudMidiInputs), *acceptedRig);
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
	_workspace->OnCameraTriggerActivity();
}

bool Scene::PreservesCameraIdleForKey(unsigned int key) const noexcept
{
	// Modifiers alone do not navigate; the eventual modified action owns activity.
	if (key == 16u || key == 17u || key == 18u) return true;
	if (_workspace->PopupOpen() || _workspace->IsEditingText() || _workspace->Editor().IsEditingText()) return false;
	return key == 32u || key == 192u || key == 96u
		|| key == 8u || (key >= 112u && key <= 120u);
}

void Scene::OnHumanActivity()
{
	_workspace->OnHumanActivity();
}

void Scene::SetCameraInteractionEligibility(bool windowEligible, bool heldInteraction)
{
	_workspace->SetCameraInteractionEligibility(windowEligible, heldInteraction, *_cameraRecordings->Snapshot());
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
	const auto frameTime = _workspace->BeginCameraPresentation(now);
	const auto deltaSeconds = frameTime.DeltaSeconds;
	const bool diagnosticDue = frameTime.DiagnosticDue;
	Camera::Presentation presentation;
	std::shared_ptr<const CameraRecordingObservation> recordings;
	bool controlsAvailable = false;
	{
		std::scoped_lock lock(_sceneMutex);
		presentation = _workspace->ObserveCameraLocked(_stations, diagnosticDue);
		controlsAvailable = !_rigSaveDialogOpen && !_rigSaveInProgress && !_stationManagement.Busy()
			&& _operation.Current() == ControlOperation::None && !_operation.HoldsWriter();
		recordings = _cameraRecordings->Snapshot();
	}

	_workspace->AdvanceCamera(now, deltaSeconds, presentation, *recordings, controlsAvailable,
		{ _sceneMutex, _stations }, _WorkspaceInputPresentation());
}

bool Scene::AdvanceUiAnimations()
{
	// Native picker/editor calls can pump Windows messages; never hold the
	// scene lock while processing a captured rack gesture.
	_workspace->PumpNativeControls();
	if (_quantisationInputResetRequested.exchange(false, std::memory_order_acq_rel))
		_workspace->ReleaseQuantisationInput({ _sceneMutex, _stations }, _WorkspaceInputPresentation());
	std::scoped_lock lock(_sceneMutex);
	_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
	const auto accepted = _rigCoordinator.Accepted();
	return _workspace->AdvancePresentationLocked(_stations, _workspace->Editor(), _workspace->Racks(), _performanceGroups.Groups(),
		{ _workspace->CtrlHeld(), _isSceneQuitting.load(std::memory_order_acquire), _rigSaveInProgress, _stationManagement.Busy(),
			_rigCoordinator.EditsEnabled(), _StationManagementAvailableLocked(), accepted ? accepted->Revision : 0u, _performanceGroups.Error() });
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



ActionResult Scene::OnAction(GuiAction action)
{
	if (_isSceneQuitting.load(std::memory_order_acquire)) return ActionResult::NoAction();
	if (_operation.HoldsWriter() && !SceneSettingsControls::IsOperationControl(action.Index)) return ActionResult::NoAction();
	if (action.Index == GuiMainPanel::LoadRigControlIndex) { _panelRigLoadRequested = true; return ActionResult::NoAction(); }
	const auto deviceChange = _workspace->Settings().DecodeDeviceChange(action);
	if (deviceChange)
	{
		if (const auto save = std::get_if<SceneSettingsControls::SaveSession>(&*deviceChange))
		{ _RequestSave(save->ChooseFolder, save->AsDefault); return ActionResult::NoAction(); }
		if (std::holds_alternative<SceneSettingsControls::LoadSession>(*deviceChange))
		{ if (_operation.Current() == ControlOperation::None) _panelLoadRequested = true; return ActionResult::NoAction(); }
		if (std::holds_alternative<SceneSettingsControls::RetryAudio>(*deviceChange))
		{ _audioStartRequested = true; _audioSchedule.Changed(DeviceSelectionSchedule::Edit::Format); return ActionResult::NoAction(); }
		if (std::holds_alternative<SceneSettingsControls::RefreshMidi>(*deviceChange))
		{ _devicePageDirty = true; RequestMidiRefresh(); return ActionResult::NoAction(); }
		if (std::holds_alternative<SceneSettingsControls::RetryPreferences>(*deviceChange))
		{
			{ std::scoped_lock lock(_sceneMutex); if (_stationManagement.Busy() || _rigSaveInProgress || _rigSaveDialogOpen) return ActionResult::NoAction(); }
			if (_operation.Current() != ControlOperation::None || !_rigCoordinator.EditsEnabled()) { _workspace->Settings().SetDeviceStatus("Preferences retry waits for other operations"); return ActionResult::NoAction(); }
			std::unique_lock lifecycle(_operation.LifecycleMutex(), std::try_to_lock);
			if (!lifecycle.owns_lock() || !_rigCoordinator.EditsEnabled()) return ActionResult::NoAction();
			const bool merged = _rigCoordinator.MergeHardwarePreferences(_pendingAudioPersistence, _pendingMidiPersistence);
			const auto accepted = AcceptedRigSnapshot();
			if (merged && accepted && SaveHardwarePreferences(accepted->Rig)) { _pendingAudioPersistence.reset(); _pendingMidiPersistence.reset(); _hardwarePersistencePending = false; _workspace->Settings().SetDeviceStatus("Preferences saved"); }
			else { _workspace->Settings().SetDeviceStatus("Preferences not saved; retry available"); ReportRuntimeNotice("Device preferences","Preferences not saved; retry available"); }
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
		_workspace->CloseTakeDitch();
		if (action.Index == GuiStationControls::ConfirmTakeDitchControlIndex) _ConfirmTakeDitch();
		else _CancelTakeDitch();
		return ActionResult::NoAction();
	}
	if (action.ElementType == GuiAction::ACTIONELEMENT_BUTTON &&
		(action.Index == GuiStationControls::CancelRemovalControlIndex || action.Index == GuiStationControls::DeleteRemovalControlIndex))
	{
		_workspace->CloseStationRemoval();
		if (action.Index == GuiStationControls::DeleteRemovalControlIndex) _AcceptStationRemoval(true);
		else { std::scoped_lock lock(_sceneMutex); _stationManagement.Cancel(); }
		return ActionResult::NoAction();
	}
	if (deviceChange)
	{
		const auto rig = _rigCoordinator.Accepted();
		const auto draft = _workspace->Settings().EditDeviceDraft(*deviceChange, _deviceInventory,
			rig ? rig->Rig.User.Serial.Devices : _userConfig.Serial.Devices, !_SessionHasTakes());
		using Draft = SceneSettingsControls::DraftChange;
		if (draft == Draft::Device || draft == Draft::Channels || draft == Draft::Format)
		{
			_audioSchedule.Changed(draft == Draft::Device ? DeviceSelectionSchedule::Edit::Device :
				draft == Draft::Channels ? DeviceSelectionSchedule::Edit::Channels : DeviceSelectionSchedule::Edit::Format);
			_devicePageDirty |= draft == Draft::Device;
		}
		_midiDraftPending |= draft == Draft::Midi;
		if (draft != Draft::None) return ActionResult::NoAction();
	}
	if (action.ElementType == GuiAction::ACTIONELEMENT_MIDIQUANTISATION)
	{
		_ForceGlobalMidiQuantStateMixedOnLocalEdit();
		return ActionResult::NoAction();
	}
	using Settings = SceneSettingsControls;
	const auto change = _workspace->Settings().Decode(action, _transportOffsetLoopFrac);
	if (!change) return ActionResult::NoAction();
	if (auto view = std::get_if<Settings::ViewModeChange>(&*change))
	{
		_workspace->SetViewMode(view->Value, { _sceneMutex, _stations }, _WorkspaceInputPresentation());
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
		_workspace->Settings().RefreshChannelPresentation(_inputSubsystem->ForcedChannelOverride());
	}
	else if (auto offset = std::get_if<Settings::OffsetChange>(&*change))
	{
		_SetTransportOffsetLoopFrac(offset->Value, false);
		_workspace->Settings().Offset()->SynchronizeValueFromOwner(_transportOffsetLoopFrac);
	}
	else if (std::holds_alternative<Settings::TapTempo>(*change))
	{
		_quantisation.PulseOverlay();
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
	std::scoped_lock lifecycle(_operation.LifecycleMutex());
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
	if (!_operation.AdmissionPending()) _RefreshMidiIfNeeded();
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
		_localTiming.Advance(_quantisation, *_audioEngine, _stations);
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
		if (_workspace->Hud())
		{
			_workspace->Hud()->PublishRoutingConfig(inputs, connectedIds, runtime);
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
		_workspace->SetFeedback("Station renamed. Save both JAM session and rig explicitly for consistent reload.");
	if (pending->MembershipChange) _stationManagement.Committed = true;
	_audioEngine->ReleaseRigSnapshotsBefore(pending->Revision);
	_rigCoordinator.ReleaseRetired();
	if (_workspace->Hud())
	{
		// Routing is published under the surrounding Scene lock; UI owns widgets.
		unsigned int audioInputs = _audioEngine->GetStreamParams().NumInputChannels;
		const auto midiState = _midiState.load(std::memory_order_acquire);
		_workspace->Hud()->PublishRoutingConfig(audioInputs, midiState->ConnectedIds, pending);
	}
	if (!pending->MembershipChange) _inputSubsystem->OpenRigTriggerInput(pending->Revision);
}

RigCoordinator::EditResult Scene::RequestRigEdit(const io::RigFile& candidateRig, RigCoordinator::EditReceiptPtr* receipt)
{
	std::scoped_lock lock(_sceneMutex);
	return _RequestRigEditLocked(candidateRig, receipt);
}

std::string Scene::_RequestStationRenameLocked(const std::shared_ptr<Station>& station,
	std::uint64_t revision, const std::string& name)
{
	if (_operation.Current() != ControlOperation::None || _stationManagement.Busy() ||
		_operation.HoldsWriter() || _sessionReplacing.load(std::memory_order_acquire) ||
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

RigCoordinator::EditResult Scene::_RequestRigEditLocked(const io::RigFile& candidateRig, RigCoordinator::EditReceiptPtr* receipt)
{
	if (receipt) receipt->reset();
	if (_operation.Current() != ControlOperation::None || _stationManagement.Busy() || _rigSaveDialogOpen || _rigSaveInProgress) return RigCoordinator::EditResult::EditsDisabled;
	switch (_RoutingEditAvailability())
	{
	case gui::RoutingEditAvailability::Applying: return RigCoordinator::EditResult::EditsDisabled;
	case gui::RoutingEditAvailability::AudioCallbackInactive: return RigCoordinator::EditResult::AudioCallbackInactive;
	case gui::RoutingEditAvailability::TriggerBusy: return RigCoordinator::EditResult::TriggerBusy;
	case gui::RoutingEditAvailability::Ready: break;
	}
	std::unique_lock lifecycle(_operation.LifecycleMutex(), std::try_to_lock);
	if (!lifecycle.owns_lock() || !_rigCoordinator.EditsEnabled()) return RigCoordinator::EditResult::EditsDisabled;
	const auto accepted = _rigCoordinator.Accepted();
	const auto result = _rigCoordinator.SubmitCandidate(candidateRig, receipt);
	if (result == RigCoordinator::EditResult::ReinitialisationRequired)
		_workspace->SetFeedback("Device setup and defaults require scene reinitialisation; reload or restart with the updated rig.");
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
	const auto now = _routingEditNow ? _routingEditNow() : std::chrono::steady_clock::now();
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
	_workspace->InitReceivers(ActionReceiver::shared_from_this());
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
	_workspace->Init();
}

void Scene::InitAudio(bool generatedRig, const audio::AsioInventory* inventory, bool exactSelection)
{
	std::scoped_lock lifecycle(_operation.LifecycleMutex());
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
		if (started) _workspace->Settings().SetAudioDraftFromStream(actual);
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
		if (_workspace->Hud())
		{
			std::scoped_lock lock(_sceneMutex);
			_workspace->Hud()->PublishRoutingConfig(actualInputs, midiState->ConnectedIds, runtime);
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
	if (_workspace->Hud()) _workspace->Hud()->CancelTriggerRenames();
	for (const auto& station : _stations)
		if (auto label = station->GetGuiRack()->StationLabel()) label->Cancel();
	_workspace->ResetRackControls();
	_sessionReplacing.store(true, std::memory_order_release);
	_operation.ReleaseWriter();
	// Finish any admitted job tick before borrowing lifecycle state. No scene,
	// MIDI or application gate mutex is held while acquiring this owner boundary.
	std::unique_lock lifecycle(_operation.LifecycleMutex());
	_workspace->CancelBackgroundClick();
	{
		// Recovery borrows callback-owned fields under this same lock. Finish
		// that borrow before teardown, and prevent a later job reopening admission.
		std::scoped_lock lock(_sceneMutex);
		// Publish the permanent close token before the job loop can exit and make
		// its final acknowledgement. Releasing quit first can strand that token.
		_inputSubsystem->CloseRigTriggerInputForever();
		_inputSubsystem->EndTriggerLearn();
		if (_workspace->Hud()) _workspace->Hud()->CancelTriggerLearn();
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
	_workspace->FinishRenderer();
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
	_workspace->ClearReceivers();
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
		if (_workspace->Hud() && _workspace->Hud()->ApplyPendingRoutingConfig())
			hoverChanged = true;
		if (_workspace->Hud()) _workspace->Hud()->UpdateTriggerLearn();

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

		_workspace->SynchronizeTempoPrompt(_networkService->PendingRemoteTempoPrompt());

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
		_workspace->InvalidateHover2d();
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

void Scene::InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	ResourceUser::InitResources(resourceLib, forceInit);
	_workspace->InitNoticeResources(resourceLib, forceInit);
	// Initialize controls added by rig edits on the render thread before drawing them.
	{
		std::scoped_lock lock(_sceneMutex);
		if (_stationManagement.Added) _stationManagement.Added->InitResources(resourceLib, false);
		_workspace->InitDynamicResourcesLocked(resourceLib, _stations, _workspace->CtrlHeld(), _StationManagementAvailableLocked());
	}

	// Stations can be added after scene resources are initialised.
	auto stations = SnapshotStations();
	for (auto& station : stations)
	{
		if (station)
			station->InitResources(resourceLib, forceInit);
	}
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
	_workspace->AttachStationLabel(station, [this, target = std::weak_ptr(station)](const auto& name, auto revision) {
			std::scoped_lock lock(_sceneMutex);
			return _RequestStationRenameLocked(target.lock(), revision, name);
		});

	auto selectDepth = static_cast<unsigned int>(_workspace->PresentationMode());
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
		_workspace->RegisterStationLocked(_stations.size() - 1u, station);
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
	if (_workspace->Settings().GlobalQuantisation())
		_workspace->Settings().GlobalQuantisation()->SetCurrentValue(static_cast<unsigned int>(state), true);

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

	if (updateInput && _workspace->Settings().Offset())
		_workspace->Settings().Offset()->SetValue(loopFrac, false);
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
	const auto depth = _workspace->SelectionDepth();
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
	_workspace->SetSelectionDepthLocked(depth);
	for (const auto& station : _stations) station->SetSelectDepth(depth);
}

void Scene::_ExecuteGroupCommandLocked(bool selection, GroupCommand command, std::size_t slot)
{
	_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
	_performanceGroups.Execute(selection, command, slot, _stations, _GroupSelectionDepthLocked(),
		[this](auto application) { _PublishGroupMuteApplication(std::move(application)); });
	if (selection && command == GroupCommand::RecallOrStore) _RestoreGroupSelectionDepthLocked();
	if (_workspace->SelectionPanel()) _workspace->SelectionPanel()->RefreshPerformanceGroups(_performanceGroups.Groups());
	_workspace->InvalidateHover2d();
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
		_quantisation.UpdateStationHints(nullptr, _workspace->SelectionDepth(), false, _stations);
		for (const auto& station : _stations)
			if (station && !station->IsRemote())
				for (const auto& take : station->GetLoopTakes())
					if (take && _workspace->Settings().Subdivision())
					{
						_workspace->Settings().Subdivision()->SetCurrentValue(
							midi::MidiQuantisation::FractionDisplayIndex(take->MidiQuantisation().Fraction), true);
						return handled;
					}
	}
	return handled;
}

bool Scene::HasSelection() const
{
	std::scoped_lock lock(_sceneMutex);
	return _HasSelection();
}

bool Scene::_HasSelection() const
{
	return HierarchySelection::HasSelection(HierarchySelection::LockedStations(_stations));
}

void Scene::_ClearStationQuantisation()
{
	_quantisation.ClearStationHints(_stations);
}

void Scene::OnInputFocusLost()
{
	_workspace->ResetKeyboardObservation();
	_contextualKeyboard.FocusLost();
	if (const auto rig = _rigCoordinator.Accepted(); rig && _inputSubsystem->TryAcceptUiRigTriggerInput(rig->Revision))
	{
		base::Action release;
		release.SetActionTime(Timer::GetTime());
		_contextualKeyboard.Flush(*rig, release);
	}
	// Window-owned focus finalization synchronously notifies the settings owner,
	// which takes its own scene lock (e.g. SnapshotStations). Commit before locking.
	_workspace->CommitFocus();
	std::scoped_lock lock(_sceneMutex);

	for (const auto key : {VK_INSERT, VK_PRIOR, VK_NEXT})
	{
		actions::KeyAction release; release.KeyChar = key; release.KeyActionType = KeyAction::KEY_UP;
		if (key == VK_INSERT) _inputSubsystem->HandleAutomationKey(release, _stations, {}, {});
		else _inputSubsystem->HandleChannelOverrideKey(release, _stations);
	}
	_performanceGroups.ResetKeys();
	_workspace->OnFocusLostLocked({ _sceneMutex, _stations }, _WorkspaceInputPresentation());
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
		[this](const auto& device) { return device.Name == _workspace->Settings().AudioDraft().Name; });
	if (found == _deviceInventory.Devices.end() || !found->Probed) { error = "Selected audio device is unavailable"; return _audioStartRequested; }
	if ((!_workspace->Settings().AudioDraft().NumChannelsIn && !_workspace->Settings().AudioDraft().NumChannelsOut) || _workspace->Settings().AudioDraft().NumChannelsIn > found->InputChannels || _workspace->Settings().AudioDraft().NumChannelsOut > found->OutputChannels) { error = "Invalid input/output channel counts"; return false; }
	if (!_workspace->Settings().AudioDraft().BufSize || !_workspace->Settings().AudioDraft().SampleRate || std::find(found->SampleRates.begin(), found->SampleRates.end(), _workspace->Settings().AudioDraft().SampleRate) == found->SampleRates.end()) { error = "Unsupported sample rate or invalid buffer request"; return false; }
	const auto actual = _audioEngine->GetStreamParams();
	if (_SessionHasTakes() && _workspace->Settings().AudioDraft().SampleRate != (actual.SampleRate ? actual.SampleRate : _userConfig.Audio.SampleRate)) { error = "Sample rate cannot change while any takes exist"; return false; }
	return true;
}



void Scene::_RebuildDevicePage()
{
	_workspace->MainPanel()->FinalizePageSettings(SettingsPage::Audio);
	const auto midi = _midiState.load(std::memory_order_acquire);
	const auto pages = _workspace->Settings().BuildDevicePages(_deviceInventory, _workspace->Settings().AudioDraft(),
		_acceptedBufferSizes, midi->Connection, _workspace->Settings().MidiDraft(), _workspace->Popups(), ActionReceiver::shared_from_this());
	_workspace->MainPanel()->ReplacePageSettings(SettingsPage::Audio, pages.Audio);
	_workspace->MainPanel()->ReplacePageSettings(SettingsPage::Midi, pages.Midi);
	_displayedMidiState = midi; _devicePageDirty = false;
}

void Scene::ReportFileStatus(const std::string& status)
{
	_workspace->Settings().SetFileStatus(status);
	std::cout << "[SESSION scene=" << _sceneIdentity << " operation=" << _operation.Identity() << "] " << status << std::endl;
}

void Scene::_RequestSave(bool chooseFolder, bool asDefault)
{
	if (_operation.Current() != ControlOperation::None || !_rigCoordinator.EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	if (_CaptureIsBusy()) { ReportFileOutcome("Save unavailable during capture or pending actions"); return; }
	if (asDefault && (_defaultJamPath.empty() || !_defaultJamSaver)) { ReportFileOutcome("Startup jam path unavailable"); return; }
	_operation.Begin(ControlOperation::Save);
	auto folder = asDefault && _currentJamPath.empty() ? utils::GetParentDirectory(_defaultJamPath) : _sessionSaveFolder;
	if (chooseFolder || folder.empty()) folder = utils::PickDirectory(L"Choose session save folder");
	if (folder.empty()) { _operation.Begin(ControlOperation::None); return; }
	_pendingSaveAsDefault = asDefault; _pendingSaveFolder = std::move(folder); _operation.Begin(ControlOperation::Save);
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
	if (_operation.Current() != ControlOperation::None || !_rigCoordinator.EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	if (_CaptureIsBusy()) { ReportFileOutcome("Load unavailable during capture or pending actions"); return; }
	_operation.Begin(ControlOperation::Load);
	try { _pendingLoadPath = utils::PickJamFile(); }
	catch (...) { _operation.Begin(ControlOperation::None); throw; }
	if (_pendingLoadPath.empty()) _operation.Begin(ControlOperation::None);
}

void Scene::RequestSessionLoad(std::wstring path)
{
	if (path.empty()) return;
	if (_operation.Current() != ControlOperation::None || !_rigCoordinator.EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	if (_CaptureIsBusy()) { ReportFileOutcome("Load unavailable during capture or pending actions"); return; }
	_pendingLoadPath = std::move(path); _operation.Begin(ControlOperation::Load);
}

std::wstring Scene::TakeReadySessionLoad()
{
	return _operation.LoadReady() ? std::exchange(_pendingLoadPath, {}) : std::wstring();
}

bool Scene::_FinishControlOperation(std::string* restorationError)
{
	if (_operation.Current() == ControlOperation::Audio) _audioSchedule.NextChange = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
	std::string error;
	const auto audioRestore = _operation.RestoreAudio(*_audioEngine, error);
	bool restored = audioRestore.Success;
	if (!restored)
	{
		if (restorationError) *restorationError = error;
		_workspace->Settings().SetDeviceStatus("Stopped: " + error); ReportFileOutcome("Audio restoration failed: " + error);
	}
	if (audioRestore.AdmissionRestored)
	{
		std::scoped_lock lock(_sceneMutex);
		_sourceLossRecoveryGateClosed = false;
	}
	try { _inputSubsystem->ResumeMidiDispatch(); }
	catch (const std::exception& dispatchError)
	{
		restored = false;
		if (restorationError) *restorationError += "; MIDI resume failed: " + std::string(dispatchError.what());
		_workspace->Settings().SetDeviceStatus("MIDI dispatch stopped: " + std::string(dispatchError.what()));
		ReportRuntimeNotice("MIDI", "MIDI dispatch stopped: " + std::string(dispatchError.what()));
	}
	std::optional<std::uint64_t> acceptedRevision;
	if (_operation.InputClosed())
		if (const auto accepted = AcceptedRigSnapshot()) acceptedRevision = accepted->Revision;
	_operation.Finish(*_inputSubsystem, acceptedRevision);
	return restored;
}

void Scene::FinishSessionLoad(bool replaced, const std::string& error)
{
	if (replaced)
	{
		_sessionReplacing.store(true, std::memory_order_release);
		_operation.FinishReplacement();
	}
	else { ReportFileOutcome(error.empty() ? "Load failed; current session retained" : error); _FinishControlOperation(); }
}

void Scene::PumpDeviceAndFileControls()
{
	if (_isSceneQuitting.load(std::memory_order_acquire)) return;
	bool inventoryBusy = false;
	{ std::scoped_lock lock(_sceneMutex); inventoryBusy = _stationManagement.Busy() || _rigSaveDialogOpen || _rigSaveInProgress; }
	const bool filesEnabled = !inventoryBusy && _operation.Current() == ControlOperation::None && _rigCoordinator.EditsEnabled();
	_workspace->Settings().SetSessionEnabled(filesEnabled, filesEnabled && !_defaultJamPath.empty() && static_cast<bool>(_defaultJamSaver));
	const auto now = std::chrono::steady_clock::now();
	const auto actual = _audioEngine->GetStreamParams();
	if (actual.BufSize && !_acceptedBufferSizes.contains(actual.Name)) { _acceptedBufferSizes[actual.Name] = {actual.BufSize}; _devicePageDirty = true; }
	if (_devicePageDirty || _displayedMidiState != _midiState.load(std::memory_order_acquire)) _RebuildDevicePage();
	std::ostringstream text;
	text << actual.Name << " | " << actual.SampleRate << " Hz | " << actual.NumInputChannels << " in / " << actual.NumOutputChannels << " out | buffer " << actual.BufSize;
	_workspace->Settings().SetActiveAudioStatus(text.str());
	if (_operation.Current() == ControlOperation::None)
	{
		if (inventoryBusy || !_rigCoordinator.EditsEnabled()) return;
		if (_midiDraftPending)
		{
			_operation.Begin(ControlOperation::Midi); _midiDraftPending = false;
			_operation.NextIdentity();
			_pendingMidiDeviceRequest.store(std::make_shared<const MidiDeviceRequest>(MidiDeviceRequest{_sceneIdentity, _operation.Identity(), _workspace->Settings().MidiDraft()}), std::memory_order_release);
		}
		else if (_audioSchedule.Revision != _audioSchedule.AttemptedRevision && now >= _audioSchedule.NextChange)
		{
			if (now < _audioSchedule.ChannelDeadline) { _workspace->Settings().SetDeviceStatus("Settling channel counts (3 seconds)"); return; }
			if (_CaptureIsBusy()) { _workspace->Settings().SetDeviceStatus("Waiting for recording/pending capture"); return; }
			if (!_audioStartRequested && _workspace->Settings().AudioDraft().Name == actual.Name && _workspace->Settings().AudioDraft().SampleRate == actual.SampleRate && _workspace->Settings().AudioDraft().BufSize == actual.BufSize && _workspace->Settings().AudioDraft().NumChannelsIn == actual.NumInputChannels && _workspace->Settings().AudioDraft().NumChannelsOut == actual.NumOutputChannels) { _audioSchedule.AttemptedRevision = _audioSchedule.Revision; return; }
			std::string error;
			if (!_ValidateAudioDraft(error)) { _workspace->Settings().SetDeviceStatus(error); ReportRuntimeNotice("Audio device",error); _audioSchedule.AttemptedRevision = _audioSchedule.Revision; return; }
			_operation.Begin(ControlOperation::Audio);
		}
		else return;
	}
	if (_operation.Current() == ControlOperation::Midi)
	{
		if (const auto completion = _completedMidiDeviceRequest.exchange({}, std::memory_order_acq_rel))
		{
			if (completion->SceneId != _sceneIdentity || completion->OperationId != _operation.Identity()) return;
			if (completion->Accepted)
			{
				if (completion->PreferencesSaved) { _pendingAudioPersistence.reset(); _pendingMidiPersistence.reset(); }
				else _pendingMidiPersistence = completion->Desired;
			}
			_hardwarePersistencePending = _pendingAudioPersistence.has_value() || _pendingMidiPersistence.has_value();
			_workspace->Settings().SetDeviceStatus(completion->Status); _operation.Begin(ControlOperation::None); _devicePageDirty = true;
			ReportRuntimeNotice("MIDI devices",completion->Status);
		}
		return;
	}
	if (_operation.LoadReady()) return;
	if (_operation.Current() == ControlOperation::Audio && now < _audioSchedule.ChannelDeadline) { _FinishControlOperation(); return; }
	if (!_operation.InputClosed())
	{
		const auto accepted = AcceptedRigSnapshot();
		if (!accepted || !_rigCoordinator.EditsEnabled()) return;
		_operation.RequestBoundary(*_inputSubsystem, accepted->Revision);
		return; // Job acknowledges on its next turn; never wait under an application lock.
	}
	if (!_operation.TryAcquireBoundary(*_inputSubsystem)) return;
	try
	{
	std::string error;
	if (_operation.Current() == ControlOperation::Save)
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
		if (_operation.Current() != ControlOperation::None) _FinishControlOperation();
		return;
	}
	if (!_operation.PauseAudio(*_audioEngine, error))
	{
		_workspace->Settings().SetDeviceStatus("Stopped/error: " + error); ReportFileOutcome(error);
		if (_operation.Current() == ControlOperation::Audio) _audioSchedule.AttemptedRevision = _audioSchedule.Revision;
		_FinishControlOperation(); return;
	}
	_inputSubsystem->SuspendMidiDispatch();
	for (const auto& station : _stations) if (station) station->ResetLiveMidiWhileQuiescent();
	// With producers excluded and callbacks drained, capture-start wins deterministically.
	if (_operation.Current() != ControlOperation::Midi && _CaptureIsBusy())
	{
		if (_operation.Current() == ControlOperation::Audio) _workspace->Settings().SetDeviceStatus("Waiting for recording");
		else ReportFileOutcome("File operation rejected during capture/pending actions");
		_FinishControlOperation(); return;
	}
	if (_operation.Current() == ControlOperation::Load) { _operation.MarkLoadReady(); ReportFileStatus("Loading session"); return; }

	if (_operation.Current() == ControlOperation::Audio)
	{
		const auto admittedRevision = _audioSchedule.Revision;
		const auto requested = _workspace->Settings().AudioDraft();
		_audioSchedule.Attempted(admittedRevision, now);
		if (!_ValidateAudioDraft(error)) { _workspace->Settings().SetDeviceStatus(error); ReportRuntimeNotice("Audio device",error); }
		else
		{
			_workspace->Settings().SetDeviceStatus("Changing audio device");
			const auto oldRate = actual.SampleRate;
			const bool applied = _audioEngine->Reconfigure(requested, error);
			const auto acceptedParams = _audioEngine->GetStreamParams();
			if (applied)
			{
				auto settings = requested; settings.Name = acceptedParams.Name; settings.SampleRate = acceptedParams.SampleRate; settings.BufSize = acceptedParams.BufSize; settings.NumBuffers = acceptedParams.NumBuffers; settings.NumChannelsIn = acceptedParams.NumInputChannels; settings.NumChannelsOut = acceptedParams.NumOutputChannels;
				if (_audioStartRequested) _operation.RestoreRunningAudio();
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
					_workspace->Hud()->PublishRoutingConfig(acceptedParams.NumInputChannels, newMidiNames, runtime);
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
				_workspace->Settings().SetDeviceStatus(_hardwarePersistencePending ? "Applied, preferences not saved" : (acceptedParams.NumOutputChannels < actual.NumOutputChannels ? "Ready; unavailable output routes disconnected" : "Ready"));
				ReportRuntimeNotice("Audio device",_hardwarePersistencePending ? "Applied, preferences not saved; retry available" :
					(acceptedParams.NumOutputChannels < actual.NumOutputChannels ? "Ready; unavailable output routes disconnected" : "Audio device ready"));
				if (_audioSchedule.Revision == admittedRevision) _workspace->Settings().SetAudioDraft(settings);
				_devicePageDirty = true;
			}
			else { _workspace->Settings().SetDeviceStatus(error); ReportRuntimeNotice("Audio device",error); }
			_audioStartRequested = false;
			std::cout << "[DEVICES scene=" << _sceneIdentity << " operation=" << _operation.Identity() << "] audio " << (applied ? "applied " : "failed ")
				<< "requested='" << requested.Name << "' in=" << requested.NumChannelsIn << " out=" << requested.NumChannelsOut << " rate=" << requested.SampleRate << " buffer=" << requested.BufSize
				<< " actual='" << acceptedParams.Name << "' in=" << acceptedParams.NumInputChannels << " out=" << acceptedParams.NumOutputChannels << " rate=" << acceptedParams.SampleRate << " buffer=" << acceptedParams.BufSize << " " << error << std::endl;
		}
	}
	if (_operation.Current() != ControlOperation::None) _FinishControlOperation();
	}
	catch (const std::exception& error)
	{
		ReportFileOutcome(std::string("Operation failed: ") + error.what());
		if (_operation.Current() == ControlOperation::Audio) _audioSchedule.AttemptedRevision = _audioSchedule.Revision;
		_FinishControlOperation();
	}
}


void Scene::_ReconcileMidiDeviceRequestOnJob()
{
	// OnJobTick holds the lifecycle owner lock; complete values are handed off by
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
			if (_workspace->Hud())
			{
					_workspace->Hud()->PublishRoutingConfig(inputs, names, runtime);
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
	return _operation.Current() == ControlOperation::None && !_operation.HoldsWriter() && !_stationManagement.Busy() && _rigCoordinator.EditsEnabled() && !_rigSaveDialogOpen && !_rigSaveInProgress &&
		std::none_of(_stations.begin(), _stations.end(), [](const auto& station) { return station->IsPresentationMoving(); }) &&
		!_workspace->PopupOpen() && !_workspace->Editor().IsEngaged() && !_workspace->IsEditingText() &&
		!_quantisationInteraction.OwnsPointer() && !_workspace->CameraDragging() && !_workspace->Touching() &&
		!_workspace->HasPointerCapture() && (!_workspace->Hud() || !_workspace->Hud()->HasCableDrag());
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
	if (!_StationManagementAvailableLocked() || _workspace->PopupOpen() || !intent.TakeStation || !intent.Take ||
		std::find(_stations.begin(), _stations.end(), intent.TakeStation) == _stations.end()) return;
	const auto& takes = intent.TakeStation->GetLoopTakes();
	if (std::find(takes.begin(), takes.end(), intent.Take) == takes.end()) return;
	const auto accepted = _rigCoordinator.Accepted();
	_workspace->OpenTakeDitch(std::move(intent.TakeStation), std::move(intent.Take), accepted ? accepted->Revision : 0u,
		ActionReceiver::shared_from_this());
}

void Scene::_CancelTakeDitch()
{
	std::scoped_lock lock(_sceneMutex);
	_workspace->TakeDitchTargetLocked();
}

void Scene::_ConfirmTakeDitch()
{
	std::shared_ptr<LoopTake> ditched;
	{
		std::scoped_lock lock(_sceneMutex);
		auto target = _workspace->TakeDitchTargetLocked();
		auto station = std::move(target.Station);
		auto take = std::move(target.Take);
		const auto accepted = _rigCoordinator.Accepted();
		if (!accepted || accepted->Revision != target.Revision) return;
		if (!station || !take || std::find(_stations.begin(), _stations.end(), station) == _stations.end()) return;
		if (station->DitchTake(take))
		{
			take->DeSelect();
			_workspace->CollapseRacksLocked(_stations);
			_workspace->InvalidateHover2d();
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
	if (!accepted || _operation.Current() != ControlOperation::None || _operation.HoldsWriter() || !_rigCoordinator.EditsEnabled() || _rigSaveDialogOpen || _rigSaveInProgress) return;
	const auto validation = _stationManagement.Validate(_stations, accepted->Revision, consent);
	if (validation == StationManagement::Validation::Confirm)
	{
		_workspace->OpenStationRemoval(_stationManagement.Target->Name(), ActionReceiver::shared_from_this());
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
		_workspace->SetFeedback("Station change failed. Please try again.");
		return;
	}
	if (!_stationManagement.Committed || !_stationManagement.Candidate) return;
	_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
	_performanceGroups.CancelPending();
	_audioEngine->PublishGroupMuteApplication(nullptr);
	const auto removed = _stationManagement.Target;
	const auto affectedBindings = _stationManagement.Candidate->TriggerRouteUpdates.size();
	if (removed)
	{
		_quantisation.DetachStationMaster(removed);
		removed->ClearPointerState();
		removed->DeSelect();
		for (const auto& take : removed->GetLoopTakeSnapshot()) { take->DeSelect(); take->DetachGuiOwnershipForRetirement(); }
		removed->DetachGuiOwnershipForRetirement();
		_retiredStations.push_back(removed);
	}
	_stations = *_stationManagement.Candidate->Stations;
	for (size_t i = 0u; i < _stations.size(); ++i) _stations[i]->SetIndex(static_cast<unsigned int>(i));
	_workspace->CompleteMembershipPresentationLocked(_stations, removed, _stationManagement.Added, affectedBindings);
	_PublishAudioStations();
	_inputSubsystem->OpenRigTriggerInput(_rigCoordinator.Accepted()->Revision);
	_stationManagement.Finish();
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
		_operation.Current() != ControlOperation::None || _workspace->PopupOpen() || _isSceneQuitting.load(std::memory_order_acquire)) return {};
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
void Scene::Draw(DrawContext& ctx)
{
	_workspace->UpdateEditorUi();
	std::scoped_lock lock(_sceneMutex);
	if (_stationManagement.Busy()) _workspace->SetFeedback("Applying station change...");
	gui::Workspace::OverlayPresentation presentation;
	presentation.RigSaveEnabled = _rigCoordinator.EditsEnabled() && !_stationManagement.Busy() && !_rigSaveInProgress && !_rigSaveDialogOpen;
	presentation.QuantisationFeedback = _workspace->QuantisationFeedback();
	presentation.ForcedChannel = _inputSubsystem->ForcedChannelOverride();
	presentation.Stream = _audioEngine->GetStreamParams();
	for (auto channel = 0u; channel < presentation.Stream.NumInputChannels; ++channel)
		presentation.AudioPeaks.push_back(_audioEngine->GetAdcPeak(channel));
	for (const auto& name : _midiState.load(std::memory_order_acquire)->ConnectedIds)
		presentation.MidiPeaks.push_back(_inputSubsystem->ConsumeMidiInputPeak(name));
	const auto tempo = _quantisation.CurrentTempoStatus(_CurrentSampleRate());
	if (tempo) presentation.Tempo = graphics::Compositor::Tempo{ tempo->Bpm, tempo->Bpi };
	_workspace->DrawOverlayLocked(ctx, _stations, _workspace->Editor(), _quantisationInteraction, presentation);
}

void Scene::DrawBackground(DrawContext& ctx)
{
	std::scoped_lock lock(_sceneMutex);
	_workspace->DrawBackground(ctx);
}

void Scene::Draw3d(DrawContext& ctx, unsigned int numInstances, base::DrawPass pass)
{
	std::scoped_lock lock(_sceneMutex);
	if (pass == PASS_SCENE) _quantisation.ApplyOverlayAlpha(_quantisation.OverlayAlpha(Timer::GetTime()), _stations);
	_workspace->DrawWorldLocked(ctx, pass, _stations, _workspace->Editor(), _quantisationInteraction,
		_performanceGroups.Groups(), { _HasSelection(), _workspace->BackgroundPreviewActive({ _sceneMutex, _stations }, _WorkspaceInputPresentation()), _workspace->BackgroundSelectionPreviewActive({ _sceneMutex, _stations }, _WorkspaceInputPresentation()), _workspace->EditControlsSuppressed() });
}

void Scene::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	std::scoped_lock lock(_sceneMutex);
	_workspace->InitResourcesLocked(resourceLib, forceInit, _workspace->Editor(), _stations);
	ResourceUser::_InitResources(resourceLib, forceInit);
}

void Scene::_ReleaseResources()
{
	{ std::scoped_lock lock(_sceneMutex); for (const auto& station : _stations) station->FinishPresentationMove(); }
	_workspace->ReleaseResources(_workspace->Editor());
	for (const auto& station : _retiredStations) station->ReleaseResources();
	for (auto& station : SnapshotStations()) station->ReleaseResources();
	Drawable::_ReleaseResources();
}

void Scene::AddChild(std::shared_ptr<base::GuiElement> child)
{
	_workspace->AddChild(std::move(child));
	_workspace->InvalidateHover2d();
}

gui::Workspace::InputPresentation Scene::_WorkspaceInputPresentation() const
{
	auto stream = _audioEngine->GetStreamParams();
	stream.SampleRate = _CurrentSampleRate();
	return { _userConfig, std::move(stream), _isSceneQuitting.load(std::memory_order_acquire),
		_operation.HoldsWriter(), _rigSaveDialogOpen, _quantisationInputResetRequested.load(std::memory_order_acquire), IsUiVerbose() };
}

void Scene::_CompleteWorkspaceInput()
{
	_ApplyRemoteTempoPromptDecision();
	auto commands = _workspace->TakeInputCommands();
	if (commands.CancelTakeDitch) _CancelTakeDitch();
	if (commands.CancelStationRemoval) { std::scoped_lock lock(_sceneMutex); if (!_stationManagement.Busy()) _stationManagement.Cancel(); }
	if (commands.MuteAll)
	{
		std::scoped_lock lock(_sceneMutex);
		_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
		_performanceGroups.SetAllMuted(*commands.MuteAll, _stations,
			[this](auto application) { _PublishGroupMuteApplication(std::move(application)); });
	}
	if (commands.StationControl)
	{
		if (commands.StationControl->Take) _RequestTakeDitch(std::move(*commands.StationControl));
		else _RequestStationControl(std::move(*commands.StationControl));
	}
}

ActionResult Scene::OnAction(TouchAction action)
{
	const auto result = _workspace->OnTouch(action, { _sceneMutex, _stations }, _WorkspaceInputPresentation());
	_CompleteWorkspaceInput();
	return result;
}

ActionResult Scene::OnAction(TouchMoveAction action)
{
	const auto result = _workspace->OnMove(action, { _sceneMutex, _stations }, _WorkspaceInputPresentation());
	_CompleteWorkspaceInput();
	return result;
}

void Scene::SetHover3d(std::vector<unsigned char> path, Action::Modifiers modifiers, std::optional<utils::Position2d> pointerPosition)
{
	_workspace->SetHover3d(std::move(path), modifiers, pointerPosition, { _sceneMutex, _stations }, _WorkspaceInputPresentation());
}

void Scene::ApplyDeferredHoverUpdates()
{
	_workspace->ApplyDeferredHoverUpdates({ _sceneMutex, _stations }, _WorkspaceInputPresentation());
}

ActionResult Scene::OnAction(KeyAction action)
{
	const bool freshLearnKey = _workspace->PrepareKeyboard(action, { _sceneMutex, _stations });
	const bool contextual = _rigSaveDialogOpen || _operation.HoldsWriter() ||
		_workspace->PopupOpen() || _workspace->IsEditingText() || _workspace->Editor().IsEngaged();
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
	if (_workspace->OwnsBackgroundClick() && action.KeyActionType == KeyAction::KEY_DOWN && action.KeyChar == 27u)
	{
		_workspace->CancelBackgroundClick();
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	// A consumed physical press keeps its release even when focus changes.
	if (_performanceGroups.ConsumeRelease(action)) return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	if (_rigSaveDialogOpen || _operation.HoldsWriter() || _workspace->PopupOpen() || _workspace->IsEditingText() || _workspace->Editor().IsEngaged())
		_performanceGroups.BlockContextualPress(action);
	if (_isSceneQuitting.load(std::memory_order_acquire)) return ActionResult::NoAction();
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	action.SetAudioParams(_audioEngine->GetStreamParams());

	if (contextual && action.KeyActionType == KeyAction::KEY_UP)
	{
		// These shortcut owners admitted their holds before a modal/text context.
		// Cleanup is explicit and never invokes ordinary trigger matching.
		std::scoped_lock lock(_sceneMutex);
		if (action.KeyChar == VK_INSERT) _inputSubsystem->HandleAutomationKey(action, _stations, {}, {});
		if (action.KeyChar == VK_PRIOR || action.KeyChar == VK_NEXT) _inputSubsystem->HandleChannelOverrideKey(action, _stations);
		if ((action.KeyChar == 192u || action.KeyChar == 96u) && _workspace->Hud()) _workspace->Hud()->SetCableRevealHeld(false);
	}
	const auto result = _workspace->OnKeyboard(action, { _sceneMutex, _stations }, _WorkspaceInputPresentation(),
		contextual, suppressedKey, freshLearnKey, *_inputSubsystem,
		[this, contextual, suppressedKey](gui::Workspace::KeyboardCommand command, const KeyAction& key, bool groupsEnabled) {
			return _HandleKeyboardCommand(command, key, groupsEnabled, contextual, suppressedKey);
		});
	_CompleteWorkspaceInput();
	return result;
}

std::optional<ActionResult> Scene::_HandleKeyboardCommand(gui::Workspace::KeyboardCommand command, const KeyAction& action,
	bool groupsEnabled, bool contextual, bool suppressedKey)
{
	if (command == gui::Workspace::KeyboardCommand::TapTempo) { _HandleTapTempo(action.GetActionTime()); return std::nullopt; }

	if (auto overrideRes = _inputSubsystem->HandleChannelOverrideKey(action, SnapshotStations());
		overrideRes.IsEaten)
	{
		_workspace->Settings().RefreshChannelPresentation(_inputSubsystem->ForcedChannelOverride());
		return overrideRes;
	}

	if (groupsEnabled)
	{
		std::scoped_lock lock(_sceneMutex);
		_performanceGroups.Acknowledge(_audioEngine->GroupMuteReceipt());
		if (_performanceGroups.HandleKey(action, _stations, _GroupSelectionDepthLocked(),
			[this](auto application) { _PublishGroupMuteApplication(std::move(application)); }))
		{
			if (_workspace->SelectionPanel()) _workspace->SelectionPanel()->RefreshPerformanceGroups(_performanceGroups.Groups());
			return ActionResult{ true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
		}
	}

	if ((82 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		_HandleReclockArm();
		return ActionResult::NoAction();
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
		auto hovered = _ChildFromPath(_workspace->CurrentHover());
		auto hoveredTake = std::dynamic_pointer_cast<LoopTake>(hovered);
		std::scoped_lock lock(_sceneMutex);
		auto automationRes = _inputSubsystem->HandleAutomationKey(action,
			_stations,
			_workspace->CurrentHover(),
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

	if (result.IsEaten) return result;
	return std::nullopt;

}

void Scene::SetRelativePointerHost(std::function<bool(int, utils::Position2d)> begin,
	std::function<void(int)> end, std::function<float()> logicalPixelScale)
{
	_workspace->SetRelativePointerHost(std::move(begin), std::move(end), std::move(logicalPixelScale),
		{ _sceneMutex, _stations }, _WorkspaceInputPresentation());
}
