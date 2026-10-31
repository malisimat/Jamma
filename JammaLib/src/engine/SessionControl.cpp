#include "SessionControl.h"
#include "../io/IoInputSubsystem.h"
#include "../utils/PathUtils.h"
#include <iostream>
#include <sstream>

using namespace audio;
using namespace io;
using namespace midi;
using namespace utils;

using namespace engine;

void SessionControl::RequestBoundary(io::IoInputSubsystem& input, std::uint64_t revision)
{
	_revision = revision;
	if (!input.RequestCloseRigTriggerInputFromUi(_revision)) return;
	_inputClosed = true;
	_admissionPending.store(true, std::memory_order_release);
	++_identity;
	// The job owner acknowledges on its next turn; no wait under a caller lock.
}

bool SessionControl::TryAcquireBoundary(io::IoInputSubsystem& input)
{
	if (!input.RigTriggerInputReadyForAudioBoundary(_revision)) return false;
	_writer = std::unique_lock<std::mutex>(_lifecycleMutex, std::try_to_lock);
	return _writer.owns_lock();
}

bool SessionControl::PauseAudio(audio::AudioHost& audio, std::string& error)
{
	if (!audio.BeginQuiescence(_pause, error)) return false;
	_paused = true;
	return true;
}

SessionControl::AudioRestoration SessionControl::RestoreAudio(audio::AudioHost& audio, std::string& error)
{
	const bool success = !_paused || audio.EndQuiescence(_pause, error);
	const bool admissionRestored = _paused && success && _pause.AdmissionWasOpen;
	_paused = false;
	return {success, admissionRestored};
}

void SessionControl::Finish(io::IoInputSubsystem& input, std::optional<std::uint64_t> acceptedRevision)
{
	if (_inputClosed && acceptedRevision) input.OpenRigTriggerInput(*acceptedRevision);
	_inputClosed = false;
	ReleaseWriter();
	_kind = None;
	_loadReady = false;
	_admissionPending.store(false, std::memory_order_release);
}

void SessionControl::FinishReplacement()
{
	// Replacement keeps the old input gate closed and abandons its paused stream.
	_paused = _loadReady = false;
	ReleaseWriter();
	_kind = None;
}

void SessionControl::Bind(audio::AudioHost& audio, io::IoInputSubsystem& input, RigCoordinator& rig,
	io::UserConfig& config, std::uint64_t sceneIdentity)
{
	_audio = &audio; _input = &input; _rig = &rig; _config = &config; _sceneIdentity = sceneIdentity;
}

void SessionControl::_RefreshMidiIfNeeded(StationAccess access, const Effects& effects, bool quitting)
{
	if (!_midiActive.load(std::memory_order_acquire) ||
		quitting)
		return;
	const auto now = std::chrono::steady_clock::now();
	const bool requested = _midiRefreshRequested.exchange(false, std::memory_order_acq_rel);
	if (!_midiRefreshPolicy.ShouldPoll(now, requested)) return;
	const auto inventory = _input->InventoryMidiInputs();
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
		quitting)
		return;
	auto result = _input->RefreshMidi(_audio->GetMidiClockAnchor_Ref(),
		_audio->GetStreamParams().SampleRate, inventory, requested);
	const auto connectedIds = result.ConnectedRegistryIds;
	const auto serialIds = _input->ConnectedSerialDeviceIds();
	ConfirmMidiConnectionChange(previous->Connection, result, connectedIds, access);
	_midiState.store(std::make_shared<const MidiState>(MidiState{ std::move(result), connectedIds, serialIds }),
		std::memory_order_release);
	const auto inputs = _audio->GetStreamParams().NumInputChannels;
	std::scoped_lock sceneLock(access.Mutex);
	effects.ApplyRuntimeAvailabilityLocked({inputs, connectedIds, serialIds});
}

void SessionControl::ConfirmMidiConnectionChange(const midi::MidiConnectionResult& previous, const midi::MidiConnectionResult& refreshed, const std::vector<std::string>& connectedIds, StationAccess access)
{
	if (!refreshed.Inventory.Error.empty()) return;
	const auto lostIds = midi::MidiDevice::LostConnectedInputIds(previous, refreshed);
	std::scoped_lock lock(access.Mutex);
	const auto accepted = _rig->Accepted();
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
	for (const auto& station : access.Stations)
		if (station)
			for (const auto& name : lostIds) station->FlushLiveHeldMidiNotesForDevice(name);
}

void SessionControl::RecoverSourcesLocked(StationAccess access, bool membershipBusy, bool quitting)
{
	if (quitting) return;
	for (const auto& station : access.Stations)
		if (station) station->FlushPendingLostLiveMidiNotes();
	const auto accepted = _rig->Accepted();
	if (!accepted)
	{
		// Reset can remove the rig while this job owns admission closure.
		if (_sourceLossRecoveryGateClosed && _audio->CallbackAdmissionIsQuiescent() &&
			_audio->ReopenCallbackAdmission()) _sourceLossRecoveryGateClosed = false;
		return;
	}
	const auto pending = [this, &accepted, membershipBusy]() {
		return membershipBusy || std::any_of(accepted->Triggers.begin(), accepted->Triggers.end(),
			[](const auto& runtime) { return runtime.Instance && runtime.Instance->HasPendingSourceLoss(); });
	};
	const auto publicationNeedsAcknowledgement = [&access]() {
		return std::any_of(access.Stations.begin(), access.Stations.end(),
			[](const auto& station) { return station && station->HasUnacknowledgedAudioState(); });
	};
	const auto heartbeat = _audio->AudioCallbackHeartbeat();
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
		_audio->CloseCallbackAdmission();
		_sourceLossRecoveryGateClosed = true;
	}
	if (!_audio->CallbackAdmissionIsQuiescent()) return;
	_audio->ApplyPendingRigWhileQuiescent();
	const auto revision = _audio->AppliedRigRevision();
	const auto streamParams = _audio->GetStreamParams();
	for (const auto& runtime : accepted->Triggers)
		if (runtime.Instance)
		{
			runtime.Instance->OnTick(Timer::GetTime(), 0u, (*_config), streamParams, revision);
			runtime.Instance->ProcessStructuralActionsOnJob((*_config), streamParams);
			runtime.Instance->OnTick(Timer::GetTime(), 0u, (*_config), streamParams, revision);
		}
	_audio->ApplyPendingRigWhileQuiescent();
	// The gate is the reader acknowledgement when a stopped driver cannot
	// observe newly published removals. External target pins do not hold it shut.
	for (const auto& station : access.Stations)
		if (station)
		{
			station->AcknowledgeAudioBoundary();
			station->ReleaseRetiredAudioStates();
		}
	if (!pending() && _audio->ReopenCallbackAdmission()) _sourceLossRecoveryGateClosed = false;
}

void SessionControl::PumpMidi(StationAccess access)
{
	std::scoped_lock midiLock(_midiLifecycleMutex);
	if (!_midiActive.load(std::memory_order_acquire)) return;
	auto stations = access.Snapshot();
	_input->PumpMidi(stations, _audio->GetAudioSampleCounter(),
		_audio->GetStreamParams(), access.Mutex);
}

void SessionControl::PumpSerial(StationAccess access)
{
	std::scoped_lock midiLock(_midiLifecycleMutex);
	if (!_midiActive.load(std::memory_order_acquire)) return;
	_input->PumpSerial(access.Stations, _audio->GetStreamParams(), access.Mutex);
}

bool SessionControl::InitAudio(const std::shared_ptr<ninjam::NinjamController>& ninjam, const std::shared_ptr<utils::Timer>& clock, bool generatedRig, const audio::AsioInventory* inventory, bool exactSelection, StationAccess access, const Effects& effects)
{
	if (inventory) _deviceInventory = *inventory;
	_devicePageDirty = true;
	const auto previousMidi = _midiState.load(std::memory_order_acquire);
	// Setup audio engine which starts device
	bool started = false;
	{
		std::scoped_lock lock(access.Mutex);
		_audio->CloseCallbackAdmission();
		_sourceLossRecoveryGateClosed = true;
		started = _audio->InitWithBlockAdvancement(ninjam,
			clock, generatedRig, inventory, exactSelection);
		if (!started)
		{
			const auto& report = _audio->GetAsioOpenReport();
			_Notice("Audio device",report.Attempts.empty() ? "Audio device could not be started; retry in settings" : report.Attempts.back().Error);
		}
		if (_audio->GetAsioInventory().EnumerationError.empty()) _deviceInventory = _audio->GetAsioInventory();
		if (!started && !_audio->LastInitReplacedStream())
		{
			// Stop failure is unknown availability, not a confirmed replacement.
			// Preserve the existing capture identities and MIDI connections.
			if (_audio->CallbackAdmissionIsQuiescent() &&
				_audio->ReopenCallbackAdmission()) _sourceLossRecoveryGateClosed = false;
			return false;
		}
		// Replacing the stream invalidates the common sample clock even when its
		// display name/channels remain unchanged. New callbacks stay gated until
		// prior captures have settled through the usual structural result protocol.
		const auto actual = _audio->GetStreamParams();
		if (started)
		{
			auto settings = _submittedAudio.Value;
			settings.Name = actual.Name;
			settings.SampleRate = actual.SampleRate;
			settings.BufSize = actual.BufSize;
			settings.NumBuffers = actual.NumBuffers;
			settings.NumChannelsIn = actual.NumInputChannels;
			settings.NumChannelsOut = actual.NumOutputChannels;
			_submittedAudio.Value = settings;
			_presentation.AudioAccepted = Candidate<io::UserConfig::AudioSettings>{std::move(settings), _submittedAudio.Revision};
		}
		++_audioStreamEpoch;
		if (const auto accepted = _rig->Accepted())
			for (const auto& runtime : accepted->Triggers)
				if (runtime.Instance) runtime.Instance->ConfirmAudioAvailabilityOnJob(
					_audioStreamEpoch, started ? actual.Name : std::string(), started ? actual.NumInputChannels : 0u);
	}

	if (started) {
		InitMidi(generatedRig);

	}
	else
	{
		_midiInputInventory.reset();
		CloseMidi();
		_midiState.store( std::make_shared<const MidiState>(), std::memory_order_release);
	}
	const auto midiState = _midiState.load(std::memory_order_acquire);
	const auto actualInputs = started ? _audio->GetStreamParams().NumInputChannels : 0u;
	ConfirmMidiConnectionChange(previousMidi->Connection, midiState->Connection, midiState->ConnectedIds, access);
	std::optional<io::UserConfig::AudioSettings> confirmedGeneratedAudio;
	if (started && generatedRig)
	{
		const auto actual = _audio->GetStreamParams();
		auto settings = (*_config).Audio;
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
	{ std::scoped_lock lock(access.Mutex); effects.ApplyRuntimeAvailabilityLocked({actualInputs, midiState->ConnectedIds, midiState->ConnectedSerialIds, false, true, true, std::nullopt, std::move(confirmedGeneratedAudio)}); }

	return true;
}

void SessionControl::CloseAudio()
{
	CloseMidi();
	_audio->Close();
}

bool SessionControl::PauseAudio()
{
	auto* device = _audio ? _audio->GetDevice() : nullptr;
	return device && device->Pause();
}

bool SessionControl::ResumeAudio()
{
	auto* device = _audio ? _audio->GetDevice() : nullptr;
	return device && device->Resume();
}

bool SessionControl::_ValidateAudioDraft(std::string& error, const Effects& effects) const
{
	const auto found = std::find_if(_deviceInventory.Devices.begin(), _deviceInventory.Devices.end(),
		[this](const auto& device) { return device.Name == _submittedAudio.Value.Name; });
	// Missing/unprobed entries can be stale after a device powers on. The exact
	// selection is validated again against fresh discovery after closing the stream.
	const bool probed = found != _deviceInventory.Devices.end() && found->Probed;
	if ((!_submittedAudio.Value.NumChannelsIn && !_submittedAudio.Value.NumChannelsOut) ||
		(probed && (_submittedAudio.Value.NumChannelsIn > found->InputChannels || _submittedAudio.Value.NumChannelsOut > found->OutputChannels))) { error = "Invalid input/output channel counts"; return false; }
	if (!_submittedAudio.Value.BufSize || !_submittedAudio.Value.SampleRate ||
		(probed && std::find(found->SampleRates.begin(), found->SampleRates.end(), _submittedAudio.Value.SampleRate) == found->SampleRates.end())) { error = "Unsupported sample rate or invalid buffer request"; return false; }
	const auto actual = _audio->GetStreamParams();
	if (effects.CheckEligibility().HasTakes && _submittedAudio.Value.SampleRate != (actual.SampleRate ? actual.SampleRate : (*_config).Audio.SampleRate)) { error = "Sample rate cannot change while any takes exist"; return false; }
	return true;
}

void SessionControl::RequestReset(const Effects& effects)
{
	if (Current() != None || HoldsWriter() || !_rig->EditsEnabled() || effects.CheckEligibility().StructuralBusy)
	{ ReportFileOutcome("Another operation is busy; retry Reset All"); return; }
	Begin(ResetSession);
	ReportFileStatus("Waiting to reset session");
}

void SessionControl::RequestSave(bool chooseFolder, bool asDefault, const Effects& effects)
{
	if (Current() != SessionControl::None || !_rig->EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	const auto eligibility = effects.CheckEligibility();
	if (eligibility.StructuralBusy) { ReportFileOutcome("Another operation is busy; retry Save"); return; }
	if (eligibility.CaptureBusy) { ReportFileOutcome("Save unavailable during capture or pending actions; retry Save"); return; }
	if (asDefault && (_defaultJamPath.empty() || !_defaultJamSaver)) { ReportFileOutcome("Startup jam path unavailable"); return; }
	Begin(SessionControl::Save);
	auto folder = asDefault && _currentJamPath.empty() ? utils::GetParentDirectory(_defaultJamPath) : _sessionSaveFolder;
	if (chooseFolder || folder.empty())
	{
		try { folder = _pickSessionDirectory ? _pickSessionDirectory(utils::GetParentDirectory(_currentJamPath))
			: utils::PickDirectory(L"Choose session save folder", utils::GetParentDirectory(_currentJamPath)); }
		catch (const std::exception& error)
		{ Begin(None); ReportFileOutcome(std::string("Save folder selection failed: ") + error.what()); return; }
	}
	if (folder.empty()) { Begin(SessionControl::None); return; }
	_pendingSaveAsDefault = asDefault; _pendingSaveFolder = std::move(folder); Begin(SessionControl::Save);
	ReportFileStatus("Waiting to capture session");
}

void SessionControl::CompleteSessionSave(bool success, const std::wstring& path, const std::string& error, std::wstring folder, bool asDefault)
{
	if (!success) { ReportFileOutcome(error); return; }
	_sessionSaveFolder = std::move(folder);
	// The manifest is already committed; updating the startup default is a
	// separate operation and cannot revoke this session's successful destination.
	_currentJamPath = path;
	if (asDefault)
	{
		if (_defaultJamSaver(path, true)) ReportFileOutcome("Saved startup jam " + utils::EncodeUtf8(path));
		else ReportFileOutcome("Jam saved at " + utils::EncodeUtf8(path) + "; startup default unchanged");
	}
	else ReportFileOutcome("Saved " + utils::EncodeUtf8(path));
}

void SessionControl::PickSessionForLoad(const Effects& effects)
{
	if (Current() != SessionControl::None || !_rig->EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	if (effects.CheckEligibility().CaptureBusy) { ReportFileOutcome("Load unavailable during capture or pending actions"); return; }
	Begin(SessionControl::Load);
	try { _pendingLoadPath = utils::PickJamFile(); }
	catch (...) { Begin(SessionControl::None); throw; }
	if (_pendingLoadPath.empty()) Begin(SessionControl::None);
}

void SessionControl::RequestSessionLoad(std::wstring path, const Effects& effects)
{
	if (path.empty()) return;
	if (Current() != SessionControl::None || !_rig->EditsEnabled()) { ReportFileOutcome("Another operation is busy"); return; }
	if (effects.CheckEligibility().CaptureBusy) { ReportFileOutcome("Load unavailable during capture or pending actions"); return; }
	_pendingLoadPath = std::move(path); Begin(SessionControl::Load);
}

std::wstring SessionControl::TakeReadySessionLoad()
{
	return LoadReady() ? std::exchange(_pendingLoadPath, {}) : std::wstring();
}

bool SessionControl::FinishOperation(std::string* restorationError, StationAccess access)
{
	if (Current() == SessionControl::Audio) _audioSchedule.NextChange = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
	std::string error;
	const auto audioRestore = RestoreAudio(*_audio, error);
	bool restored = audioRestore.Success;
	if (!restored)
	{
		if (restorationError) *restorationError = error;
		_SetDeviceStatus("Stopped: " + error); ReportFileOutcome("Audio restoration failed: " + error);
	}
	if (audioRestore.AdmissionRestored)
	{
		std::scoped_lock lock(access.Mutex);
		_sourceLossRecoveryGateClosed = false;
	}
	try { _input->ResumeMidiDispatch(); }
	catch (const std::exception& dispatchError)
	{
		restored = false;
		if (restorationError) *restorationError += "; MIDI resume failed: " + std::string(dispatchError.what());
		_SetDeviceStatus("MIDI dispatch stopped: " + std::string(dispatchError.what()));
		_Notice("MIDI", "MIDI dispatch stopped: " + std::string(dispatchError.what()));
	}
	std::optional<std::uint64_t> acceptedRevision;
	if (InputClosed())
		if (const auto accepted = _rig->Accepted()) acceptedRevision = accepted->Revision;
	Finish(*_input, acceptedRevision);
	return restored;
}

void SessionControl::FinishSessionLoad(bool replaced, const std::string& error, StationAccess access)
{
	if (replaced)
	{
		_sessionReplacing.store(true, std::memory_order_release);
		FinishReplacement();
	}
	else { ReportFileOutcome(error.empty() ? "Load failed; current session retained" : error); FinishOperation(nullptr, access); }
}

void SessionControl::PumpOnWindow(StationAccess access, const Effects& effects, bool quitting)
{
	if (quitting) return;
	const bool deviceChange = _audioChangeHintRequested.exchange(false, std::memory_order_acq_rel) && !_manualAudioRefresh;
	if (_audio->ObserveBackendReset(deviceChange))
	{
		_audioResetPending = true;
		_SetDeviceStatus("Audio driver requested reset; suspending unavailable device");
		std::cout << "[ASIO] driver requested reset; suspending stream on window lifecycle owner" << std::endl;
	}
	const auto eligibility = effects.CheckEligibility();
	const auto now = std::chrono::steady_clock::now();
	const auto actual = _audio->GetStreamParams();
	_UpdateWindowPresentation(actual, eligibility.StructuralBusy);
	if (!_ScheduleWindowOperation(now, actual, effects, eligibility.StructuralBusy)) return;
	if (Current() == Midi) { _CompleteMidiRequest(); return; }
	if (_AdmitBoundary(access, now)) _ApplyAdmittedOperation(actual, now, access, effects);
}

void SessionControl::_UpdateWindowPresentation(const audio::AudioStreamParams& actual, bool inventoryBusy)
{
	const bool filesEnabled = !inventoryBusy && Current() == SessionControl::None && _rig->EditsEnabled();
	_SetSessionEnabled(filesEnabled, filesEnabled && !_defaultJamPath.empty() && static_cast<bool>(_defaultJamSaver));
	if (actual.BufSize && !_acceptedBufferSizes.contains(actual.Name)) { _acceptedBufferSizes[actual.Name] = {actual.BufSize}; _devicePageDirty = true; }
	if (_devicePageDirty || _displayedMidiState != _midiState.load(std::memory_order_acquire)) _RefreshPresentation();
	std::ostringstream text;
	text << actual.Name << " | " << actual.SampleRate << " Hz | " << actual.NumInputChannels << " in / " << actual.NumOutputChannels << " out | buffer " << actual.BufSize;
	_SetActiveAudioStatus(text.str());
}

bool SessionControl::_ScheduleWindowOperation(DeviceSelectionSchedule::Clock::time_point now, const audio::AudioStreamParams& actual, const Effects& effects, bool inventoryBusy)
{
	if (Current() == SessionControl::None)
	{
		// A lost source must settle captures through their existing recovery path;
		// recording eligibility cannot postpone closing its dead stream forever.
		if (_audioResetPending || (_audioUnavailablePresentationPending && _rig->EditsEnabled())) { Begin(AudioReset); return true; }
		if (inventoryBusy || !_rig->EditsEnabled()) return false;
		if (_midiDraftPending)
		{
			Begin(SessionControl::Midi); _midiDraftPending = false;
			_admittedMidiRevision = _submittedMidi.Revision;
			NextIdentity();
			_pendingMidiDeviceRequest.store(std::make_shared<const MidiDeviceRequest>(MidiDeviceRequest{_sceneIdentity, Identity(), _submittedMidi.Revision, _submittedMidi.Value}), std::memory_order_release);
		}
		else if (_audioSchedule.Revision != _audioSchedule.AttemptedRevision && now >= _audioSchedule.NextChange)
		{
			if (now < _audioSchedule.EditDeadline) { _SetDeviceStatus("Settling audio settings (3 seconds)"); return false; }
			if (effects.CheckEligibility().CaptureBusy) { _SetDeviceStatus("Waiting for recording/pending capture"); return false; }
			if (_audio->HasMatchingRunningStream(_submittedAudio.Value))
			{
				_audioSchedule.AttemptedRevision = _audioSchedule.Revision;
				_SetDeviceStatus("Audio device ready");
				return false;
			}
			std::string error;
			if (!_ValidateAudioDraft(error, effects)) { _SetDeviceStatus(error); _Notice("Audio device",error); _audioSchedule.AttemptedRevision = _audioSchedule.Revision; return false; }
			Begin(SessionControl::Audio);
		}
		else if (_audioRefreshRequested.load(std::memory_order_acquire))
		{
			if (effects.CheckEligibility().CaptureBusy || RigSaveInProgress() || RigSaveDialogOpen()) return false;
			if (!_ScheduleAudioRefresh(_audio->HasOpenStream())) return false;
		}
		else return false;
	}
	return true;
}

bool SessionControl::_ScheduleAudioRefresh(bool streamOpen)
{
	// Notifications never interrupt an open stream. Explicit Rescan uses the
	// admitted Reconfigure path, which closes it before ASIO discovery.
	_audioRefreshRequested.exchange(false, std::memory_order_acq_rel);
	if (streamOpen && !_manualAudioRefresh)
	{
		_SetDeviceStatus("Device change detected; Rescan audio devices to refresh the list");
		return false;
	}
	// With no open stream, both requests recover the configured preferred card.
	Begin(std::exchange(_manualAudioRefresh, false) && streamOpen ? AudioRescan : AudioRefresh);
	_SetDeviceStatus("Waiting to rescan audio devices");
	return true;
}

void SessionControl::_CompleteMidiRequest()
{
	if (const auto completion = _completedMidiDeviceRequest.exchange({}, std::memory_order_acq_rel))
	{
		if (completion->SceneId != _sceneIdentity || completion->OperationId != Identity() || completion->DraftRevision != _admittedMidiRevision) return;
		if (completion->Accepted)
		{
			// Keep the shell's user configuration aligned with the accepted rig so
			// an explicit File-tab save includes the latest MIDI selection.
			_config->Midi.Devices = completion->Desired;
			_presentation.MidiAccepted = Candidate<std::vector<io::UserConfig::MidiSettings>>{completion->Desired, completion->DraftRevision};
		}
		_SetDeviceStatus(completion->Status); Begin(SessionControl::None); _devicePageDirty = true;
		_Notice("MIDI devices",completion->Status);
	}
	return;

}

bool SessionControl::_AdmitBoundary(StationAccess access, DeviceSelectionSchedule::Clock::time_point now)
{
	if (LoadReady()) return false;
	if (Current() == SessionControl::Audio && now < _audioSchedule.EditDeadline) { FinishOperation(nullptr, access); return false; }
	if (!InputClosed())
	{
		const auto accepted = _rig->Accepted();
		if (!accepted || (Current() != AudioReset && !_rig->EditsEnabled())) return false;
		RequestBoundary(*_input, accepted->Revision);
		return false; // Job acknowledges on its next turn; never wait under an application lock.
	}
	return TryAcquireBoundary(*_input);
}

void SessionControl::_ApplyAdmittedOperation(const audio::AudioStreamParams& actual, DeviceSelectionSchedule::Clock::time_point now, StationAccess access, const Effects& effects)
{
	try
	{
		std::string error;
		if (Current() == AudioReset)
		{
			// The job owner may have authorized a reopen before observing reset.
			// Reclose under the acquired lifecycle writer before checking readers.
			_audio->CloseCallbackAdmission();
			// Observation already closed engine admission. Yield this frame while
			// an admitted callback leaves; never spin with the lifecycle writer held.
			if (!_audio->CallbackAdmissionIsQuiescent()) { ReleaseWriter(); return; }
			if (!_audioResetPending)
			{
				const auto midi = MidiSnapshot();
				{
					std::scoped_lock lock(access.Mutex);
					effects.ApplyRuntimeAvailabilityLocked({0u, midi->ConnectedIds, midi->ConnectedSerialIds, true, false, true});
					_audioUnavailablePresentationPending = false;
				}
				FinishOperation(nullptr, access);
				return;
			}
			const bool stopped = _audio->SuspendResetStream(error);
			_audioResetPending = false;
			_audioRefreshRequested.store(false, std::memory_order_release);
			const auto midi = MidiSnapshot();
			{
				std::scoped_lock lock(access.Mutex);
				_sourceLossRecoveryGateClosed = true;
				++_audioStreamEpoch;
				if (const auto accepted = _rig->Accepted())
					for (const auto& runtime : accepted->Triggers) if (runtime.Instance)
						runtime.Instance->ConfirmAudioAvailabilityOnJob(_audioStreamEpoch, {}, 0u);
				_audioUnavailablePresentationPending = !_rig->EditsEnabled();
				if (!_audioUnavailablePresentationPending)
					effects.ApplyRuntimeAvailabilityLocked({0u, midi->ConnectedIds, midi->ConnectedSerialIds, true, false, true});
			}
			for (auto& device : _deviceInventory.Devices)
				if (device.Name == actual.Name) { device.Probed = false; device.ProbeError = "Driver requested reset; reconnect and rescan"; }
			// This is loss, not a temporary pause: FinishOperation must not restart it.
			_paused = false;
			FinishOperation(nullptr, access);
			_SetDeviceStatus(stopped ? "Audio device unavailable; reconnect and Rescan audio devices" : error);
			_devicePageDirty = true;
			return;
		}
		if (Current() == SessionControl::Save) { _CaptureSession(actual, access, effects); return; }
		if (Current() == AudioRefresh && _audio->HasOpenStream())
		{
			_SetDeviceStatus("Device change detected; Rescan audio devices to refresh the list");
			FinishOperation(nullptr, access); return;
		}
		if (!PauseAudio(*_audio, error))
		{
			_SetDeviceStatus("Stopped/error: " + error); ReportFileOutcome(error);
			if (Current() == SessionControl::Audio) _audioSchedule.AttemptedRevision = _audioSchedule.Revision;
			FinishOperation(nullptr, access); return;
		}
		_input->SuspendMidiDispatch();
		for (const auto& station : access.Stations) if (station) station->ResetLiveMidiWhileQuiescent();
		// With producers excluded and callbacks drained, capture-start wins deterministically.
		if (Current() == ResetSession)
		{
			if (effects.CheckEligibility().StructuralBusy || !effects.ResetSessionWhileQuiescent)
				ReportFileOutcome("Another operation is busy; retry Reset All");
			else { effects.ResetSessionWhileQuiescent(); ClearJamDestination(); ReportFileOutcome("Session reset"); }
			FinishOperation(nullptr, access); return;
		}
		if (Current() != SessionControl::Midi && effects.CheckEligibility().CaptureBusy)
		{
			if (Current() == AudioRescan) RefreshAudioFromWindow();
			else if (Current() == AudioRefresh) RequestAudioRefresh();
			if (Current() == SessionControl::Audio) _SetDeviceStatus("Waiting for recording");
			else ReportFileOutcome("File operation rejected during capture/pending actions");
			FinishOperation(nullptr, access); return;
		}
		if (Current() == SessionControl::Load) { MarkLoadReady(); ReportFileStatus("Loading session"); return; }

		if (Current() == SessionControl::Audio || Current() == AudioRefresh || Current() == AudioRescan) _ApplyAudioCandidate(actual, now, access, effects, error);
		if (Current() != SessionControl::None) FinishOperation(nullptr, access);
	}
	catch (const std::exception& error)
	{
		ReportFileOutcome(std::string("Operation failed: ") + error.what());
		if (Current() == SessionControl::Audio) _audioSchedule.AttemptedRevision = _audioSchedule.Revision;
		FinishOperation(nullptr, access);
	}

}

void SessionControl::_ApplyAudioCandidate(const audio::AudioStreamParams& actual, DeviceSelectionSchedule::Clock::time_point now, StationAccess access, const Effects& effects, std::string& error)
{
	const auto admittedRevision = _audioSchedule.Revision;
	const bool refresh = Current() == AudioRefresh || Current() == AudioRescan;
	_admittedAudio = refresh ? Candidate<io::UserConfig::AudioSettings>{_config->Audio, _submittedAudio.Revision} : _submittedAudio;
	if (Current() == AudioRescan && actual.SampleRate != 0u)
	{
		// Rescan preserves the current stream, including a startup fallback. Neither
		// the saved preferred device nor an unsaved draft chooses this replacement.
		auto& settings = _admittedAudio.Value;
		settings = _audio->GetUserConfig().Audio;
		settings.Name = actual.Name;
		settings.SampleRate = actual.SampleRate;
		settings.BufSize = actual.BufSize;
		settings.NumBuffers = actual.NumBuffers;
		settings.NumChannelsIn = actual.NumInputChannels;
		settings.NumChannelsOut = actual.NumOutputChannels;
	}
	const auto requested = _admittedAudio.Value;
	if (!refresh) _audioSchedule.Attempted(admittedRevision, now);
	// Recovery opens the configured device against fresh discovery in Reconfigure;
	// startup inventory may still describe the powered-off device as unavailable.
	if (!refresh && !_ValidateAudioDraft(error, effects)) { _SetDeviceStatus(error); _Notice("Audio device",error); }
	else
	{
		_SetDeviceStatus("Changing audio device");
		const bool applied = _audio->Reconfigure(requested, error);
		const auto acceptedParams = _audio->GetStreamParams();
		const auto freshInventory = _audio->GetAsioInventory();
		if (freshInventory.EnumerationError.empty()) _deviceInventory = freshInventory;
		_devicePageDirty = true;
		if (applied) _AcceptAudioCandidate(actual, access, effects, refresh);
		else { _SetDeviceStatus(error); _Notice("Audio device",error); }
		std::cout << "[DEVICES scene=" << _sceneIdentity << " operation=" << Identity() << "] audio " << (applied ? "applied " : "failed ")
			<< "requested='" << requested.Name << "' in=" << requested.NumChannelsIn << " out=" << requested.NumChannelsOut << " rate=" << requested.SampleRate << " buffer=" << requested.BufSize
			<< " actual='" << acceptedParams.Name << "' in=" << acceptedParams.NumInputChannels << " out=" << acceptedParams.NumOutputChannels << " rate=" << acceptedParams.SampleRate << " buffer=" << acceptedParams.BufSize << " " << error << std::endl;
	}

}

void SessionControl::_AcceptAudioCandidate(const audio::AudioStreamParams& actual, StationAccess access, const Effects& effects, bool refresh)
{
	const bool preservePreferences = Current() == AudioRescan && actual.SampleRate != 0u;
	// Rescan retains the previous running/paused state. A selected/recovered
	// replacement starts even when the previous stream was absent.
	if (!preservePreferences) RestoreRunningAudio();
	const auto acceptedParams = _audio->GetStreamParams();
	auto settings = _admittedAudio.Value; settings.Name = acceptedParams.Name; settings.SampleRate = acceptedParams.SampleRate; settings.BufSize = acceptedParams.BufSize; settings.NumBuffers = acceptedParams.NumBuffers; settings.NumChannelsIn = acceptedParams.NumInputChannels; settings.NumChannelsOut = acceptedParams.NumOutputChannels;
	const auto oldRate = actual.SampleRate ? actual.SampleRate : _config->Audio.SampleRate;
	if (!preservePreferences) (*_config).Audio = settings;
	auto& sizes = _acceptedBufferSizes[acceptedParams.Name];
	if (std::find(sizes.begin(), sizes.end(), acceptedParams.BufSize) == sizes.end()) { if (sizes.size() == 16u) sizes.erase(sizes.begin()); sizes.push_back(acceptedParams.BufSize); }
	const auto midiState = _midiState.load(std::memory_order_acquire);
	// No capture is active and all producers are excluded at this boundary.
	++_audioStreamEpoch;
	if (const auto currentRig = _rig->Accepted())
		for (const auto& runtime : currentRig->Triggers) if (runtime.Instance)
			runtime.Instance->ConfirmAudioAvailabilityOnJob(_audioStreamEpoch, acceptedParams.Name, acceptedParams.NumInputChannels);
	// Failed startup skips input initialization entirely, including serial ports.
	// Later audio changes only reprepare MIDI mappings and retain open serial ports.
	const auto refreshedMidi = actual.SampleRate == 0u
		? _input->Init(_audio->GetMidiClockAnchor_Ref(), acceptedParams.SampleRate)
		: _input->RefreshMidi(_audio->GetMidiClockAnchor_Ref(), acceptedParams.SampleRate, _input->InventoryMidiInputs());
	const auto newMidiNames = refreshedMidi.ConnectedRegistryIds;
	const auto serialIds = _input->ConnectedSerialDeviceIds();
	ConfirmMidiConnectionChange(midiState->Connection, refreshedMidi, newMidiNames, access);
	_midiState.store(std::make_shared<const MidiState>(MidiState{refreshedMidi, newMidiNames, serialIds}), std::memory_order_release);
	_midiActive.store(true, std::memory_order_release);
	{ std::scoped_lock lock(access.Mutex); effects.ApplyRuntimeAvailabilityLocked({acceptedParams.NumInputChannels, newMidiNames, serialIds, true, false, true, oldRate}); }
	const bool merged = preservePreferences || _rig->MergeHardwarePreferences(settings, std::nullopt);
	_SetDeviceStatus(preservePreferences ? "Audio devices rescanned; current audio restored" :
		(!merged ? "Applied; rig update unavailable" : (acceptedParams.NumOutputChannels < actual.NumOutputChannels ? "Ready; unavailable output routes disconnected" : "Ready")));
	_Notice("Audio device", !merged ? "Audio device ready, but the rig could not be updated" :
		(acceptedParams.NumOutputChannels < actual.NumOutputChannels ? "Ready; unavailable output routes disconnected" : "Audio device ready"));
	if (!preservePreferences && _submittedAudio.Revision == _admittedAudio.Revision && (!refresh || _submittedAudio.Value == _admittedAudio.Value)) _submittedAudio.Value = settings;
	if (!preservePreferences && (!refresh || _submittedAudio.Value == settings))
		_presentation.AudioAccepted = Candidate<io::UserConfig::AudioSettings>{settings, _admittedAudio.Revision};
	_devicePageDirty = true;
}

void SessionControl::_CaptureSession(const audio::AudioStreamParams& actual, StationAccess access, const Effects& effects)
{
	ReportFileStatus("Capturing/saving session");
	const auto result = effects.CaptureSession(_pendingSaveFolder, actual,
		[this, &access](std::string& restoreError) { return FinishOperation(&restoreError, access); },
		[this, &access, &effects](std::string& eligibilityError) {
			_input->SuspendMidiDispatch();
			for (const auto& station : access.Stations) if (station) station->ResetLiveMidiWhileQuiescent();
			if (effects.CheckEligibility().CaptureBusy) { eligibilityError = "Save unavailable during capture or pending actions"; return false; }
			return true;
		});
	CompleteSessionSave(result.Success, result.Path, result.Error, _pendingSaveFolder, _pendingSaveAsDefault);
	if (!result.AudioResumed) ReportFileOutcome("Save outcome: " + utils::EncodeUtf8(result.Path) + "; audio restoration failed: " + result.Error);
	if (Current() != SessionControl::None) FinishOperation(nullptr, access);
}

void SessionControl::_ReconcileMidiDeviceRequestOnJob(StationAccess access, const Effects& effects, bool quitting)
{
	// OnJobTick holds the lifecycle owner lock; complete values are handed off by
	// UI atomics. Only this job owner mutates MIDI desired selection/endpoints.
	const auto desired = _pendingMidiDeviceRequest.exchange({}, std::memory_order_acq_rel);
	if (!desired) return;
	const auto complete = [this, &desired](std::string status, bool accepted = false, bool saved = false) {
		std::cout << "[MIDI scene=" << desired->SceneId << " operation=" << desired->OperationId << "] " << status << std::endl;
		_completedMidiDeviceRequest.store(std::make_shared<const MidiDeviceCompletion>(MidiDeviceCompletion{
			desired->SceneId, desired->OperationId, desired->DraftRevision, std::move(status), accepted, saved, desired->Desired}),
			std::memory_order_release);
	};
	if (quitting)
	{
		complete("MIDI change cancelled: session is stopping");
		return;
	}
	try
	{
		std::scoped_lock midiLock(_midiLifecycleMutex);
		const auto previous = _midiState.load(std::memory_order_acquire);
		const auto inventory = _input->InventoryMidiInputs();
		auto result = _input->ReconcileMidi(desired->Desired, _audio->GetMidiClockAnchor_Ref(),
			(_audio->GetStreamParams().SampleRate ? _audio->GetStreamParams().SampleRate : (*_config).Audio.SampleRate), inventory);
		if (!result.Accepted)
		{
			const auto error = result.Error.empty() ? (result.Inventory.Error.empty() ? "MIDI reconciliation failed" : result.Inventory.Error) : result.Error;
			const auto surviving = result.ConnectedRegistryIds;
			const auto serialIds = _input->ConnectedSerialDeviceIds();
			ConfirmMidiConnectionChange(previous->Connection, result, surviving, access);
			_midiState.store(std::make_shared<const MidiState>(MidiState{std::move(result), surviving, serialIds}), std::memory_order_release);
			if (surviving != previous->ConnectedIds)
			{
				std::scoped_lock sceneLock(access.Mutex);
				effects.ApplyRuntimeAvailabilityLocked({_audio->GetStreamParams().NumInputChannels, surviving, serialIds, false, true, false});
			}
			complete(error);
			return;
		}
		_midiActive.store(true, std::memory_order_release);
		const auto names = result.ConnectedRegistryIds;
		const auto serialIds = _input->ConnectedSerialDeviceIds();
		ConfirmMidiConnectionChange(previous->Connection, result, names, access);
		_midiState.store(std::make_shared<const MidiState>(MidiState{std::move(result), names, serialIds}), std::memory_order_release);
		bool merged = false;
		{
			std::scoped_lock sceneLock(access.Mutex);
			merged = _rig->MergeHardwarePreferences(std::nullopt, desired->Desired);
		}
		const auto inputs = _audio->GetStreamParams().NumInputChannels;
		{
		std::scoped_lock sceneLock(access.Mutex);
		effects.ApplyRuntimeAvailabilityLocked({inputs, names, serialIds});
		}
		complete(merged ? "MIDI desired selection accepted" : "MIDI applied, rig update unavailable", true, true);
	}
	catch (const std::exception& error)
	{
		complete(std::string("MIDI change failed: ") + error.what());
	}
}

void SessionControl::ReportFileStatus(const std::string& status)
{
	_SetFileStatus(status);
	std::cout << "[SESSION scene=" << _sceneIdentity << " operation=" << Identity() << "] " << status << std::endl;
}

void SessionControl::PumpOnJob(StationAccess access, const Effects& effects, bool quitting)
{
	_ReconcileMidiDeviceRequestOnJob(access, effects, quitting);
	if (!AdmissionPending()) _RefreshMidiIfNeeded(access, effects, quitting);
}

void SessionControl::InitMidi(bool generatedRig)
{
	std::scoped_lock midiLock(_midiLifecycleMutex);
	auto result = _input->Init(_audio->GetMidiClockAnchor_Ref(), _audio->GetStreamParams().SampleRate,
		_midiInputInventory ? &*_midiInputInventory : nullptr, generatedRig);
	_midiInputInventory.reset();
	const auto ids = result.ConnectedRegistryIds;
	_midiState.store(std::make_shared<const MidiState>(MidiState{std::move(result), ids, _input->ConnectedSerialDeviceIds()}), std::memory_order_release);
	_midiActive.store(true, std::memory_order_release);
}

void SessionControl::CloseMidi()
{
	std::scoped_lock midiLock(_midiLifecycleMutex);
	_midiActive.store(false, std::memory_order_release);
	_input->CloseMidi();
}

void SessionControl::ReportFileOutcome(const std::string& status)
{
	_SetFileStatus(status);
	_Notice("JAM", status);
}

void SessionControl::SetRigSaveDialogOpenLocked(bool open)
{
	// Drain captured filename typing before every gate transition, including cancellation.
	actions::KeyAction discarded;
	while (_input->PumpGlobalKeyCapture(discarded)) {}
	_rigSaveDialogOpen = open;
}

SessionControl::Presentation SessionControl::TakePresentation()
{
	Presentation result;
	result.SceneId = _sceneIdentity; result.OperationId = Identity();
	result.SessionEnabled = _presentation.SessionEnabled; result.DefaultEnabled = _presentation.DefaultEnabled;
	result.DeviceStatus = _presentation.DeviceStatus; result.ActiveAudioStatus = _presentation.ActiveAudioStatus; result.FileStatus = _presentation.FileStatus;
	result.PagesChanged = std::exchange(_presentation.PagesChanged, false);
	if (result.PagesChanged) { result.Inventory = _deviceInventory; result.AcceptedBufferSizes = _acceptedBufferSizes; result.Midi = MidiSnapshot(); }
	result.AudioAccepted = std::exchange(_presentation.AudioAccepted, {});
	result.MidiAccepted = std::exchange(_presentation.MidiAccepted, {});
	result.Notices = std::exchange(_presentation.Notices, {});
	return result;
}
