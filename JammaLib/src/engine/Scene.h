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
#include <thread>
#include <unordered_map>
#include <utility>
#include "../resources/ResourceLib.h"
#include "../actions/JobAction.h"
#include "../audio/AudioDevice.h"
#include "../audio/ChannelMixer.h"
#include "../graphics/Image.h"
#include "../graphics/Camera.h"
#include "../graphics/GlDrawContext.h"
#include "../graphics/Skybox.h"
#include "../gui/GuiLabel.h"
#include "../gui/GuiButton.h"
#include "../gui/GuiPopup.h"
#include "../gui/GuiFocusManager.h"
#include "../gui/GuiNumericInput.h"
#include "../gui/GuiToggle.h"
#include "../gui/GuiPopupManager.h"
#include "../gui/SceneSelector.h"
#include "../gui/GuiMainPanel.h"
#include "../gui/GuiHud.h"
#include "../gui/GuiRadio.h"
#include "../io/JamFile.h"
#include "../io/RigFile.h"
#include "../io/InitFile.h"
#include "../io/SerialDevice.h"
#include "../ninjam/NinjamController.h"
#include "../audio/AudioHost.h"
#include "../io/IoInputSubsystem.h"
#include "../ninjam/NinjamNetworkService.h"
#include "../vst/VstEditorWindowManager.h"
#include "../midi/MidiDevice.h"
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
#include "StationRemote.h"
#include "RigCoordinator.h"
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
		void AdvanceUiAnimations();

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
		virtual void InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		void InitReceivers();
		void AddChild(std::shared_ptr<base::GuiElement> child);
		void SetHover3d(std::vector<unsigned char> path, base::Action::Modifiers modifiers);
		unsigned int Width() const { return _sizeParams.Size.Width; }
		unsigned int Height() const { return _sizeParams.Size.Height; }
		void Reset();
		void InitGui();
		void InitAudio(bool generatedRig = false,
			const audio::AsioInventory* inventory = nullptr);
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
		void Shutdown();
		void SetLogging(io::LoggingConfig config) noexcept;
		bool IsUiVerbose() const noexcept { return _loggingConfig.Ui == "verbose"; }
		void InitMidi(bool generatedRig = false)
		{
			std::scoped_lock midiLock(_midiLifecycleMutex);
			auto result = _inputSubsystem->Init(_audioEngine->GetMidiClockAnchor_Ref(),
				_audioEngine->GetStreamParams().SampleRate,
				_midiInputInventory ? &*_midiInputInventory : nullptr, generatedRig);
			_midiInputInventory.reset();
			std::vector<std::string> connectedNames;
			connectedNames.reserve(result.Connected.size());
			for (const auto& endpoint : result.Connected)
				if (std::find(connectedNames.begin(), connectedNames.end(), endpoint.Name) == connectedNames.end())
					connectedNames.push_back(endpoint.Name);
			_midiState.store(
				std::make_shared<const MidiState>(MidiState{ std::move(result), std::move(connectedNames) }),
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
			_inputSubsystem->Close();
		}
		void RequestMidiRefresh() noexcept { _midiRefreshRequested.store(true, std::memory_order_release); }
		void InitSerial() {}
		void CloseSerial() {}
		void CommitChanges();
		bool SaveRig(const io::RigFile& rig) const { return _saveRig && _saveRig(rig); }
		RigCoordinator::EditResult RequestRigEdit(const io::RigFile& candidateRig);
		std::shared_ptr<const RigSnapshot> AcceptedRigSnapshot() const noexcept
		{
			return _rigCoordinator.Accepted();
		}
		void ApplyDeferredHoverUpdates();
		void SetRelativePointerHost(std::function<bool(int, utils::Position2d)> begin,
			std::function<void(int)> end);
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
		void _UpdateSelection(actions::ActionResultType res);
		void _UpdateRackVisibilityLocked();
		void _CollapseRacksLocked();
		// Selection helpers require _sceneMutex throughout hierarchy access.
		void _SetSelectionMutedLocked(const std::vector<unsigned char>& path, bool muted);
		base::Tweakable::TweakState _SelectionTweakStateLocked(const std::shared_ptr<base::GuiElement>& target) const;
		void _AddStation(std::shared_ptr<Station> station, bool publishAudioStations = true);
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
		void _JobLoop();
		void _PumpMidi();
		void _RefreshMidiIfNeeded();
		void _ConfirmMidiConnectionChange(const midi::MidiConnectionResult& previous,
			const midi::MidiConnectionResult& refreshed, const std::vector<std::string>& connectedNames);
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
		void _LogNinjamTimingDiagnostics(const ninjam::NinjamTimingDiagnostics& diagnostics);
		void _LogNinjamTempoJoinState();
		void _EnsureRemoteTempoPromptUi();
		void _OpenRemoteTempoPromptIfNeeded();
		void _HandleRemoteTempoPromptDecision(bool accept);
		void _CloseRemoteTempoPrompt();
		void _OnLoopGridEditorOpened();
		void _ReleaseQuantisationInput();
		bool _EditControlsSuppressed() const;
		std::optional<actions::ActionResult> _RouteQuantisationTouch(actions::TouchAction action);
		std::optional<actions::ActionResult> _RouteQuantisationMove(actions::TouchMoveAction action);


	protected:
		static constexpr std::uint8_t  UnresolvedMidiDeviceSlot       = 0xffu;
		static constexpr unsigned int MidiChannelOverrideControlIndex = 7001u;
		static constexpr unsigned int TransportOffsetControlIndex = 7002u;
		static constexpr unsigned int NinjamMetronomeControlIndex = 7003u;
		static constexpr unsigned int TapTempoControlIndex = 7004u;
		static constexpr unsigned int MidiSubdivisionControlIndex = 7005u;
		static constexpr unsigned int NinjamRemoteTempoAcceptControlIndex = 7101u;
		static constexpr unsigned int NinjamRemoteTempoRejectControlIndex = 7102u;

		bool _isSceneTouching;
		std::atomic_bool _isSceneQuitting;
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
			std::vector<std::string> ConnectedNames;
		};
		std::atomic<std::shared_ptr<const MidiState>> _midiState = std::make_shared<const MidiState>();
		std::mutex _midiLifecycleMutex;
		std::atomic<bool> _midiRefreshRequested{ false };
		std::atomic<bool> _midiActive{ false };
		std::chrono::steady_clock::time_point _lastMidiInventoryCheck{};
		std::uint64_t _sourceRecoveryHeartbeat = 0u; // Job owner only.
		std::chrono::steady_clock::time_point _sourceRecoveryHeartbeatAt{};
		bool _sourceLossRecoveryGateClosed = false; // Serialized by _sceneMutex.
		std::uint64_t _audioStreamEpoch = 0u; // Serialized lifecycle owner.
		std::unique_ptr<vst::VstEditorWindowManager> _windowSubsystem;
		std::unique_ptr<ninjam::NinjamNetworkService> _networkService;
		engine::Quantiser _quantisation;
		io::LoggingConfig _loggingConfig;
		std::shared_ptr<gui::GuiRadio> _modeRadio;
		std::shared_ptr<gui::GuiNumericInput> _midiChannelOverrideInput;
		std::shared_ptr<gui::GuiNumericInput> _transportOffsetInput;
		std::shared_ptr<gui::GuiToggle> _ninjamMetronomeToggle;
		std::shared_ptr<gui::GuiRadio> _globalMidiQuantRadio;
		std::shared_ptr<gui::GuiRadio> _midiSubdivisionRadio;
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
		std::vector<std::shared_ptr<base::GuiElement>> _guiChildren;
		gui::GuiFocusManager _focusManager;
		gui::GuiPopupManager _popupManager;
		bool _remoteTempoDialogOpen = false;
		std::uint64_t _lastPresentedNinjamDiagnosticSequence = 0u;
		std::uint64_t _lastPresentedNinjamDiagnosticOverflowCount = 0u;
		std::uint64_t _ninjamJoinGeneration = 0u;
		std::uint64_t _ninjamTempoRequestId = 0u;
		ninjam::TempoRequestState _lastLoggedTempoRequestState = ninjam::TempoRequestState::Idle;
		std::shared_ptr<gui::GuiPopup> _remoteTempoDialog;
		std::vector<std::shared_ptr<Station>> _stations;
		RigCoordinator _rigCoordinator;
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
		graphics::Camera _camera;
		std::optional<Time> _lastCameraUpdateTime;
		std::optional<Time> _lastPanelAnimationTime;
		std::thread _jobRunner;
		std::mutex _jobMutex;
		std::list<actions::JobAction> _jobList;
		mutable std::mutex _sceneMutex;
		io::UserConfig _userConfig;
		std::function<bool(const io::RigFile&)> _saveRig;
		ViewMode _viewMode;
		utils::Position2d _cursorPos{};
		std::function<bool(int, utils::Position2d)> _beginRelativePointer;
		std::function<void(int)> _endRelativePointer;
		// Declared last: it holds references to the members above.
		midi::LoopGridEditor _loopEditor;
	};
}
