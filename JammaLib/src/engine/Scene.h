#pragma once
#include <atomic>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include "../resources/ResourceLib.h"
#include "../actions/JobAction.h"
#include "../audio/AudioDevice.h"
#include "../audio/MidiDevice.h"
#include "../audio/ChannelMixer.h"
#include "../graphics/Image.h"
#include "../graphics/Camera.h"
#include "../graphics/GlDrawContext.h"
#include "../graphics/Skybox.h"
#include "../gui/GuiLabel.h"
#include "../gui/GuiSlider.h"
#include "../gui/GuiSelector.h"
#include "../gui/GuiRadio.h"
#include "../io/JamFile.h"
#include "../io/RigFile.h"
#include "NinjamSession.h"
#include "Quantisation.h"
#include "../graphics/VstEditorWindow.h"
#include "Tickable.h"
#include "Drawable.h"
#include "ActionReceiver.h"
#include "AudioSource.h"
#include "Moveable.h"
#include "Sizeable.h"
#include "GuiElement.h"
#include "Station.h"
#include "StationRemote.h"
#include "UndoHistory.h"
#include "MidiQueue.h"

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
			ReleaseResources();
			CloseMidi();

			_isSceneQuitting.store(true, std::memory_order_release);
			if (_jobRunner.joinable())
				_jobRunner.join();
			// NinjamSession destructs after _jobRunner exits, so Pump() can no
			// longer be called when the session tears down.
			_ninjamSession.reset();
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
			other._selector = std::make_unique<gui::GuiSelector>(
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
		void SetHover3d(std::vector<unsigned char> path, base::Action::Modifiers modifiers);
		unsigned int Width() const { return _sizeParams.Size.Width; }
		unsigned int Height() const { return _sizeParams.Size.Height; }
		void Reset();
		void InitGui();
		void InitAudio();
		void CloseAudio();
		void InitMidi();
		void CloseMidi();
		void CommitChanges();

		// Send a chat message on the active ninjam session (no-op if none).
		void SendNinjamChat(const std::string& msg);

		// Close all open VST editor windows immediately.
		// Call this on the main thread before OleUninitialize() during shutdown.
		void CloseAllVstEditorWindows();

		// Connect to an arbitrary NINJAM host ("host:port"). Reuses credentials
		// from the loaded jam config when available; falls back to anonymous.
		void ConnectNinjam(const std::string& host);

		// Disconnect the active NINJAM session. No-op if not connected.
		void DisconnectNinjam();
		
	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

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
		void _SetQuantisation(unsigned int quantiseSamps, Timer::QuantisationType quantisation);
		void _JobLoop();
		void _PumpMidi();
		void _PublishAudioStations();
		std::shared_ptr<base::GuiElement> _ChildFromPath(std::vector<unsigned char> path);
		void _UpdateSelectDepth(unsigned int depth);
		void _UpdateRemoteStationsFromSnapshot(const io::NinjamRemoteSnapshot& snapshot);
		QuantisationPolicy _QuantisationPolicy() const;
		unsigned int _CurrentSampleRate() const;
		std::uint64_t _EstimatedAudioSampleAt(Time actionTime) const;
		void _ApplyQuantisationTiming(const QuantisationTiming& timing, const char* source);
		bool _HandleTapTempo(Time actionTime);
		bool _TrySetMasterFromHover(bool confirm);
		void _RefreshQuantisationOverlays(std::shared_ptr<base::GuiElement> candidate, base::SelectDepth depth, bool confirmCandidate);
		void _ClearQuantisationOverlays();
		unsigned long _MasterLengthForTarget(const std::shared_ptr<base::GuiElement>& target, base::SelectDepth depth) const;
		std::shared_ptr<Loop> _RepresentativeLoopForTarget(const std::shared_ptr<base::GuiElement>& target, base::SelectDepth depth) const;
		void _QueueLocalTempoFromClock();
		void _SendQueuedTempoAtIntervalWrap(const io::NinjamRemoteSnapshot& snapshot);
		void _ApplyRemoteTempoToClock(const io::NinjamRemoteSnapshot& snapshot);
		void _PruneClosedVstEditorWindows();
		bool _OpenVstEditorForPlugin(const std::shared_ptr<vst::VstPlugin>& plugin);
		bool _TryOpenVstEditorForLoop(const std::shared_ptr<Loop>& loop, size_t pluginIndex);
		bool _TryOpenVstEditorForStation(const std::shared_ptr<Station>& station, size_t pluginIndex);
		bool _TryOpenVstEditorForHover(const std::shared_ptr<base::GuiElement>& hovering,
			base::SelectDepth depth,
			size_t pluginIndex);


	protected:
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
		std::shared_ptr<audio::ChannelMixer> _channelMixer;
		std::unique_ptr<audio::AudioDevice> _audioDevice;
		std::unique_ptr<audio::MidiDevice> _midiDevice;
		MidiQueue<1024> _midiIngress;
		std::uint64_t _lastMidiDropCount;
		std::shared_ptr<gui::GuiRadio> _modeRadio;
		std::unique_ptr<gui::GuiLabel> _label;
		std::unique_ptr<gui::GuiSelector> _selector;
		std::vector<std::shared_ptr<Station>> _stations;
		std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<Station>>>> _audioStations;
		std::optional<io::JamFile::NinjamConfig> _ninjamConfig;
		std::unique_ptr<NinjamSession> _ninjamSession;
		UndoHistory _undoHistory;
		std::weak_ptr<base::GuiElement> _touchDownElement;
		std::weak_ptr<base::GuiElement> _hoverElement3d;
		std::shared_ptr<Loop> _masterLoop;
		unsigned long _masterLoopLengthSamps;
		TapTempoTracker _tapTempo;
		// Open plugin editor windows created from the UI (main thread only).
		std::vector<std::unique_ptr<graphics::VstEditorWindow>> _vstEditorWindows;
		std::atomic<std::uint64_t> _audioSampleCounter;
			std::atomic<std::int64_t> _midiAnchorMicros;
		graphics::Camera _camera;
		std::thread _jobRunner;
		std::mutex _jobMutex;
		std::list<actions::JobAction> _jobList;
		std::mutex _audioMutex;
		std::mutex _tempoMutex;
		io::UserConfig _userConfig;
		std::shared_ptr<Timer> _clock;
		ViewMode _viewMode;

		// NINJAM tempo / reclock state (job-thread owned).
		unsigned int _remoteMasterLoopSamps = 0u;
		unsigned int _remoteSampleRate = 0u;
		unsigned int _effectiveQuantiseSamps = 0u;
		unsigned int _lastRemoteIntervalPos = 0u;
		bool _armReclock = false;
		bool _hasPendingTempo = false;
	};
}
