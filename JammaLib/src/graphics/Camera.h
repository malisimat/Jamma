#pragma once

#include "Moveable.h"
#include "CameraFraming.h"
#include "UnattendedCameraController.h"
#include "../actions/ActionResult.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace graphics
{
	class CameraParams : public base::MoveableParams
	{
	public:
		CameraParams(base::MoveableParams moveParams,
			unsigned int id) :
			base::MoveableParams(moveParams),
			Id(id)
		{}

	public:
		unsigned int Id;
	};

	class Camera :
		public base::Moveable
	{
	public:
		enum class View
		{
			Front,
			StationInterior,
			TopDown
		};

		struct Pose
		{
			utils::Position3d Eye{};
			utils::Position3d Forward{ 0.0f, 0.0f, -1.0f };
			utils::Position3d Up{ 0.0f, 1.0f, 0.0f };
		};

		// Editor entry must not replace any of the user's remembered view poses.
		struct EditorReturnState
		{
			View ActiveView = View::Front;
			Pose TargetPose{};
			std::array<Pose, 3u> RememberedPoses{};
			std::array<bool, 3u> HasRememberedPose{};
			bool InteriorForcedLoopTakeDepth = false;
			bool InteriorSelectDepthChanged = false;
			bool InteriorRestorePending = false;
			std::shared_ptr<const void> InteriorFocusIdentity;
		};

		enum class SelectDepthChange { None, Station, LoopTake };

		struct FrameTime { float DeltaSeconds; bool DiagnosticDue; };
		struct TakeFraming
		{
			std::weak_ptr<engine::LoopTake> Identity;
			CameraBounds Bounds;
			utils::Position3d Centre;
		};
		struct StationFraming
		{
			std::weak_ptr<engine::Station> Identity;
			CameraBounds Bounds;
			utils::Position3d Centre;
		};
		struct Presentation
		{
			std::vector<std::weak_ptr<engine::LoopTake>> LiveTakes;
			std::vector<TakeFraming> Takes;
			std::vector<StationFraming> Stations;
			CameraBounds OverviewBounds, VisibleBounds;
		};

		Camera(CameraParams camParams);
		// Window-only idle policy and physics use copied presentation observations;
		// the caller samples membership under its own lock before entering here.
		FrameTime BeginPresentation(Time now);
		void AdvancePresentation(Time now, float deltaSeconds, const Presentation& presentation,
			const engine::CameraRecordingObservation& recordings, bool contextAllowsIdle, utils::Size2d viewport);
		void HumanActivity(Time now);
		void TriggerActivity(Time now);
		void SetInteractionEligibility(bool windowEligible, bool heldInteraction,
			const engine::CameraRecordingObservation& recordings, Time now);

		actions::ActionResult HandleBackgroundDrag(actions::TouchAction action);
		actions::ActionResult UpdateBackgroundDrag(actions::TouchMoveAction action);
		actions::ActionResult HandleWheel(int wheelNotches);
		actions::ActionResult HandleWheel(int wheelNotches,
			utils::Position2d cursorPosition,
			unsigned int viewportWidth,
			unsigned int viewportHeight,
			utils::Position3d stationCentre);
		void TickBackgroundDrag(float deltaSeconds);
		void SetAutomaticTarget(utils::Position3d eye) noexcept;
		void TickAutomaticMotion(float deltaSeconds) noexcept;
		void CancelAutomaticMotion(bool preserveVelocity = false) noexcept;
		bool IsAutomatic() const noexcept { return _automaticTarget.has_value(); }
		void SetFrontVisibleBounds(const CameraBounds& bounds) noexcept { _frontVisibleBounds = bounds; }
		bool IsBackgroundDragging() const noexcept;
		bool IsBackgroundPointerHeld() const noexcept;
		void CancelBackgroundCoast() noexcept;
		bool BackgroundDragWasDragged() const noexcept;
		bool IsTransitioning() const noexcept;
	View CurrentView() const noexcept;
	Pose CurrentPose() const noexcept;
	glm::mat4 ViewMatrix() const;
	glm::mat4 Projection(float aspectRatio, utils::Position3d stationCentre) const;
	glm::mat4 SkyboxProjection(float aspectRatio) const;
	utils::Position3d FocusPointAtCursor(utils::Position2d cursorPosition,
		unsigned int viewportWidth,
		unsigned int viewportHeight,
		utils::Position3d stationCentre) const;
		float StationInteriorFieldOfView() const noexcept;
		bool HasRememberedPose(View view) const noexcept;
		Pose RememberedPose(View view) const noexcept;
		void SetViewTarget(View view, Pose target) noexcept;
		void SetEditorPose(Pose pose) noexcept;
		EditorReturnState CaptureEditorReturnState() const noexcept;
		void RestoreEditorReturnState(const EditorReturnState& state) noexcept;
		void SetEditorPerspective(bool enabled) noexcept { _editorPerspective = enabled; }
		void ForgetStation(const std::shared_ptr<const void>& identity) noexcept;
		void RevealPosition(utils::Position3d position, utils::Size2d viewport, utils::Position3d centre, int margin);
		void RegisterStation(size_t index, std::shared_ptr<const void> identity, std::uint64_t revision);
		void ObserveStation(size_t index, std::shared_ptr<const void> identity, std::uint64_t revision, utils::Position3d position);
		void CompleteStationObservation(size_t stationCount, std::optional<utils::Position3d> firstStation);
		SelectDepthChange CycleView(utils::Position3d stationCentre,
			std::optional<utils::Position3d> hoveredStation,
			std::optional<utils::Position3d> firstStation,
			std::shared_ptr<const void> hoveredIdentity,
			std::shared_ptr<const void> firstIdentity,
			bool stationSelectDepth);
		bool SelectDepthChanged(bool stationSelected) noexcept;
		SelectDepthChange PendingSelectDepthChange() noexcept;

	private:
		friend class CameraPresentationTestAccess;
		static constexpr unsigned int BackgroundDragLeftButtonMask    = 1u << 0;
		static constexpr unsigned int BackgroundDragRightButtonMask   = 1u << 2;
		static constexpr unsigned int BackgroundDragRelativeBlendFrames = 2u;
		static constexpr float TransitionDurationSeconds = 0.4f;
		static constexpr float WheelZoomTransitionDurationSeconds = 0.15f;
		static constexpr size_t ViewCount = 3u;

		enum class BackgroundDragMode
		{
			None,
			RelativePan,
			InertialPan
		};

		struct BackgroundDragState
		{
			BackgroundDragMode Mode = BackgroundDragMode::None;
			bool Dragged = false;
			unsigned int MouseButtonsDown = 0u;
			unsigned int RelativeBlendFrames = 0u;
			utils::Position2d PointerAnchor{};
			utils::Position2d LastPointerPosition{};
			utils::Position3d CameraAnchor{};
			utils::Position3d CameraPosition{};
			utils::Position3d Velocity{};
		};

		actions::ActionResult _BackgroundDragActionResult() const;
		unsigned int _BackgroundDragMouseButtonsDown(unsigned int mouseButtonsDown, int index, bool isTouchDown) const noexcept;
		BackgroundDragMode _ResolveBackgroundDragMode(unsigned int mouseButtonsDown) const noexcept;
		utils::Position3d _ClampBackgroundDragVelocity(utils::Position3d velocity) const noexcept;
		utils::Position3d _RelativeBackgroundDragPosition(utils::Position2d pointerPosition) const noexcept;
		utils::Position3d _ApplyBackgroundDragBlend(utils::Position3d targetPosition) noexcept;
		utils::Position3d _UpdateInertialBackgroundDrag(utils::Position2d pointerDelta) noexcept;
		void _ApplyBackgroundDragPosition(utils::Position3d position) noexcept;
		void _SwitchBackgroundDragMode(utils::Position2d pointerPosition, unsigned int mouseButtonsDown) noexcept;
		void _EndBackgroundDrag() noexcept;
		void _CoastBackgroundDrag(float deltaSeconds);
		static utils::Position3d _Normalise(utils::Position3d value) noexcept;
		static utils::Position3d _Lerp(utils::Position3d from, utils::Position3d to, float amount) noexcept;
		void _TickTransition(float deltaSeconds) noexcept;
		void _ApplyPose(Pose pose) noexcept;
		utils::Position3d _ConstrainDragPosition(utils::Position3d position) const noexcept;
		static size_t _ViewIndex(View view) noexcept;
		Pose _PoseForView(View view, utils::Position3d stationCentre,
			std::optional<utils::Position3d> hoveredStation,
			std::optional<utils::Position3d> firstStation) const noexcept;
		SelectDepthChange _LeaveStationInteriorSelectDepth() noexcept;
		unsigned int _id;
		BackgroundDragState _backgroundDrag;
		View _view;
		Pose _pose;
		float _stationInteriorFieldOfView;
		Pose _transitionStart;
		Pose _transitionTarget;
		float _transitionElapsedSeconds;
		bool _transitioning;
		bool _wheelZoomTransition;
		utils::Position3d _wheelZoomFocusPoint;
		utils::Position3d _wheelZoomStationCentre;
		Pose _rememberedPoses[ViewCount];
		bool _hasRememberedPose[ViewCount];
		struct ObservedStation
		{
			// Keep the station alive until the next observation detects removal.
			std::shared_ptr<const void> Identity;
			std::uint64_t Revision = 0u;
		};
		std::vector<ObservedStation> _observedStations;
		std::shared_ptr<const void> _lastChangedStationIdentity;
		std::shared_ptr<const void> _interiorFocusIdentity;
		std::optional<utils::Position3d> _lastChangedStationPosition;
		bool _interiorForcedLoopTakeDepth = false;
		bool _interiorSelectDepthChanged = false;
		bool _interiorRestorePending = false;
		bool _editorPerspective = false;
		// Window owner only. Targets never overwrite remembered manual view poses.
		std::optional<utils::Position3d> _automaticTarget;
		std::optional<Pose> _automaticManualPose;
		utils::Position3d _automaticVelocity{};
		bool _resumeAutomaticVelocity = false;
		CameraBounds _frontVisibleBounds;
		UnattendedCameraController _unattendedCamera;
		bool _windowEligible = true;
		bool _heldInteraction = false;
		std::optional<Time> _lastPresentationTime;
		std::optional<Time> _lastDiagnosticTime;
		std::uint64_t _humanActivityRevision = 0u;
	};
}
