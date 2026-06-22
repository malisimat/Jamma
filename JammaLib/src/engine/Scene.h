#pragma once
#include <atomic>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
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
#include "../gui/GuiFocusManager.h"
#include "../gui/GuiPopupHost.h"
#include "../gui/SceneSelector.h"
#include "../gui/GuiMainPanel.h"
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
#include "../timing/TimingQuantiser.h"
#include "Tickable.h"
#include "Drawable.h"
#include "ActionReceiver.h"
#include "AudioSource.h"
#include "Moveable.h"
#include "Sizeable.h"
#include "GuiElement.h"
#include "Station.h"
#include "StationRemote.h"
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
		/*
		// Move
		Scene(Scene&& other) :
			base::Tickable(std::move(other)),
			base::Drawable(std::move(other)),
			base::Sizeable(std::move(other)),
			_viewProj(other._viewProj),
			_overlayViewProj(other._overlayViewProj),
			_channelMixer(std::move(other._channelMixer)),
			_audioDevice(std::move(other._audioDevice)),
			_label(std::move(other._label)),
			_selector(std::move(other._selector)),
			_undoHistory(std::move(other._undoHistory)),
			_stations(std::move(other._stations)),
			_touchDownElement(other._touchDownElement),
			_hoverElement3d(other._hoverElement3d),
			_touchDownElement3d(other._touchDownElement3d),
			_masterLoop(other._masterLoop)
		{
			other._stations = std::vector<std::shared_ptr<Station>>();
			other._viewProj = glm::mat4();
			other._overlayViewProj = glm::mat4();
			other._channelMixer = std::make_unique<audio::ChannelMixer>();
			other._audioDevice = std::make_unique<audio::AudioDevice>();
			other._label = std::make_unique<gui::GuiLabel>(
				gui::GuiLabelParams(
					base::GuiElementParams(
						base::DrawableParams{ "" },
						base::MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
						base::SizeableParams{ 1,1 },
						"",
						"",
						"",
						{}),
					""));
			other._selector = std::make_unique<gui::SceneSelector>(
				gui::GuiSelectorParams(
					base::GuiElementParams(
						base::DrawableParams{ "" },
						base::MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
						base::SizeableParams{ 1,1 },
						"",
						"",
						"",
						{}),
					""));
			_undoHistory = UndoHistory();
			other._masterLoop = std::make_shared<Loop>(LoopParams());
		}

		Scene& operator=(Scene&& other)
		{
			if (this != &other)
			{
				ReleaseResources();

				std::swap(_viewProj, other._viewProj);
				std::swap(_overlayViewProj, other._overlayViewProj);
				_channelMixer.swap(other._channelMixer);
				_audioDevice.swap(other._audioDevice);
				_label.swap(other._label);
				_selector.swap(other._selector);
				_stations.swap(other._stations);
				_undoHistory.swap(other._undoHistory);
				std::swap(_touchDownElement, other._touchDownElement),
				std::swap(_hoverElement3d, other._hoverElement3d),
				std::swap(_touchDownElement3d, other._touchDownElement3d),
				_masterLoop.swap(other._masterLoop);
				std::swap(_drawParams, other._drawParams);
				std::swap(_sizeParams, other._sizeParams);
				std::swap(_texture, other._texture);
			}

			return *this;
		}*/

		static std::optional<std::shared_ptr<Scene>> FromFile(SceneParams sceneParams,
			io::JamFile jam,
			io::RigFile rig,
			std::wstring dir);
		
		virtual void Draw(base::DrawContext& ctx) override;
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

		virtual void SetSize(utils::Size2d size) override
		{
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
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params) override;
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
		bool InitGlobalInsertCapture();
		void CloseGlobalInsertCapture();
		bool PumpGlobalInsertCapture(actions::KeyAction& action) noexcept;
		void Shutdown();
		void SetLogging(io::LoggingConfig config) noexcept;
		void InitMidi()
		{
			_inputSubsystem->Init(_audioEngine->GetAudioSampleCounter_Ref(), _audioEngine->GetMidiAnchorMicros_Ref());
		}
		void CloseMidi()
		{
			_inputSubsystem->Close();
		}
		void InitSerial() {}
		void CloseSerial() {}
		void CommitChanges();
		void ResolveDeferredHover();

		// Returns a locked snapshot of the current station list.  Always use
		// this when reading _stations from outside the render/tick thread (e.g.
		// exporters, network handlers, tests).  Holding the snapshot keeps the
		// shared_ptrs alive even if _stations is mutated on another thread.
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
		void ConnectNinjam(const std::string& host)
		{
			_networkService->Connect(host);
		}

		// Disconnect the active NINJAM session. No-op if not connected.
		void DisconnectNinjam()
		{
			_networkService->Disconnect();
		}
		
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
		void _UpdateSelection(actions::ActionResultType res);
		glm::mat4 _View();
		void _AddStation(std::shared_ptr<Station> station);
		void _HandleReclockArm();
		actions::ActionResult _HandleUndo();
		void _SetQuantisation(unsigned int quantiseSamps, utils::Timer::QuantisationType quantisation);
		void _SetMidiQuantisationGrain(unsigned int grainSamps, const char* source);
		void _SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState state, bool fromLocalEdit = false);
		void _ApplyGlobalMidiQuantStateToAllLoopTakes();
		void _ForceGlobalMidiQuantStateMixedOnLocalEdit();
		void _JobLoop();
		void _PumpMidi();
		void _RegisterMidiTriggerRoute(const std::string& deviceName, std::shared_ptr<Trigger> trigger);
		void _PumpSerial();
		void _PublishAudioStations();
		std::shared_ptr<base::GuiElement> _ChildFromPath(std::vector<unsigned char> path);
		void _UpdateSelectDepth(unsigned int depth);
		void _UpdateRemoteStationsFromSnapshot(const ninjam::NinjamRemoteSnapshot& snapshot);
		timing::QuantisationPolicy _QuantisationPolicy() const;
		unsigned int _CurrentSampleRate() const;
		std::uint64_t _EstimatedAudioSampleAt(Time actionTime) const;
		void _ApplyQuantisationTiming(const timing::QuantisationTiming& timing, const char* source);
		void _ClearTimingState(bool clearTapTempo);
		void _ResetIfEmpty();
		bool _HandleTapTempo(Time actionTime);
		void _PulseQuantisationOverlay();
		void _SetQuantisationOverlayHeld(bool held);
		float _QuantisationOverlayAlpha(Time now) const;
		void _ApplyQuantisationOverlayAlpha(float alpha);
		timing::QuantisationInteractionContext _InteractionContext() const;
		void _InvalidateHover2d();
		std::vector<std::weak_ptr<base::GuiElement>> _ResolveHoverPath2d();
		void _ApplyHoverPath2d(const std::vector<std::weak_ptr<base::GuiElement>>& nextPath);
		static std::vector<std::shared_ptr<base::GuiElement>> _LockHoverPath(const std::vector<std::weak_ptr<base::GuiElement>>& path);
		static size_t _SharedHoverPathPrefix(const std::vector<std::shared_ptr<base::GuiElement>>& lhs,
			const std::vector<std::shared_ptr<base::GuiElement>>& rhs);
		actions::ActionResult _BeginBackgroundDrag(actions::TouchAction action);
		actions::ActionResult _UpdateBackgroundDrag(actions::TouchMoveAction action);
		void _EndBackgroundDrag();
		bool _TrySetMasterFromHover(bool confirm);
		void _UpdateStationQuantisation(std::shared_ptr<base::GuiElement> candidate, base::SelectDepth depth, bool confirmCandidate);
		void _ClearStationQuantisation();
		bool _HasQuantisationSelection() const;
		bool _HasQuantisationHover() const;
		bool _IsMidiPhaseDragModifier(base::Action::Modifiers modifiers) const noexcept;
		void _QueueLocalTempoFromClock()
		{
			_networkService->QueueLocalTempoFromClock(_quantisation, _userConfig, _CurrentSampleRate());
		}
		void _SendQueuedTempoAtIntervalWrap(const ninjam::NinjamRemoteSnapshot& snapshot)
		{
			_networkService->SendQueuedTempoAtIntervalWrap(snapshot, _quantisation, _CurrentSampleRate());
		}
		void _ApplyRemoteTempoToClock(const ninjam::NinjamRemoteSnapshot& snapshot)
		{
			_networkService->ApplyRemoteTempoToClock(snapshot, _quantisation, _stations, _userConfig);
		}


	protected:
		static constexpr std::uint8_t  UnresolvedMidiDeviceSlot       = 0xffu;

		bool _isSceneTouching;
		std::atomic_bool _isSceneQuitting;
		std::atomic_bool _isSceneReset;
		bool _isSceneDragged;
		utils::Position2d _initTouchDownPosition;
		utils::Position3d _initTouchCamPosition;
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
		timing::TimingQuantiser _quantisation;
		io::LoggingConfig _loggingConfig;
		std::shared_ptr<gui::GuiRadio> _modeRadio;
		std::shared_ptr<gui::GuiRadio> _globalMidiQuantRadio;
		io::JamFile::GlobalMidiQuantState _globalMidiQuantState = io::JamFile::GlobalMidiQuantState::Mixed;
		std::unique_ptr<gui::GuiLabel> _label;
		std::unique_ptr<gui::SceneSelector> _selector;
		std::shared_ptr<gui::GuiMainPanel> _mainPanel;
		std::vector<std::shared_ptr<base::GuiElement>> _guiChildren;
		gui::GuiFocusManager _focusManager;
		gui::GuiPopupHost _popupHost;
		std::vector<std::shared_ptr<Station>> _stations;
		actions::ActionUndoHistory _undoHistory;
		std::weak_ptr<base::GuiElement> _touchDownElement;
		std::weak_ptr<base::GuiElement> _hoverElement3d;
		std::vector<unsigned char> _hoverPath3d;
		std::vector<std::weak_ptr<base::GuiElement>> _hoverPath2d;
		bool _hover2dDirty;
		std::vector<unsigned char> _lastLoggedHoverPath;
		graphics::CtrlHandleOverlay _ctrlHandleOverlay;
		timing::TimingQuantiserController _quantisationInteraction;
		graphics::Camera _camera;
		std::thread _jobRunner;
		std::mutex _jobMutex;
		std::list<actions::JobAction> _jobList;
		mutable std::mutex _sceneMutex;
		io::UserConfig _userConfig;
		ViewMode _viewMode;
		utils::Position2d _cursorPos{};
	};
}
