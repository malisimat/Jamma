#pragma once

#include "Moveable.h"
#include "../actions/ActionResult.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"
#include <glm/glm.hpp>

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
		static constexpr float WheelZoomStep = 250.0f;

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

		Camera(CameraParams camParams);

		actions::ActionResult HandleBackgroundDrag(actions::TouchAction action);
		actions::ActionResult UpdateBackgroundDrag(actions::TouchMoveAction action);
		actions::ActionResult HandleWheel(int wheelNotches);
		actions::ActionResult HandleWheel(int wheelNotches,
			utils::Position2d cursorPosition,
			unsigned int viewportWidth,
			unsigned int viewportHeight,
			utils::Position3d stationCentre);
		void TickBackgroundDrag(unsigned int samps, unsigned int sampleRate);
		bool IsBackgroundDragging() const noexcept;
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

	private:
		static constexpr unsigned int BackgroundDragLeftButtonMask    = 1u << 0;
		static constexpr unsigned int BackgroundDragRightButtonMask   = 1u << 2;
		static constexpr unsigned int BackgroundDragRelativeBlendFrames = 2u;
		static constexpr float TransitionDurationSeconds = 0.4f;
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
		void _CoastBackgroundDrag(unsigned int samps, unsigned int sampleRate);
		static utils::Position3d _Normalise(utils::Position3d value) noexcept;
		static utils::Position3d _Lerp(utils::Position3d from, utils::Position3d to, float amount) noexcept;
		void _TickTransition(unsigned int samps, unsigned int sampleRate) noexcept;
		void _ApplyPose(Pose pose) noexcept;
		utils::Position3d _ConstrainDragPosition(utils::Position3d position) const noexcept;
		static size_t _ViewIndex(View view) noexcept;
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
	};
}
