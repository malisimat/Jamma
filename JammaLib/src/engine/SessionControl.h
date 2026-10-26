#pragma once

#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>
#include <utility>
#include "DeviceSelectionSchedule.h"
#include "RigCoordinator.h"
#include "../io/IoInputSubsystem.h"
#include "../io/IoSessionExporter.h"
#include "../midi/MidiRefreshPolicy.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include "../audio/AudioHost.h"

namespace io { class IoInputSubsystem; }

namespace engine
{
	// Non-audio session control: one admission/writer protocol spans schedule,
	// drain, hardware/file effects and restoration. Scene retains structural
	// eligibility, accepted publication/timing effects and top-level teardown.
	class SessionControl
	{
	public:
		enum Kind { None, Audio, Midi, Save, Load };
		struct AudioRestoration { bool Success; bool AdmissionRestored; };
		Kind Current() const { return _kind; }
		std::uint64_t Identity() const { return _identity; }
		// Job/startup/teardown callers retain their existing lock scopes.
		std::mutex& LifecycleMutex() { return _lifecycleMutex; }
		bool HoldsWriter() const { return _writer.owns_lock(); }
		void ReleaseWriter() { if (_writer.owns_lock()) _writer.unlock(); }
		bool AdmissionPending() const { return _admissionPending.load(std::memory_order_acquire); }
		bool InputClosed() const { return _inputClosed; }
		// Caller resumes MIDI dispatch and applies hierarchy/notice effects first.

		struct StationAccess
		{
			std::mutex& Mutex;
			const std::vector<std::shared_ptr<Station>>& Stations;
			std::vector<std::shared_ptr<Station>> Snapshot() const { std::scoped_lock lock(Mutex); return Stations; }
		};
		struct Eligibility { bool CaptureBusy, HasTakes, StructuralBusy; };
		struct RuntimeAvailability
		{
			unsigned int Inputs;
			std::vector<std::string> Midi, Serial;
			bool ApplyWhileQuiescent = false, OpenInput = true, Present = true;
			std::optional<unsigned int> PreviousSampleRate;
			std::optional<io::UserConfig::AudioSettings> ConfirmedGeneratedAudio;
		};
		using Restore = std::function<bool(std::string&)>;
		struct Effects
		{
			// Eligibility/export run outside Scene lock; runtime application holds it.
			// These synchronous effects never retain borrowed values or callbacks.
			std::function<Eligibility()> CheckEligibility;
			std::function<void(const RuntimeAvailability&)> ApplyRuntimeAvailabilityLocked;
			std::function<io::IoSessionExporter::ExportResult(const std::wstring&, const audio::AudioStreamParams&, const Restore&, const Restore&)> CaptureSession;
		};
		template<class T> struct Candidate { T Value; std::uint64_t Revision = 0u; };
		struct Notice { std::string Title, Message; };
		struct MidiState;
		struct Presentation
		{
			std::uint64_t SceneId = 0u, OperationId = 0u;
			bool SessionEnabled = false, DefaultEnabled = false, PagesChanged = true;
			std::string DeviceStatus, ActiveAudioStatus, FileStatus;
			audio::AsioInventory Inventory;
			std::unordered_map<std::string, std::vector<unsigned int>> AcceptedBufferSizes;
			std::optional<Candidate<io::UserConfig::AudioSettings>> AudioAccepted;
			std::optional<Candidate<std::vector<io::UserConfig::MidiSettings>>> MidiAccepted;
			std::vector<Notice> Notices;
			std::shared_ptr<const MidiState> Midi;
		};
		enum class RigSaveMode { Save, SaveAs, StartupDefault };
		void Bind(audio::AudioHost& audio, io::IoInputSubsystem& input, RigCoordinator& rig, io::UserConfig& config, std::uint64_t sceneIdentity);
		void InitializeCandidates(io::UserConfig::AudioSettings audio, std::vector<io::UserConfig::MidiSettings> midi)
		{ _submittedAudio.Value = std::move(audio); _submittedMidi.Value = std::move(midi); }
		void SubmitAudio(io::UserConfig::AudioSettings value, std::uint64_t revision, DeviceSelectionSchedule::Edit edit)
		{ _submittedAudio = {std::move(value), revision}; _audioSchedule.Changed(edit); _devicePageDirty |= edit == DeviceSelectionSchedule::Edit::Device; }
		void SubmitMidi(std::vector<io::UserConfig::MidiSettings> value, std::uint64_t revision)
		{ _submittedMidi = {std::move(value), revision}; _midiDraftPending = true; }
		void RetryAudio() { _audioStartRequested = true; _audioSchedule.Changed(DeviceSelectionSchedule::Edit::Retry); }
		void RequestMidiRefresh() noexcept { _midiRefreshRequested.store(true, std::memory_order_release); }
		void RefreshMidiFromWindow() noexcept { _devicePageDirty = true; RequestMidiRefresh(); }
		void RetryPreferences(const Effects& effects);
		void PumpOnWindow(StationAccess access, const Effects& effects, bool quitting);
		void PumpOnJob(StationAccess access, const Effects& effects, bool quitting);
		void RecoverSourcesLocked(StationAccess access, bool membershipBusy, bool quitting);
		void PumpMidi(StationAccess access);
		void PumpSerial(StationAccess access);
		void ConfirmMidiConnectionChange(const midi::MidiConnectionResult& previous, const midi::MidiConnectionResult& refreshed,
			const std::vector<std::string>& connectedIds, StationAccess access);
		// Caller holds LifecycleMutex through the conditional Scene commit/recovery tail.
		bool InitAudio(const std::shared_ptr<ninjam::NinjamController>& ninjam, const std::shared_ptr<utils::Timer>& clock,
			bool generatedRig, const audio::AsioInventory* inventory, bool exactSelection, StationAccess access, const Effects& effects);
		void InitMidi(bool generatedRig = false);
		void CloseMidi();
		void CloseAudio();
		bool PauseAudio();
		bool ResumeAudio();
		void SetMidiInputInventory(midi::MidiInputInventory inventory) { _midiInputInventory = std::move(inventory); }
		void SetHardwarePreferenceSaver(std::function<bool(const io::RigFile&)> saver) { _saveHardwarePreferences = std::move(saver); }
		bool SaveHardwarePreferences(const io::RigFile& rig) const { return _saveHardwarePreferences && _saveHardwarePreferences(rig); }
		void RequestSave(bool chooseFolder, bool asDefault, const Effects& effects);
		void PickSessionForLoad(const Effects& effects);
		void RequestSessionLoad(std::wstring path, const Effects& effects);
		std::wstring TakeReadySessionLoad();
		void FinishSessionLoad(bool replaced, const std::string& error, StationAccess access);
		bool FinishOperation(std::string* restorationError, StationAccess access);
		void CompleteSessionSave(bool success, const std::wstring& path, const std::string& error, std::wstring folder, bool asDefault);
		const std::wstring& SessionSaveFolder() const noexcept { return _sessionSaveFolder; }
		void ReportFileStatus(const std::string& status);
		void ReportFileOutcome(const std::string& status);
		void SetSessionSaveFolder(std::wstring folder) { _sessionSaveFolder = std::move(folder); }
		void SetDefaultJamSaver(std::wstring path, std::function<bool(const std::wstring&, bool)> saver) { _defaultJamPath = std::move(path); _defaultJamSaver = std::move(saver); }
		void SetCurrentJamPath(std::wstring path) { _currentJamPath = std::move(path); }
		const std::wstring& CurrentJamPath() const noexcept { return _currentJamPath; }
		bool ConsumePanelLoadRequest() { return std::exchange(_panelLoadRequested, false); }
		void RequestPanelLoad() { if (Current() == None) _panelLoadRequested = true; }
		bool IsReplacing() const noexcept { return _sessionReplacing.load(std::memory_order_acquire); }
		void BeginReplacement() noexcept { _sessionReplacing.store(true, std::memory_order_release); }
		void CloseSourceRecoveryGateLocked() noexcept { _sourceLossRecoveryGateClosed = true; }
		bool SourceRecoveryGateClosed() const noexcept { return _sourceLossRecoveryGateClosed; }
		bool RigSaveInProgress() const noexcept { return _rigSaveInProgress; }
		bool RigSaveDialogOpen() const noexcept { return _rigSaveDialogOpen; }
		void RequestRigSave(RigSaveMode mode) { _rigSaveMode = mode; _rigSaveRequested = true; }
		bool TakeRigSaveRequest() { return std::exchange(_rigSaveRequested, false); }
		RigSaveMode RequestedRigSaveMode() const noexcept { return _rigSaveMode; }
		void BeginExplicitRigSaveLocked() { _rigSaveInProgress = true; }
		void SetRigSaveDialogOpenLocked(bool open);
		void QueueExplicitRigSaveLocked(std::function<void()> work) { _rigSaveInProgress = true; _explicitRigSaveJob = std::move(work); }
		std::function<void()> TakeExplicitRigSaveLocked() { return std::exchange(_explicitRigSaveJob, {}); }
		void FinishExplicitRigSaveLocked() { _rigSaveInProgress = false; }
		Presentation TakePresentation();
		const audio::AsioInventory& DeviceInventory() const noexcept { return _deviceInventory; }
		struct MidiState
		{
			midi::MidiConnectionResult Connection;
			std::vector<std::string> ConnectedIds;
			// Collected under _midiLifecycleMutex with MIDI setup; immutable after
			// publication, so availability readers never touch mutable serial ports.
			std::vector<std::string> ConnectedSerialIds;
		};
		std::shared_ptr<const MidiState> MidiSnapshot() const { return _midiState.load(std::memory_order_acquire); }

	private:
		friend class SourceLossIntegrationTestAccess;
		void Begin(Kind kind) { _kind = kind; }
		void NextIdentity() { ++_identity; }
		void RequestBoundary(io::IoInputSubsystem& input, std::uint64_t revision);
		bool TryAcquireBoundary(io::IoInputSubsystem& input);
		bool PauseAudio(audio::AudioHost& audio, std::string& error);
		AudioRestoration RestoreAudio(audio::AudioHost& audio, std::string& error);
		void RestoreRunningAudio() { _pause.WasRunning = _pause.AdmissionWasOpen = true; }
		bool LoadReady() const { return _loadReady; }
		void MarkLoadReady() { _loadReady = true; }
		void Finish(io::IoInputSubsystem& input, std::optional<std::uint64_t> acceptedRevision);
		void FinishReplacement();

		std::optional<midi::MidiInputInventory> _midiInputInventory;
		std::atomic<std::shared_ptr<const MidiState>> _midiState = std::make_shared<const MidiState>();
		// UI publishes complete desired set; job exchanges/accepts it and publishes completion.
		struct MidiDeviceRequest
		{
			std::uint64_t SceneId = 0u, OperationId = 0u, DraftRevision = 0u;
			std::vector<io::UserConfig::MidiSettings> Desired;
		};
		struct MidiDeviceCompletion
		{
			std::uint64_t SceneId = 0u, OperationId = 0u, DraftRevision = 0u;
			std::string Status;
			bool Accepted = false, PreferencesSaved = false;
			std::vector<io::UserConfig::MidiSettings> Desired;
		};
		std::atomic<std::shared_ptr<const MidiDeviceRequest>> _pendingMidiDeviceRequest;
		std::atomic<std::shared_ptr<const MidiDeviceCompletion>> _completedMidiDeviceRequest;
		std::atomic<bool> _sessionReplacing{ false }; // UI writes; job reads; dies after join.
		std::function<bool(const io::RigFile&)> _saveHardwarePreferences; // Immutable callback, serialized lifecycle callers; Scene bounds lifetime.
		// Submitted candidates, schedule, presentation and file paths are window-owned.
		DeviceSelectionSchedule _audioSchedule;
		bool _audioStartRequested = false;
		std::unordered_map<std::string, std::vector<unsigned int>> _acceptedBufferSizes;
		audio::AsioInventory _deviceInventory;
		std::optional<io::UserConfig::AudioSettings> _pendingAudioPersistence;
		std::optional<std::vector<io::UserConfig::MidiSettings>> _pendingMidiPersistence;
		bool _midiDraftPending = false, _devicePageDirty = true, _hardwarePersistencePending = false;
		std::shared_ptr<const MidiState> _displayedMidiState;
		std::wstring _sessionSaveFolder, _pendingSaveFolder, _pendingLoadPath;
		std::wstring _defaultJamPath, _currentJamPath;
		std::function<bool(const std::wstring&, bool)> _defaultJamSaver;
		bool _pendingSaveAsDefault = false;
		bool _panelLoadRequested = false;
		std::mutex _midiLifecycleMutex;
		std::atomic<bool> _midiRefreshRequested{ false };
		std::atomic<bool> _midiActive{ false };
		midi::MidiRefreshPolicy _midiRefreshPolicy; // Job owner only; shutdown joins the job reader.
		std::uint64_t _sourceRecoveryHeartbeat = 0u; // Job owner only.
		std::chrono::steady_clock::time_point _sourceRecoveryHeartbeatAt{};
		bool _sourceLossRecoveryGateClosed = false; // Serialized by _sceneMutex.
		std::uint64_t _audioStreamEpoch = 0u; // Serialized lifecycle owner.

		bool _rigSaveRequested = false; // Window owner only.
		RigSaveMode _rigSaveMode = RigSaveMode::StartupDefault; // Window owner only.
		std::function<void()> _explicitRigSaveJob; // Scene mutex handoff to existing job owner; drained on shutdown.
		bool _rigSaveInProgress = false; // Scene mutex; UI completion resets.
		bool _rigSaveDialogOpen = false; // Protected by _sceneMutex.

		audio::AudioHost* _audio = nullptr;
		io::IoInputSubsystem* _input = nullptr;
		RigCoordinator* _rig = nullptr;
		io::UserConfig* _config = nullptr;
		std::uint64_t _sceneIdentity = 0u;
		Candidate<io::UserConfig::AudioSettings> _submittedAudio, _admittedAudio;
		Candidate<std::vector<io::UserConfig::MidiSettings>> _submittedMidi;
		Presentation _presentation;
		std::uint64_t _admittedMidiRevision = 0u;
		void _UpdateWindowPresentation(const audio::AudioStreamParams& actual, bool inventoryBusy);
		bool _ScheduleWindowOperation(DeviceSelectionSchedule::Clock::time_point now, const audio::AudioStreamParams& actual, const Effects& effects, bool inventoryBusy);
		void _CompleteMidiRequest();
		bool _AdmitBoundary(StationAccess access, DeviceSelectionSchedule::Clock::time_point now);
		void _ApplyAdmittedOperation(const audio::AudioStreamParams& actual, DeviceSelectionSchedule::Clock::time_point now, StationAccess access, const Effects& effects);
		void _ApplyAudioCandidate(const audio::AudioStreamParams& actual, DeviceSelectionSchedule::Clock::time_point now, StationAccess access, const Effects& effects, std::string& error);
		void _CaptureSession(const audio::AudioStreamParams& actual, StationAccess access, const Effects& effects);
		void _RefreshMidiIfNeeded(StationAccess access, const Effects& effects, bool quitting);
		void _ReconcileMidiDeviceRequestOnJob(StationAccess access, const Effects& effects, bool quitting);
		bool _ValidateAudioDraft(std::string& error, const Effects& effects) const;
		void _SetDeviceStatus(std::string text) { _presentation.DeviceStatus = std::move(text); }
		void _SetActiveAudioStatus(std::string text) { _presentation.ActiveAudioStatus = std::move(text); }
		void _SetFileStatus(std::string text) { _presentation.FileStatus = std::move(text); }
		void _SetSessionEnabled(bool enabled, bool defaults) { _presentation.SessionEnabled = enabled; _presentation.DefaultEnabled = defaults; }
		void _RefreshPresentation() { _presentation.PagesChanged = true; _displayedMidiState = MidiSnapshot(); _devicePageDirty = false; }
		void _Notice(std::string title, std::string message) { _presentation.Notices.push_back({std::move(title),std::move(message)}); }
		Kind _kind = None;
		std::uint64_t _identity = 0u, _revision = 0u;
		std::mutex _lifecycleMutex;
		std::unique_lock<std::mutex> _writer;
		std::atomic<bool> _admissionPending{false}; // UI publishes; job acquires.
		audio::AudioHost::QuiescenceState _pause{};
		bool _paused = false, _inputClosed = false, _loadReady = false;
	};
}
