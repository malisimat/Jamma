#pragma once

// Job/UI orchestrator: presents and forwards subsystem values without taking over
// audio-callback application, remote timing authority, or per-entity loop state.
#include <atomic>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
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

namespace engine
{
	class SceneParams :
		public base::DrawableParams,
		public base::MoveableParams,
		public base::SizeableParams
	{
	public:
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
		void InitAudio();
		void CloseAudio();
		bool InitGlobalKeyCapture();
		void CloseGlobalKeyCapture();
		bool PumpGlobalKeyCapture(actions::KeyAction& action) noexcept;
		void Shutdown();
		void SetLogging(io::LoggingConfig config) noexcept;
		bool IsUiVerbose() const noexcept { return _loggingConfig.Ui == "verbose"; }
		void InitMidi()
		{
			_inputSubsystem->Init(_audioEngine->GetMidiClockAnchor_Ref());
		}
		void CloseMidi()
		{
			_inputSubsystem->Close();
		}
		void InitSerial() {}
		void CloseSerial() {}
		void CommitChanges();
		bool SaveRig(const io::RigFile& rig) const { return _saveRig && _saveRig(rig); }
		RigCoordinator::EditResult RequestRigEdit(const io::RigFile& candidateRig);
		void ApplyDeferredHoverUpdates();

		// Returns a locked station snapshot safe to use outside render/tick threads.
		std::vector<std::shared_ptr<Station>> SnapshotStations() const;

		// Send a chat message on the active ninjam session (no-op if none).
		void SendNinjamChat(const std::string& msg)
		{
			_networkService->SendChat(msg);
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
		// Call on the main/non-audio thread during shutdown.
		void ForceUnloadAllVstPlugins();
		
	protected:
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
		glm::mat4 _View();
		void _AddStation(std::shared_ptr<Station> station, bool publishAudioStations = true);
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
		void _AdvanceRigPublication();
		gui::RoutingEditAvailability _RoutingEditAvailability();
		void _PumpSerial();
		void _PublishAudioStations();
		std::shared_ptr<base::GuiElement> _ChildFromPath(std::vector<unsigned char> path);
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
		bool _HasQuantisationSelection() const;
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


	protected:
		static constexpr std::uint8_t  UnresolvedMidiDeviceSlot       = 0xffu;
		static constexpr unsigned int MidiChannelOverrideControlIndex = 7001u;
		static constexpr unsigned int TransportOffsetControlIndex = 7002u;
		static constexpr unsigned int NinjamMetronomeControlIndex = 7003u;
		static constexpr unsigned int NinjamRemoteTempoAcceptControlIndex = 7101u;
		static constexpr unsigned int NinjamRemoteTempoRejectControlIndex = 7102u;

		bool _isSceneTouching;
		std::atomic_bool _isSceneQuitting;
		std::atomic_bool _isSceneReset;
		glm::mat4 _viewProj;
		glm::mat4 _overlayViewProj;
		glm::mat4 _viewRotOnlyProj;
		glm::mat4 _skyboxViewProj;
		bool _skyboxStarted;
		Time _skyboxStartTime;
		graphics::Skybox _skybox;
		std::unique_ptr<audio::AudioHost> _audioEngine;
		std::unique_ptr<io::IoInputSubsystem> _inputSubsystem;
		std::unique_ptr<vst::VstEditorWindowManager> _windowSubsystem;
		std::unique_ptr<ninjam::NinjamNetworkService> _networkService;
		engine::Quantiser _quantisation;
		io::LoggingConfig _loggingConfig;
		std::shared_ptr<gui::GuiRadio> _modeRadio;
		std::shared_ptr<gui::GuiNumericInput> _midiChannelOverrideInput;
		std::shared_ptr<gui::GuiNumericInput> _transportOffsetInput;
		std::shared_ptr<gui::GuiToggle> _ninjamMetronomeToggle;
		std::shared_ptr<gui::GuiRadio> _globalMidiQuantRadio;
		io::JamFile::GlobalMidiQuantState _globalMidiQuantState = io::JamFile::GlobalMidiQuantState::Mixed;
		double _transportOffsetLoopFrac = 0.0;
		std::unique_ptr<gui::GuiLabel> _label;
		std::unique_ptr<gui::SceneSelector> _selector;
		std::shared_ptr<gui::GuiMainPanel> _mainPanel;
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
		std::uint64_t _lastAudioCallbackHeartbeat = 0u;
		std::chrono::steady_clock::time_point _lastAudioCallbackHeartbeatAt{};
		actions::ActionUndoHistory _undoHistory;
		std::weak_ptr<base::GuiElement> _touchDownElement;
		// Whether the touch sequence started on the HUD panel, recorded at touch-down.
		bool _touchDownIsHud = false;
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
		std::thread _jobRunner;
		std::mutex _jobMutex;
		std::list<actions::JobAction> _jobList;
		mutable std::mutex _sceneMutex;
		io::UserConfig _userConfig;
		std::function<bool(const io::RigFile&)> _saveRig;
		ViewMode _viewMode;
		utils::Position2d _cursorPos{};
	};
}
