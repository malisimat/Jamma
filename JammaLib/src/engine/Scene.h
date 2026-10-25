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
#include "../gui/Workspace.h"
#include "../gui/SceneSettingsControls.h"
#include "../io/JamFile.h"
#include "DeviceSelectionSchedule.h"
#include "SessionControl.h"
#include "LocalTiming.h"
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
		// Optional monotonic clock for the UI routing-liveness gate.
		std::function<std::chrono::steady_clock::time_point()> RoutingEditNow;
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
			_workspace.reset();
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
			_workspace->ResizeLocked(_sizeParams.Size, _workspace->Editor(), _stations);
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
		void SetSessionSaveFolder(std::wstring folder) { _operation.SetSessionSaveFolder(std::move(folder)); }
		void SetDefaultJamSaver(std::wstring path, std::function<bool(const std::wstring&, bool)> saver)
		{ _operation.SetDefaultJamSaver(std::move(path), std::move(saver)); }
		void SetCurrentJamPath(std::wstring path) { _operation.SetCurrentJamPath(std::move(path)); }
		const std::wstring& CurrentJamPath() const noexcept { return _operation.CurrentJamPath(); }
		std::uint64_t Identity() const noexcept { return _sceneIdentity; }
		void ReportFileStatus(const std::string& status);
		void ReportFileOutcome(const std::string& status)
		{ _operation.ReportFileOutcome(status); }
		// Owned values only; off-callback workers may post, UI alone presents.
		void ReportRuntimeNotice(std::string title, std::string message)
		{ _workspace->Notices().Notify(std::move(title),std::move(message)); }
		void SetRuntimeDialogFallback(gui::RuntimeDialogs::Fallback fallback)
		{ _workspace->Notices().SetFallback(std::move(fallback)); }
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
		{ return _operation.RigSaveDialogOpen() || _operation.HoldsWriter() || _workspace->PopupOpen() || _workspace->IsEditingText() || _workspace->Editor().IsEngaged() || _contextualKeyboard.Suppresses(key); }
		void Shutdown();
		bool JobThreadFailed() const noexcept { return _jobThreadFailed.load(std::memory_order_acquire); }
		std::string_view JobThreadFailureDetail() const noexcept { return JobThreadFailed() ? std::string_view(_jobThreadFailureDetail.data()) : std::string_view{}; }
		void SetLogging(io::LoggingConfig config) noexcept;
		bool IsUiVerbose() const noexcept { return _loggingConfig.Ui == "verbose"; }
		void InitMidi(bool generatedRig = false) { _operation.InitMidi(generatedRig); }
		void SetMidiInputInventory(midi::MidiInputInventory inventory) { _operation.SetMidiInputInventory(std::move(inventory)); }
		midi::MidiConnectionResult GetMidiConnectionResult() const { return _operation.MidiSnapshot()->Connection; }
		void CloseMidi() { _operation.CloseMidi(); }
		void RequestMidiRefresh() noexcept { _operation.RequestMidiRefresh(); }
		void InitSerial() {}
		void CloseSerial() {}
		// UI thread only: reconciles pending topology and popup presentation before rendering.
		void CommitChanges();
		// Off-callback hardware-only saver; never persists live membership or routing.
		bool SaveHardwarePreferences(const io::RigFile& rig) const
		{ return _operation.SaveHardwarePreferences(rig); }
		bool ConsumePanelRigLoadRequest() { return std::exchange(_panelRigLoadRequested, false); }
		bool ConsumeRigSaveRequest(); // Main/window thread only.
		using RigSaveMode = SessionControl::RigSaveMode;
		RigSaveMode RequestedRigSaveMode() const { return _operation.RequestedRigSaveMode(); }
		struct CapturedRig { io::RigFile Rig; std::uint64_t Revision = 0u, SceneId = 0u; };
		std::optional<CapturedRig> CaptureCommittedRigVersion() const;
		std::optional<CapturedRig> BeginExplicitRigSave();
		std::optional<io::RigFile> CaptureCommittedRig() const;
		void QueueExplicitRigSave(std::function<void()> work);
		void FinishExplicitRigSave();
		void SetRigSaveDialogOpen(bool open); // Window owner; gates conflicting input.

		RigCoordinator::EditResult RequestRigEdit(const io::RigFile& candidateRig,
			RigCoordinator::EditReceiptPtr* receipt = nullptr);
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
			return _workspace->Editor().Open(take, audioLoop, midiLoop);
		}
		void CloseLoopGridEditor() { _workspace->Editor().Close(); }
		bool IsLoopGridEditorOpen() const noexcept { return _workspace->Editor().IsOpen(); }
		bool LoopGridEditorReady() const noexcept { return _workspace->Editor().IsReady(); }
		float LoopGridEditorMorph() const noexcept { return _workspace->Editor().Morph(); }
		float LoopGridEditorSurroundingDim() const noexcept { return _workspace->Editor().SurroundingDim(); }
		std::shared_ptr<Loop> LoopGridEditorAudioLoop() const noexcept { return _workspace->Editor().AudioLoop(); }
		std::shared_ptr<midi::MidiLoop> LoopGridEditorMidiLoop() const noexcept { return _workspace->Editor().TargetMidiLoop(); }
		std::shared_ptr<LoopTake> LoopGridEditorTake() const noexcept { return _workspace->Editor().Take(); }

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
		static int AudioCallback(void* outBuffer,
			void* inBuffer,
			unsigned int numSamps,
			double streamTime,
			RtAudioStreamStatus status,
			void* userData);

		void _OnAudio(float* inBuffer,
			float* outBuffer,
			unsigned int numSamps);
		void _PublishGroupMuteApplication(std::shared_ptr<const GroupMuteApplication> application);
		void _ExecuteGroupCommandLocked(bool selection, GroupCommand command, std::size_t slot);
		GroupTargetKind _GroupSelectionDepthLocked() const;
		void _RestoreGroupSelectionDepthLocked();
		// Selection helpers require _sceneMutex throughout hierarchy access.
		gui::Workspace::InputPresentation _WorkspaceInputPresentation() const;
		void _CompleteWorkspaceInput();
		SessionControl::Effects _ControlEffects();
		void _ApplyRuntimeAvailabilityLocked(const SessionControl::RuntimeAvailability& update);
		std::optional<actions::ActionResult> _HandleKeyboardCommand(gui::Workspace::KeyboardCommand command, const actions::KeyAction& action,
			bool groupsEnabled, bool contextual, bool suppressedKey);
		void _PrepareStation(const std::shared_ptr<Station>& station);
		void _AddStation(std::shared_ptr<Station> station, bool publishAudioStations = true);
		void _RequestStationControl(gui::GuiStationControls::Intent intent);
		void _RequestTakeDitch(gui::GuiStationControls::Intent intent);
		void _CancelTakeDitch();
		void _ConfirmTakeDitch();
		void _AcceptStationRemoval(bool consent);
		void _CommitStationMembershipLocked();
		RigCoordinator::EditResult _RequestRigEditLocked(const io::RigFile& candidateRig,
			RigCoordinator::EditReceiptPtr* receipt = nullptr);
		std::string _RequestStationRenameLocked(const std::shared_ptr<Station>& station,
			std::uint64_t revision, const std::string& name);
		bool _JobReceiverAvailableLocked(const std::shared_ptr<base::ActionReceiver>& receiver) const;
		bool _StationManagementAvailableLocked() const;

		// Pass a locked station list or a snapshot; remote updates can erase entries.
		void _HandleReclockArm();
		void _SetQuantisation(unsigned int quantiseSamps, utils::Timer::QuantisationType quantisation);
		void _SetMidiQuantisationGrain(unsigned int grainSamps, const char* source);
		void _SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState state, bool fromLocalEdit = false);
		void _SetTransportOffsetLoopFrac(double loopFrac, bool updateInput = true);
		void _ApplyGlobalMidiQuantStateToAllLoopTakes();
		void _ForceGlobalMidiQuantStateMixedOnLocalEdit();
		void _JobLoop(std::function<void()> onStart) noexcept;
		void _FailJobThread(const char* stage, const char* detail, const actions::JobAction* job = nullptr) noexcept;
		void _QueueJobs(std::vector<actions::JobAction> jobs);
		bool _CaptureIsBusy() const;
		bool _SessionHasTakes() const;
		void _PumpSourceLossRecovery();
		void _PumpTriggerStructuralActions();
		void _AdvanceRigPublication();
		gui::RoutingEditAvailability _RoutingEditAvailability();
		void _ConsumeTriggerOutcomes();
		void _PublishAudioStations();
		std::shared_ptr<base::GuiElement> _ChildFromPath(std::vector<unsigned char> path);
		std::shared_ptr<base::GuiElement> _ChildFromPathLocked(const std::vector<unsigned char>& path) const;
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
		void _ClearStationQuantisation();
		bool _HasSelection() const;
		void _HandleRemoteTempoSnapshot(const ninjam::NinjamRemoteSnapshot& snapshot,
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent);
		void _ApplyNinjamTimingUpdate(const ninjam::NinjamTimingUpdate& update);
		void _LogNinjamTempoJoinState();
		void _ApplyRemoteTempoPromptDecision();


	protected:
		static constexpr std::uint8_t  UnresolvedMidiDeviceSlot       = 0xffu;
		std::atomic_bool _isSceneQuitting;
		std::atomic_bool _jobThreadFailed{false}; // Publishes the immutable failure detail to UI; lifetime ends after join.
		std::array<char, 4096> _jobThreadFailureDetail{}; // Job writes once, before release-publishing failure.
		std::atomic_bool _isSceneReset;
		std::unique_ptr<audio::AudioHost> _audioEngine;
		std::unique_ptr<io::IoInputSubsystem> _inputSubsystem;
		engine::SessionControl _operation;
		bool _panelRigLoadRequested = false;
		inline static std::atomic<std::uint64_t> _nextSceneIdentity{1u};
		const std::uint64_t _sceneIdentity = _nextSceneIdentity.fetch_add(1u);
		std::unique_ptr<vst::VstEditorWindowManager> _windowSubsystem;
		std::unique_ptr<ninjam::NinjamNetworkService> _networkService;
		engine::Quantiser _quantisation;
		LocalTiming _localTiming;
		io::LoggingConfig _loggingConfig;
		std::atomic_bool _quantisationInputResetRequested{ false };
		io::JamFile::GlobalMidiQuantState _globalMidiQuantState = io::JamFile::GlobalMidiQuantState::Mixed;
		double _transportOffsetLoopFrac = 0.0;
		PerformanceGroupController _performanceGroups; // scene mutex, never audio
		ContextualKeyboardInput _contextualKeyboard; // Window thread only; no callback state.
		ninjam::NinjamTimingDiagnosticPresenter _ninjamDiagnosticPresenter;
		std::uint64_t _ninjamJoinGeneration = 0u;
		std::uint64_t _ninjamTempoRequestId = 0u;
		ninjam::TempoRequestState _lastLoggedTempoRequestState = ninjam::TempoRequestState::Idle;
		std::vector<std::shared_ptr<Station>> _stations;
		RigCoordinator _rigCoordinator;
		std::vector<std::shared_ptr<Station>> _retiredStations; // Scene mutex; UI reclaims after all callback/job pins release.
		std::optional<ninjam::NinjamRemoteSnapshot> _deferredRemoteSnapshot; // Scene mutex; latest remote membership while a local change commits.
		StationManagement _stationManagement; // All access under _sceneMutex; UI teardown.

		std::uint64_t _rigTransitionRequestedRevision = 0u;
		std::unordered_map<const Trigger*, std::pair<std::uint64_t, std::uint64_t>> _triggerOutcomeCounts;
		std::function<std::chrono::steady_clock::time_point()> _routingEditNow;
		std::uint64_t _lastAudioCallbackHeartbeat = 0u;
		std::chrono::steady_clock::time_point _lastAudioCallbackHeartbeatAt{};
		std::unique_ptr<gui::Workspace> _workspace;
		engine::QuantiserController _quantisationInteraction;
		// Recording publication has separate structural ownership from camera presentation.
		std::shared_ptr<CameraRecordingPublisher> _cameraRecordings = std::make_shared<CameraRecordingPublisher>();
		std::thread _jobRunner;
		std::mutex _jobMutex;
		std::list<actions::JobAction> _jobList;
		std::function<void()> _startupComplete; // Protected by _jobMutex; callback never reads.
		bool _startupJobDiagnostics = false; // Initial queue admission, protected by _jobMutex.
		mutable std::mutex _sceneMutex;
		io::UserConfig _userConfig;
	};
}
