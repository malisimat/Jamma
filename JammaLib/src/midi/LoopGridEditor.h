#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "../actions/ActionResult.h"
#include "../actions/ActionUndoHistory.h"
#include "../actions/KeyAction.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"
#include "../base/DrawContext.h"
#include "../base/GuiElement.h"
#include "../engine/Loop.h"
#include "../engine/LoopTake.h"
#include "../engine/Station.h"
#include "../graphics/Camera.h"
#include "../gui/GuiButton.h"
#include "../gui/GuiLabel.h"
#include "../resources/ResourceLib.h"
#include "MidiGridGesture.h"
#include "MidiGridTargets.h"
#include "MidiPitchViewGesture.h"
#include "LoopGridGeometry.h"
#include "MidiLoop.h"
#include "MidiLoopEditUndo.h"

namespace midi
{
	// UI-thread owner of the loop grid editor: target tracking, camera framing,
	// pointer gestures and overlay controls. A MIDI target is one MidiLoop, never its take.
	class LoopGridEditor
	{
	public:
		// Scene-owned collaborators; every reference must outlive the editor.
		struct Host
		{
			graphics::Camera& Camera;
			actions::ActionUndoHistory& Undo;
			std::mutex& SceneMutex;
			const std::vector<std::shared_ptr<engine::Station>>& Stations;
			std::function<std::shared_ptr<base::GuiElement>()> Hovered;
			std::function<glm::mat4(float aspect)> ViewProjection;
			std::function<void()> OnOpened;
			std::function<bool(int button, utils::Position2d anchor)> BeginRelativePointer;
			std::function<void(int button)> EndRelativePointer;
		};

	public:
		LoopGridEditor(Host host, utils::Size2d size);

		void InitResources(resources::ResourceLib& resourceLib, bool forceInit);
		void ReleaseResources();
		void SetSize(utils::Size2d size);

		bool Open(const std::shared_ptr<engine::LoopTake>& take,
			const std::shared_ptr<engine::Loop>& audioLoop,
			const std::shared_ptr<MidiLoop>& midiLoop);
		void Close();
		void Tick(float deltaSeconds);
		void UpdateUi(const glm::mat4& viewProjection);
		void ApplyToModels();
		void Draw(base::DrawContext& ctx);
		bool FindCandidate(std::shared_ptr<engine::LoopTake>& take,
			std::shared_ptr<engine::Loop>& audioLoop,
			std::shared_ptr<MidiLoop>& midiLoop) const;

		// Each handler returns a result only when the editor consumed the input.
		std::optional<actions::ActionResult> OnAction(actions::TouchAction action);
		std::optional<actions::ActionResult> OnAction(actions::TouchMoveAction action);
		std::optional<actions::ActionResult> OnAction(const actions::KeyAction& action);
		std::optional<actions::ActionResult> TryOpenFromKey(const actions::KeyAction& action);
		void CancelInput();

		bool IsOpen() const noexcept { return State::Opening == _state || State::Active == _state; }
		// Open or closing: the editor owns input and the 3d picker is suspended.
		bool IsEngaged() const noexcept { return State::Closed != _state; }
		bool IsReady() const noexcept;
		bool OwnsPointer() const noexcept { return _pointerOwned; }
		float Morph() const noexcept { return _blend; }
		float SurroundingDim() const noexcept { return 1.0f - 0.72f * _blend; }
		glm::vec3 ProbeEyeLocal() const;
		std::shared_ptr<engine::Loop> AudioLoop() const noexcept { return _audioLoop.lock(); }
		std::shared_ptr<MidiLoop> TargetMidiLoop() const noexcept { return _midiLoop.lock(); }
		std::shared_ptr<engine::LoopTake> Take() const noexcept { return _take.lock(); }

	private:
		enum class State { Closed, Opening, Active, Closing };
		struct CursorEntry
		{
			std::weak_ptr<MidiLoop> Loop;
			std::shared_ptr<actions::MidiEditRevisionCursor> Cursor;
		};

		static actions::ActionResult _Eaten();
		static utils::Position2d _ButtonPosition(utils::Size2d size);
		static float _MidiRadius(std::uint32_t lengthSamps) noexcept;

		void _Layout();
		void _SetFeedback(const std::string& message);
		void _ResetTarget();
		std::string _UnavailableReason(const std::shared_ptr<engine::LoopTake>& take,
			const std::shared_ptr<engine::Loop>& audioLoop,
			const std::shared_ptr<MidiLoop>& midiLoop) const;
		bool _Validate() const;
		void _AcquireRevisionCursor(const std::shared_ptr<MidiLoop>& midiLoop);
		void _FitPitchRange(const std::shared_ptr<MidiLoop>& midiLoop);
		glm::mat4 _ModelMatrix() const;
		glm::mat4 _ViewProjection() const;
		void _PositionCamera();
		graphics::Camera::Pose _OrbitPose() const noexcept;
		void _BeginOrbit(utils::Position2d pointer) noexcept;
		void _UpdateOrbit(utils::Position2d pointer) noexcept;
		void _EndOrbit() noexcept;
		std::optional<MidiGridGesture::Point> _PointAt(utils::Position2d pixel,
			bool clampToGrid) const;
		std::uint8_t _ChannelOf(const std::shared_ptr<MidiLoop>& loop) const;
		double _PixelsPerSample(std::uint32_t lengthSamps) const;
		void _HandleWheel(const actions::TouchAction& action);
		void _BeginGesture(const actions::TouchAction& action);
		bool _BeginSpecialGesture(const actions::TouchAction& action);
		void _UpdateHeldTarget();
		void _EndGesture(const actions::TouchAction& action);
		void _PublishGesture();
		void _CancelGesture();
		void _UpdatePreview();
		void _UpdateHover(MidiGridGesture::Point point);
		void _UpdateIdleHover();
		void _ClearIdleHover(bool clearCache = false);
		void _CheckGesture();
		bool _HandleButton(actions::TouchAction action);

	private:
		Host _host;
		utils::Size2d _size;
		State _state = State::Closed;
		std::weak_ptr<engine::Station> _station;
		std::weak_ptr<engine::LoopTake> _take;
		std::weak_ptr<engine::Loop> _audioLoop;
		std::weak_ptr<MidiLoop> _midiLoop;
		graphics::Camera::EditorReturnState _returnCamera{};
		glm::vec3 _orbitCentre{};
		float _orbitRadius = 1.0f;
		float _orbitHorizontal = 0.0f;
		float _orbitVertical = 0.0f;
		float _orbitAnchorHorizontal = 0.0f;
		float _orbitAnchorVertical = 0.0f;
		utils::Position2d _orbitPointerAnchor{};
		bool _orbitDragging = false;
		float _blend = 0.0f;
		bool _buttonPressed = false;
		bool _buttonShowsClose = false;
		bool _pointerOwned = false;
		int _pointerButton = -1;
		bool _relativePointer = false;
		bool _pitchView = false;
		MidiPitchViewGesture _pitchViewGesture;
		utils::Position2d _pitchViewPointerAnchor{};
		std::uint64_t _gestureModelGeneration = 0u;
		bool _previewDirty = false;
		// UI-owned: retain pixels so camera and pitch-range changes refresh stationary hover.
		std::optional<utils::Position2d> _idleHoverPointer;
		bool _idleHoverCacheValid = false;
		std::uint64_t _idleHoverCacheRevision = 0u;
		std::uint32_t _idleHoverCacheLength = 0u;
		MidiQuantisationSettings _idleHoverCacheQuantisation{};
		std::uint64_t _idleHoverCacheTransportStart = 0u;
		MidiGridTargets _idleHoverTargets;
		std::optional<LoopGridGeometry> _idleHoverGrid;
		std::uint64_t _hoverRevision = 0u; // UI-owned; invalidates model-instance targets
		std::unique_ptr<MidiGridGesture> _gesture;
		std::shared_ptr<actions::MidiEditRevisionCursor> _revisionCursor;
		std::vector<CursorEntry> _revisionCursors;
		std::string _feedbackText;
		std::shared_ptr<gui::GuiButton> _button;
		std::shared_ptr<gui::GuiLabel> _feedback;
		std::shared_ptr<gui::GuiLabel> _modeLabel;
		std::array<std::shared_ptr<gui::GuiLabel>, 5> _timeTicks;
		std::array<std::shared_ptr<gui::GuiLabel>, 11> _pitchTicks;
	};
}
