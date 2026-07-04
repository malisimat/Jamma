#pragma once

#include "Moveable.h"
#include "../actions/ActionResult.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"

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
		Camera(CameraParams camParams);

		actions::ActionResult HandleBackgroundDrag(actions::TouchAction action);
		actions::ActionResult UpdateBackgroundDrag(actions::TouchMoveAction action);
		void TickBackgroundDrag(unsigned int samps, unsigned int sampleRate);
		bool IsBackgroundDragging() const noexcept;
		bool BackgroundDragWasDragged() const noexcept;

	private:
		static constexpr unsigned int BackgroundDragLeftButtonMask    = 1u << 0;
		static constexpr unsigned int BackgroundDragRightButtonMask   = 1u << 2;
		static constexpr unsigned int BackgroundDragRelativeBlendFrames = 2u;

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
		unsigned int _id;
		BackgroundDragState _backgroundDrag;
	};
}
