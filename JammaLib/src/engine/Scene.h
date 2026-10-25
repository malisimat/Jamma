#pragma once

// Job/UI orchestrator: presents and forwards subsystem values without taking over
// audio-callback application, remote timing authority, or per-entity loop state.
#include <atomic>
#include <array>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include "PerformanceGroupController.h"
#include "ContextualKeyboardInput.h"
#include "../resources/ResourceLib.h"
#include "../actions/JobAction.h"
#include "../audio/AudioDevice.h"
#include "../audio/ChannelMixer.h"
#include "../graphics/Image.h"
#include "../graphics/Camera.h"
#include "../graphics/UnattendedCameraController.h"
#include "../graphics/GlDrawContext.h"
#include "../graphics/Skybox.h"
#include "../gui/GuiLabel.h"
#include "../gui/NinjamTempoPrompt.h"
#include "../gui/GuiFocusManager.h"
#include "../gui/SceneSettingsControls.h"
#include "../gui/GuiNumericInput.h"
#include "../gui/GuiDropdown.h"
#include "../gui/GuiToggle.h"
#include "../gui/GuiPopupManager.h"
#include "../gui/SceneSelector.h"
#include "../gui/GuiMainPanel.h"
#include "../gui/GuiHud.h"
#include "../io/JamFile.h"
#include "DeviceSelectionSchedule.h"
#include "../io/RigFile.h"
#include "../io/InitFile.h"
#include "../io/SerialDevice.h"
#include "../ninjam/NinjamController.h"
#include "../audio/AudioHost.h"
#include "../io/IoInputSubsystem.h"
#include "../ninjam/NinjamNetworkService.h"
#include "../ninjam/NinjamTimingDiagnosticPresenter.h"
#include "../vst/VstEditorWindowManager.h"
#include "../midi/MidiDevice.h"
#include "../midi/MidiRefreshPolicy.h"
#include "../midi/MidiRouter.h"
#include "../graphics/VstEditorWindow.h"
#include "../graphics/CtrlHandleOverlay.h"
#include "../engine/Quantiser.h"
#include "Tickable.h"
#include "Drawable.h"
#include "ActionReceiver.h"
#include "AudioSource.h"
#include "Moveable.h"
#include "Sizeable.h"
#include "GuiElement.h"
#include "Station.h"
#include "HierarchySelection.h"
#include "StationRemote.h"
#include "RigCoordinator.h"
#include "StationManagement.h"
#include "../gui/GuiStationControls.h"
#include "../gui/VstRackControls.h"
#include "../actions/ActionUndoHistory.h"
#include "../midi/LoopGridEditor.h"

namespace engine
{
	class SceneParams :
		public base::DrawableParams,
		public base::MoveableParams,
		public base::SizeableParams
	{
	public:
		std::function<void()> OnJobThreadStart;
		bool StartupDiagnostics = false;
		SceneParams(base::DrawableParams drawParams,
			base::MoveableParams moveParams,
			base::SizeableParams sizeParams) :
			base::DrawableParams(drawParams),
			base::MoveableParams(moveParams),
			base::SizeableParams(sizeParams)
		{}
	};

	class Scene :
		public base::Tickable,
		public base::Drawable,
		public base::Moveable,
		public base::Sizeable,
		public base::ActionReceiver
	{
	public:
		enum ViewMode
		{
			VIEW_STATION = 0,
			VIEW_LOOPTAKE = 1,
			VIEW_LOOP = 2
		};

	public:
		Scene(SceneParams params,
			io::UserConfig user);
		~Scene()
		{
			Shutdown();
			ReleaseResources();
		}

		// Copy
		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;
		static std::optional<std::shared_ptr<Scene>> FromFile(SceneParams sceneParams,
			io::JamFile jam,
			io::RigFile rig,
			std::wstring dir,
			std::function<bool(const io::RigFile&)> saveRig = {});
		
		virtual void Draw(base::DrawContext& ctx) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		void DrawBackground(base::DrawContext& ctx);
		bool HasSelection() const;
		void UpdateCamera();
		bool AdvanceUiAnimations();
		void OnHumanActivity();
		void OnCameraTriggerActivity();
		bool PreservesCameraIdleForKey(unsigned int key) const noexcept;
		bool IsCameraTriggerKey(unsigned int key) const;
		void SetCameraInteractionEligibility(bool windowEligible, bool heldInteraction);

		virtual void SetSize(utils::Size2d size) override
		{
			std::scoped_lock lock(_sceneMutex);
			_sizeParams.Size = size;
			_InitSize();
			_InvalidateHover2d();
		}

		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		virtual actions::ActionResult OnAction(actions::GuiAction action) override;
		virtual void OnTick(Time curTime,
			unsigned int samps,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<audio::AudioStreamParams>& params) override;
		virtual void OnJobTick(Time curTime);
		// UI arms after first render; job owner completes after queued startup work.
		void CompleteStartupWhenIdle(std::function<void()> complete);
		virtual void InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		void InitReceivers();
		void AddChild(std::shared_ptr<base::GuiElement> child);
		void SetHover3d(std::vector<unsigned char> path, base::Action::Modifiers modifiers,
			std::optional<utils::Position2d> pointerPosition = std::nullopt);
		unsigned int Width() const { return _sizeParams.Size.Width; }
		unsigned int Height() const { return _sizeParams.Size.Height; }
		void Reset();
		void InitGui();
		// Main/UI owner only. One operation spans admission through restoration.
		void PumpDeviceAndFileControls();
		void RequestSessionLoad(std::wstring path);
		void PickSessionForLoad();
		bool ConsumePanelLoadRequest();
		std::wstring TakeReadySessionLoad();
		void FinishSessionLoad(bool replaced, const std::string& error = {});
		void SetSessionSaveFolder(std::wstring folder) { _sessionSaveFolder = std::move(folder); }
		void SetDefaultJamSaver(std::wstring path, std::function<bool(const std::wstring&, bool)> saver)
		{ _defaultJamPath = std::move(path); _defaultJamSaver = std::move(saver); }
		void SetCurrentJamPath(std::wstring path) { _currentJamPath = std::move(path); }
		const std::wstring& CurrentJamPath() const noexcept { return _currentJamPath; }
		std::uint64_t Identity() const noexcept { return _sceneIdentity; }
		void ReportFileStatus(const std::string& status);
		void ReportFileOutcome(const std::string& status)
		{ _settingsControls->SetFileStatus(status); ReportRuntimeNotice("JAM",status); }
		// Owned values only; off-callback workers may post, UI alone presents.
		void ReportRuntimeNotice(std::string title, std::string message)
		{ _runtimeDialogs.Notify(std::move(title),std::move(message)); }
		void SetRuntimeDialogFallback(gui::RuntimeDialogs::Fallback fallback)
		{ _runtimeDialogs.SetFallback(std::move(fallback)); }
		void InitAudio(bool generatedRig = false,
			const audio::AsioInventory* inventory = nullptr, bool exactSelection = false);
		const audio::AsioOpenReport& GetAsioOpenReport() const noexcept
		{
			return _audioEngine->GetAsioOpenReport();
		}
		audio::AudioStreamParams GetAudioStreamParams() const
		{
			return _audioEngine->GetStreamParams();
		}
		void CloseAudio();
		bool PauseAudio();
		bool ResumeAudio();
		bool InitGlobalKeyCapture();
		void CloseGlobalKeyCapture();
		bool PumpGlobalKeyCapture(actions::KeyAction& action) noexcept;
		void OnInputFocusLost();
		bool CapturesContextualKeyboard(unsigned int key) const
		{ return _rigSaveDialogOpen || _controlWriterExclusion.owns_lock() || _popupManager.IsOpen() || _focusManager.IsEditingText() || _loopEditor.IsEngaged() || _contextualKeyboard.Suppresses(key); }
		void Shutdown();
		bool JobThreadFailed() const noexcept { return _jobThreadFailed.load(std::memory_order_acquire); }
		std::string_view JobThreadFailureDetail() const noexcept { return JobThreadFailed() ? std::string_view(_jobThreadFailureDetail.data()) : std::string_view{}; }
		void SetLogging(io::LoggingConfig config) noexcept;
		bool IsUiVerbose() const noexcept { return _loggingConfig.Ui == "verbose"; }
		void InitMidi(bool generatedRig = false)
		{
			std::scoped_lock midiLock(_midiLifecycleMutex);
			auto result = _inputSubsystem->Init(_audioEngine->GetMidiClockAnchor_Ref(),
				_audioEngine->GetStreamParams().SampleRate,
				_midiInputInventory ? &*_midiInputInventory : nullptr, generatedRig);
			_midiInputInventory.reset();
			const auto connectedIds = result.ConnectedRegistryIds;
			_midiState.store(
				std::make_shared<const MidiState>(MidiState{ std::move(result), connectedIds, _inputSubsystem->ConnectedSerialDeviceIds() }),
				std::memory_order_release);
			_midiActive.store(true, std::memory_order_release);
		}
		void SetMidiInputInventory(midi::MidiInputInventory inventory)
		{
			_midiInputInventory = std::move(inventory);
		}
		midi::MidiConnectionResult GetMidiConnectionResult() const
		{
			return _midiState.load(std::memory_order_acquire)->Connection;
		}
		void CloseMidi()
		{
			std::scoped_lock midiLock(_midiLifecycleMutex);
			_midiActive.store(false, std::memory_order_release);
			_inputSubsystem->CloseMidi();
		}
		void RequestMidiRefresh() noexcept { _midiRefreshRequested.store(true, std::memory_order_release); }
		void InitSerial() {}
		void CloseSerial() {}
		// UI thread only: reconciles pending topology and popup presentation before rendering.
		void CommitChanges();
		// Off-callback hardware-only saver; never persists live membership or routing.
		bool SaveHardwarePreferences(const io::RigFile& rig) const
		{ return _saveHardwarePreferences && _saveHardwarePreferences(rig); }
		bool ConsumePanelRigLoadRequest() { return std::exchange(_panelRigLoadRequested, false); }
		bool ConsumeRigSaveRequest(); // Main/window thread only.
		enum class RigSaveMode { Save, SaveAs, StartupDefault };
		RigSaveMode RequestedRigSaveMode() const { return _rigSaveMode; }
		struct CapturedRig { io::RigFile Rig; std::uint64_t Revision = 0u, SceneId = 0u; };
		std::optional<CapturedRig> CaptureCommittedRigVersion() const;
		std::optional<CapturedRig> BeginExplicitRigSave();
		std::optional<io::RigFile> CaptureCommittedRig() const;
		void QueueExplicitRigSave(std::function<void()> work);
		void FinishExplicitRigSave();
		void SetRigSaveDialogOpen(bool open); // Window owner; gates conflicting input.

		RigCoordinator::EditResult RequestRigEdit(const io::RigFile& candidateRig);
		std::shared_ptr<const RigSnapshot> AcceptedRigSnapshot() const noexcept
		{
			return _rigCoordinator.Accepted();
		}
		void ApplyDeferredHoverUpdates();
		void SetRelativePointerHost(std::function<bool(int, utils::Position2d)> begin,
			std::function<void(int)> end, std::function<float()> logicalPixelScale = {});
		// UI-thread editor seam, forwarded to midi::LoopGridEditor.
		bool OpenLoopGridEditor(const std::shared_ptr<LoopTake>& take,
			const std::shared_ptr<Loop>& audioLoop,
			const std::shared_ptr<midi::MidiLoop>& midiLoop)
		{
			return _loopEditor.Open(take, audioLoop, midiLoop);
		}
		void CloseLoopGridEditor() { _loopEditor.Close(); }
		bool IsLoopGridEditorOpen() const noexcept { return _loopEditor.IsOpen(); }
		bool LoopGridEditorReady() const noexcept { return _loopEditor.IsReady(); }
		float LoopGridEditorMorph() const noexcept { return _loopEditor.Morph(); }
		float LoopGridEditorSurroundingDim() const noexcept { return _loopEditor.SurroundingDim(); }
		std::shared_ptr<Loop> LoopGridEditorAudioLoop() const noexcept { return _loopEditor.AudioLoop(); }
		std::shared_ptr<midi::MidiLoop> LoopGridEditorMidiLoop() const noexcept { return _loopEditor.TargetMidiLoop(); }
		std::shared_ptr<LoopTake> LoopGridEditorTake() const noexcept { return _loopEditor.Take(); }

		// Returns a locked station snapshot safe to use outside render/tick threads.
		std::vector<std::shared_ptr<Station>> SnapshotStations() const;

		// Send a chat message on the active ninjam session (no-op if none).
		bool SendNinjamChat(const std::string& msg)
		{
			return _networkService->SendChat(msg);
		}
		// App-owner presentation query; NinjamSession pins the physical connection.
		bool NinjamConnected() const noexcept
		{
			return _networkService->GetController()->Session()->IsConnected();
		}

		// Close all open VST editor windows immediately.
		// Call this on the main thread before OleUninitialize() during shutdown.
		void CloseAllVstEditorWindows()
		{
			_windowSubsystem->CloseAllVstEditorWindows();
		}

		// Connect to an arbitrary NINJAM host ("host:port"). Reuses credentials
		// from the loaded jam config when available; falls back to anonymous.
		void ConnectNinjam(const std::string& host);
		void ConnectNinjam(const std::string& host,
			const ninjam::NinjamTempoJoinOptions& options);

		// Disconnect the active NINJAM session. No-op if not connected.
		void DisconnectNinjam();

		// Force-unload all hosted VST plugins owned by stations/takes/loops.
		// Call on the main thread only after CloseAudio() has stopped the callback.
		void ForceUnloadAllVstPlugins();
		
	protected:
		friend class SourceLossIntegrationTestAccess;
		friend class SceneStartupDiagnosticTestAccess;
		friend class audio::AudioHostRealtimeTestAccess;
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

		static std::shared_ptr<StationRemote> FindRemoteStation(const std::vector<std::shared_ptr<Station>>& stations,
			const std::string& userName);
		static std::vector<unsigned char> TrimPath(std::vector<unsigned char> path,
			unsigned int depth);
		static int AudioCallback(void* outBuffer,
			void* inBuffer,
			unsigned int numSamps,
			double streamTime,
			RtAudioStreamStatus status,
			void* userData);

		void _OnAudio(float* inBuffer,
			float* outBuffer,
			unsigned int numSamps);
		bool _OnUndo(std::shared_ptr<base::ActionUndo> undo);
		void _InitSize();
		void _UpdateHudStationAnchors();
		void _UpdateViewProjectionLocked();
		void _PublishGroupMuteApplication(std::shared_ptr<const GroupMuteApplication> application);
		void _ExecuteGroupCommandLocked(bool selection, GroupCommand command, std::size_t slot);
		GroupTargetKind _GroupSelectionDepthLocked() const;
		void _RestoreGroupSelectionDepthLocked();
		void _UpdateSelection(actions::ActionResultType res);
		actions::ActionResult _OnPointerAction(actions::TouchAction action);
		actions::ActionResult _OnPointerMove(actions::TouchMoveAction action);
		void _UpdateRackVisibilityLocked();
		void _CollapseRacksLocked();
		// Selection helpers require _sceneMutex throughout hierarchy access.
		base::Tweakable::TweakState _SelectionTweakStateLocked(const std::shared_ptr<base::GuiElement>& target) const;
		void _PrepareStation(const std::shared_ptr<Station>& station);
		void _AddStation(std::shared_ptr<Station> station, bool publishAudioStations = true);
		void _RequestStationControl(gui::GuiStationControls::Intent intent);
		void _RequestTakeDitch(gui::GuiStationControls::Intent intent);
		void _CancelTakeDitch();
		void _ConfirmTakeDitch();
		void _AcceptStationRemoval(bool consent);
		void _CommitStationMembershipLocked();
		RigCoordinator::EditResult _RequestRigEditLocked(const io::RigFile& candidateRig);
		std::string _RequestStationRenameLocked(const std::shared_ptr<Station>& station,
			std::uint64_t revision, const std::string& name);
		bool _JobReceiverAvailableLocked(const std::shared_ptr<base::ActionReceiver>& receiver) const;
		bool _StationManagementAvailableLocked() const;

		// Pass a locked station list or a snapshot; remote updates can erase entries.
		static utils::Position3d _StationCentre(const std::vector<std::shared_ptr<Station>>& stations);
		void _CycleCameraView();
		void _ApplyCameraSelectDepthChange(graphics::Camera::SelectDepthChange change);
		void _HandleReclockArm();
		actions::ActionResult _HandleUndo();
		void _SetQuantisation(unsigned int quantiseSamps, utils::Timer::QuantisationType quantisation);
		void _SetMidiQuantisationGrain(unsigned int grainSamps, const char* source);
		void _SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState state, bool fromLocalEdit = false);
		void _SetTransportOffsetLoopFrac(double loopFrac, bool updateInput = true);
		void _ApplyGlobalMidiQuantStateToAllLoopTakes();
		void _ForceGlobalMidiQuantStateMixedOnLocalEdit();
		void _JobLoop(std::function<void()> onStart) noexcept;
		void _FailJobThread(const char* stage, const char* detail, const actions::JobAction* job = nullptr) noexcept;
		void _QueueJobs(std::vector<actions::JobAction> jobs);
		void _RebuildDevicePage();
		void _SetAudioDraftFromStream(const audio::AudioStreamParams& params);
		void _RequestSave(bool chooseFolder, bool asDefault = false);
		void _CompleteSessionSave(bool success, const std::wstring& path, const std::string& error);
		bool _CaptureIsBusy() const;
		bool _SessionHasTakes() const;
		bool _ValidateAudioDraft(std::string& error) const;
		bool _FinishControlOperation(std::string* restorationError = nullptr);
		void _ReconcileMidiDeviceRequestOnJob();
		void _PumpMidi();
		void _RefreshMidiIfNeeded();
		void _ConfirmMidiConnectionChange(const midi::MidiConnectionResult& previous,
			const midi::MidiConnectionResult& refreshed, const std::vector<std::string>& connectedIds);
		void _PumpSourceLossRecovery();
		void _PumpTriggerStructuralActions();
		void _AdvanceRigPublication();
		gui::RoutingEditAvailability _RoutingEditAvailability();
		void _PumpSerial();
		void _ConsumeTriggerOutcomes();
		void _PublishAudioStations();
		std::shared_ptr<base::GuiElement> _ChildFromPath(std::vector<unsigned char> path);
		std::shared_ptr<base::GuiElement> _ChildFromPathLocked(const std::vector<unsigned char>& path) const;
		void _UpdateSelectDepth(unsigned int depth);
		void _UpdateRemoteStationsFromSnapshot(const ninjam::NinjamRemoteSnapshot& snapshot);
		engine::QuantisationPolicy _QuantisationPolicy() const;
		unsigned int _CurrentSampleRate() const;
		std::uint64_t _EstimatedAudioSampleAt(Time actionTime) const;
		void _ApplyQuantisationTiming(const engine::QuantisationTiming& timing, const char* source);
		void _ClearTimingState(bool clearTapTempo);
		void _HandleAudioLocalContentState(bool hasLocalContent);
		void _UpdateRecordedPlaybackCompensation();
		std::optional<unsigned int> _lastRejectedRecordedLatency; // Job owner, under _sceneMutex.
		void _ResetIfEmpty();
		bool _HandleTapTempo(Time actionTime);
		void _PulseQuantisationOverlay();
		void _SetQuantisationOverlayHeld(bool held);
		float _QuantisationOverlayAlpha(Time now) const;
		void _ApplyQuantisationOverlayAlpha(float alpha);
		engine::QuantisationInteractionContext _InteractionContext() const;
		engine::QuantisationInteractionContext _InteractionContextLocked() const;
		void _InvalidateHover2d();
		void _ResolveHoverPath2d(std::vector<std::weak_ptr<base::GuiElement>>& outPath);
		void _ApplyHoverPath2d(const std::vector<std::weak_ptr<base::GuiElement>>& nextPath);
		void _LockHoverPath(const std::vector<std::weak_ptr<base::GuiElement>>& path,
			std::vector<std::shared_ptr<base::GuiElement>>& outPath) const;
		actions::ActionResult _BeginBackgroundDrag(actions::TouchAction action);
		actions::ActionResult _UpdateBackgroundDrag(actions::TouchMoveAction action);
		void _EndBackgroundDrag();
		bool _TrySetMasterFromHover(bool confirm);
		void _UpdateStationQuantisation(std::shared_ptr<base::GuiElement> candidate, base::SelectDepth depth, bool confirmCandidate);
		void _ClearStationQuantisation();
		bool _HasSelection() const;
		bool _HasQuantisationHover() const;
		bool _IsMidiPhaseDragModifier(base::Action::Modifiers modifiers) const noexcept;
		void _HandleRemoteTempoSnapshot(const ninjam::NinjamRemoteSnapshot& snapshot,
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent);
		void _ApplyNinjamTimingUpdate(const ninjam::NinjamTimingUpdate& update);
		void _LogNinjamTempoJoinState();
		void _ApplyRemoteTempoPromptDecision();
		void _OnLoopGridEditorOpened();
		void _ReleaseQuantisationInput();
		void _ReleaseQuantisationInputLocked();
		bool _EditControlsSuppressed() const;
		std::optional<actions::ActionResult> _RouteQuantisationTouch(actions::TouchAction action);
		std::optional<actions::ActionResult> _RouteQuantisationMove(actions::TouchMoveAction action);


	protected:
		static constexpr std::uint8_t  UnresolvedMidiDeviceSlot       = 0xffu;

		bool _isSceneTouching;
		// Window-thread pointer ownership; command and button are latched at a
		// background press. Any movement cancels the command, retaining ownership
		// until release so it cannot turn into a camera drag.
		enum class BackgroundCommand { UnmuteAll, MuteAll, SelectAll };
		struct BackgroundGesture { int Button; BackgroundCommand Command; utils::Position2d PressPosition; bool Cancelled = false; bool Moved = false; };
		std::optional<BackgroundGesture> _backgroundGesture;
		bool _BackgroundPreviewActive() const;
		bool _BackgroundSelectionPreviewActive() const;
		void _SelectAllLocalLocked();
		std::atomic_bool _isSceneQuitting;
		std::atomic_bool _jobThreadFailed{false}; // Publishes the immutable failure detail to UI; lifetime ends after join.
		std::array<char, 4096> _jobThreadFailureDetail{}; // Job writes once, before release-publishing failure.
		std::atomic_bool _isSceneReset;
		glm::mat4 _viewProj;
		glm::mat4 _overlayViewProj;
		glm::mat4 _skyboxViewProj;
		bool _skyboxStarted;
		Time _skyboxStartTime;
		graphics::Skybox _skybox;
		std::unique_ptr<audio::AudioHost> _audioEngine;
		std::unique_ptr<io::IoInputSubsystem> _inputSubsystem;
		std::optional<midi::MidiInputInventory> _midiInputInventory;
		struct MidiState
		{
			midi::MidiConnectionResult Connection;
			std::vector<std::string> ConnectedIds;
			// Collected under _midiLifecycleMutex with MIDI setup; immutable after
			// publication, so availability readers never touch mutable serial ports.
			std::vector<std::string> ConnectedSerialIds;
		};
		std::atomic<std::shared_ptr<const MidiState>> _midiState = std::make_shared<const MidiState>();
		// UI lifecycle owner uses try_lock after trigger gate acknowledgement.
		// Job holds this for its complete tick; callback never accesses it.
		std::mutex _controlLifecycleMutex;
		// UI publishes complete desired set; job exchanges/accepts it and publishes completion.
		struct MidiDeviceRequest
		{
			std::uint64_t SceneId = 0u, OperationId = 0u;
			std::vector<io::UserConfig::MidiSettings> Desired;
		};
		struct MidiDeviceCompletion
		{
			std::uint64_t SceneId = 0u, OperationId = 0u;
			std::string Status;
			bool Accepted = false, PreferencesSaved = false;
			std::vector<io::UserConfig::MidiSettings> Desired;
		};
		std::atomic<std::shared_ptr<const MidiDeviceRequest>> _pendingMidiDeviceRequest;
		std::atomic<std::shared_ptr<const MidiDeviceCompletion>> _completedMidiDeviceRequest;
		std::atomic<bool> _controlAdmissionPending{false};
		std::unique_lock<std::mutex> _controlWriterExclusion;
		std::atomic<bool> _sessionReplacing{ false }; // UI writes; job reads; dies after join.
		enum class ControlOperation { None, Audio, Midi, Save, Load };
		std::function<bool(const io::RigFile&)> _saveHardwarePreferences; // Immutable callback, serialized lifecycle callers; Scene bounds lifetime.
		ControlOperation _controlOperation = ControlOperation::None;
		audio::AudioHost::QuiescenceState _controlPause{};
		bool _controlPaused = false;
		bool _controlInputClosed = false;
		std::uint64_t _controlRevision = 0u;
		std::uint64_t _controlOperationId = 0u;
		inline static std::atomic<std::uint64_t> _nextSceneIdentity{1u};
		const std::uint64_t _sceneIdentity = _nextSceneIdentity.fetch_add(1u);
		// All remaining drafts, deadlines, widgets/results and paths are UI-only.
		io::UserConfig::AudioSettings _audioDraft{};
		DeviceSelectionSchedule _audioSchedule;
		bool _audioStartRequested = false;
		std::unordered_map<std::string, std::vector<unsigned int>> _acceptedBufferSizes;
		audio::AsioInventory _deviceInventory;
		std::vector<io::UserConfig::MidiSettings> _midiDraft;
		std::optional<io::UserConfig::AudioSettings> _pendingAudioPersistence;
		std::optional<std::vector<io::UserConfig::MidiSettings>> _pendingMidiPersistence;
		bool _midiDraftPending = false, _devicePageDirty = true, _hardwarePersistencePending = false;
		std::shared_ptr<const MidiState> _displayedMidiState;
		std::wstring _sessionSaveFolder, _pendingSaveFolder, _pendingLoadPath;
		std::wstring _defaultJamPath, _currentJamPath;
		std::function<bool(const std::wstring&, bool)> _defaultJamSaver;
		bool _pendingSaveAsDefault = false;
		bool _panelRigLoadRequested = false;
		bool _panelLoadRequested = false, _loadReady = false;
		std::mutex _midiLifecycleMutex;
		std::atomic<bool> _midiRefreshRequested{ false };
		std::atomic<bool> _midiActive{ false };
		midi::MidiRefreshPolicy _midiRefreshPolicy; // Job owner only; shutdown joins the job reader.
		std::uint64_t _sourceRecoveryHeartbeat = 0u; // Job owner only.
		std::chrono::steady_clock::time_point _sourceRecoveryHeartbeatAt{};
		bool _sourceLossRecoveryGateClosed = false; // Serialized by _sceneMutex.
		std::uint64_t _audioStreamEpoch = 0u; // Serialized lifecycle owner.
		std::unique_ptr<vst::VstEditorWindowManager> _windowSubsystem;
		std::unique_ptr<gui::VstRackControls> _vstRackControls; // Window-thread owner; reset before station teardown.
		std::unique_ptr<ninjam::NinjamNetworkService> _networkService;
		engine::Quantiser _quantisation;
		std::uint64_t _lastLocalMasterTimingVersion = 0u;
		std::uint64_t _preparingLocalMasterTimingVersion = 0u;
		std::uint64_t _preparingLocalMasterTimingDelta = 0u;
		io::LoggingConfig _loggingConfig;
		std::unique_ptr<gui::SceneSettingsControls> _settingsControls;
		// Window-thread physical key state; repeats and consumed text taps cannot
		// become extra tempo taps. Gesture holds are owned by the controller.
		bool _spaceHeld = false;
		bool _ctrlHeld = false;
		std::string _quantisationFeedback;
		std::atomic_bool _quantisationInputResetRequested{ false };
		io::JamFile::GlobalMidiQuantState _globalMidiQuantState = io::JamFile::GlobalMidiQuantState::Mixed;
		double _transportOffsetLoopFrac = 0.0;
		std::unique_ptr<gui::GuiLabel> _tempoLabel;
		std::unique_ptr<gui::GuiLabel> _versionLabel;
		std::unique_ptr<gui::SceneSelector> _selector;
		std::shared_ptr<gui::GuiMainPanel> _mainPanel;
		std::shared_ptr<gui::GuiMainPanel> _selectionPanel;
		std::shared_ptr<gui::GuiHud> _hudPanel;
		PerformanceGroupController _performanceGroups; // scene mutex, never audio
		ContextualKeyboardInput _contextualKeyboard; // Window thread only; no callback state.
		std::array<bool, 256u> _learnKeysDown{}; // Window owner: repeat filtering, cleared on focus loss.
		std::vector<std::shared_ptr<base::GuiElement>> _guiChildren;
		gui::GuiFocusManager _focusManager;
		gui::GuiPopupManager _popupManager;
		gui::RuntimeDialogs _runtimeDialogs;
		bool _runtimeRendererReady = false; // UI-only; job and audio never inspect it.
		gui::NinjamTempoPrompt _tempoPrompt;
		ninjam::NinjamTimingDiagnosticPresenter _ninjamDiagnosticPresenter;
		// Job-owned diagnostic maxima; no callback reads/writes.
		std::optional<Time> _ninjamTraceLastJobAt;
		long long _ninjamTraceMaxJobGapMs = 0;
		long long _ninjamTraceMaxPumpUs = 0;
		std::size_t _ninjamTraceJobQueueDepth = 0u;
		std::uint64_t _ninjamJoinGeneration = 0u;
		std::uint64_t _ninjamTempoRequestId = 0u;
		ninjam::TempoRequestState _lastLoggedTempoRequestState = ninjam::TempoRequestState::Idle;
		std::vector<std::shared_ptr<Station>> _stations;
		RigCoordinator _rigCoordinator;
		std::vector<std::shared_ptr<Station>> _retiredStations; // Scene mutex; UI reclaims after all callback/job pins release.
		std::optional<ninjam::NinjamRemoteSnapshot> _deferredRemoteSnapshot; // Scene mutex; latest remote membership while a local change commits.
		StationManagement _stationManagement; // All access under _sceneMutex; UI teardown.
		gui::GuiStationControls _stationControls; // Window/render owner only.
		std::shared_ptr<gui::GuiPopup> _stationDeleteDialog; // Window owner only.
		std::shared_ptr<gui::GuiPopup> _takeDitchDialog; // Window owner only.
		// Window-owned confirmation pins; request/cancel/confirm serialize with job edits via _sceneMutex.
		// Released on cancel, successful/stale confirmation, or Scene teardown.
		std::shared_ptr<Station> _takeDitchStation;
		std::shared_ptr<LoopTake> _takeDitchTake;
		bool _rigSaveRequested = false; // Window owner only.
		RigSaveMode _rigSaveMode = RigSaveMode::StartupDefault; // Window owner only.
		std::function<void()> _explicitRigSaveJob; // Scene mutex handoff to existing job owner; drained on shutdown.
		bool _rigSaveInProgress = false; // Scene mutex; UI completion resets.
		bool _rigSaveDialogOpen = false; // Protected by _sceneMutex.

		std::uint64_t _rigTransitionRequestedRevision = 0u;
		std::unordered_map<const Trigger*, std::pair<std::uint64_t, std::uint64_t>> _triggerOutcomeCounts;
		std::uint64_t _lastAudioCallbackHeartbeat = 0u;
		std::chrono::steady_clock::time_point _lastAudioCallbackHeartbeatAt{};
		actions::ActionUndoHistory _undoHistory;
		std::weak_ptr<base::GuiElement> _touchDownElement;
		// Whether the touch sequence started on the HUD panel, recorded at touch-down.
		bool _touchDownIsHud = false;
		bool _touchDownIsSettings = false;
		bool _consumeSettingsRelease = false;
		void _OnSettingsHidden(const std::shared_ptr<base::GuiElement>& subtree);
		std::optional<actions::ActionResult> _RouteSettingsTouch(actions::TouchAction action);
		std::weak_ptr<base::GuiElement> _hoverElement3d;
		std::vector<unsigned char> _hoverPath3d;
		std::vector<std::weak_ptr<base::GuiElement>> _hoverPath2d;
		std::vector<std::weak_ptr<base::GuiElement>> _hoverPath2dScratch;
		std::vector<std::shared_ptr<base::GuiElement>> _hoverPath2dPrevSharedScratch;
		std::vector<std::shared_ptr<base::GuiElement>> _hoverPath2dNextSharedScratch;
		bool _hover2dDirty;
		std::vector<unsigned char> _lastLoggedHoverPath;
		graphics::CtrlHandleOverlay _ctrlHandleOverlay;
		engine::QuantiserController _quantisationInteraction;
		// Window-only controller/eligibility; publisher has separate structural ownership.
		graphics::UnattendedCameraController _unattendedCamera;
		bool _cameraWindowEligible = true;
		bool _cameraHeldInteraction = false;
		std::shared_ptr<CameraRecordingPublisher> _cameraRecordings = std::make_shared<CameraRecordingPublisher>();
		graphics::Camera _camera;
		std::optional<Time> _lastCameraUpdateTime;
		std::optional<Time> _lastCameraDiagnosticTime;
		std::uint64_t _cameraHumanActivityRevision = 0u;
		// Window owner only: selection/group clicks retain their gesture classification through release.
		bool _cameraPointerPreservesIdle = false;
		std::optional<Time> _lastPanelAnimationTime;
		std::thread _jobRunner;
		std::mutex _jobMutex;
		std::list<actions::JobAction> _jobList;
		std::function<void()> _startupComplete; // Protected by _jobMutex; callback never reads.
		bool _startupJobDiagnostics = false; // Initial queue admission, protected by _jobMutex.
		mutable std::mutex _sceneMutex;
		io::UserConfig _userConfig;
		ViewMode _viewMode;
		utils::Position2d _cursorPos{};
		std::function<bool(int, utils::Position2d)> _beginRelativePointer;
		std::function<void(int)> _endRelativePointer;
		std::function<float()> _logicalPixelScale;
		// Declared last: it holds references to the members above.
		midi::LoopGridEditor _loopEditor;
	};
}
