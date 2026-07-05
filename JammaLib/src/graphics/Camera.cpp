#include "Camera.h"
#include <algorithm>
#include <cmath>
#include <iostream>

using namespace actions;
using namespace base;
using namespace graphics;
using namespace utils;

Camera::Camera(CameraParams params) :
	Moveable(params),
	_id(params.Id),
	_backgroundDrag()
{
}

ActionResult Camera::_BackgroundDragActionResult() const
{
	ActionResult res;
	res.IsEaten = true;
	res.SourceId = "";
	res.TargetId = "";
	res.ResultType = ACTIONRESULT_DEFAULT;
	res.Undo = std::shared_ptr<ActionUndo>();
	res.ActiveElement = std::weak_ptr<GuiElement>();
	return res;
}

unsigned int Camera::_BackgroundDragMouseButtonsDown(unsigned int mouseButtonsDown, int index, bool isTouchDown) const noexcept
{
	if (0u != mouseButtonsDown)
		return mouseButtonsDown;

	if (!isTouchDown)
		return 0u;

	if (index < 0 || index >= 32 || 4 == index)
		return 0u;

	return 1u << index;
}

Camera::BackgroundDragMode Camera::_ResolveBackgroundDragMode(unsigned int mouseButtonsDown) const noexcept
{
	if (0u != (mouseButtonsDown & BackgroundDragRightButtonMask))
		return BackgroundDragMode::InertialPan;

	if (0u != mouseButtonsDown)
		return BackgroundDragMode::RelativePan;

	return BackgroundDragMode::None;
}

utils::Position3d Camera::_ClampBackgroundDragVelocity(utils::Position3d velocity) const noexcept
{
	constexpr float maxSpeed = 40.0f;
	velocity.X = std::clamp(velocity.X, -maxSpeed, maxSpeed);
	velocity.Y = std::clamp(velocity.Y, -maxSpeed, maxSpeed);
	velocity.Z = 0.0f;
	return velocity;
}

utils::Position3d Camera::_RelativeBackgroundDragPosition(utils::Position2d pointerPosition) const noexcept
{
	auto dPos = pointerPosition - _backgroundDrag.PointerAnchor;
	return _backgroundDrag.CameraAnchor - Position3d{ (float)dPos.X, (float)dPos.Y, 0.0f };
}

utils::Position3d Camera::_ApplyBackgroundDragBlend(utils::Position3d targetPosition) noexcept
{
	if (0u == _backgroundDrag.RelativeBlendFrames)
		return targetPosition;

	constexpr float blend = 0.45f;
	auto current = _backgroundDrag.CameraPosition;
	_backgroundDrag.RelativeBlendFrames--;
	return {
		current.X + ((targetPosition.X - current.X) * blend),
		current.Y + ((targetPosition.Y - current.Y) * blend),
		targetPosition.Z
	};
}

utils::Position3d Camera::_UpdateInertialBackgroundDrag(utils::Position2d pointerDelta) noexcept
{
	constexpr float gain = 0.18f;
	constexpr float carry = 0.74f;
	constexpr float linearDragCoeff = 0.01f;
	constexpr float quadraticDragCoeff = 0.1f;
	constexpr float inputDragStep = 1.0f;
	auto inputVelocity = Position3d{ -(float)pointerDelta.X * gain, -(float)pointerDelta.Y * gain, 0.0f };
	_backgroundDrag.Velocity = _ClampBackgroundDragVelocity({
		(_backgroundDrag.Velocity.X * carry) + inputVelocity.X,
		(_backgroundDrag.Velocity.Y * carry) + inputVelocity.Y,
		0.0f
	});

	auto applyDrag = [](float velocity, float step, float linearCoeff, float quadraticCoeff)
	{
		if (0.0f == velocity)
			return 0.0f;

		return velocity / (1.0f + ((linearCoeff + (quadraticCoeff * std::fabs(velocity))) * step));
	};

	_backgroundDrag.Velocity = _ClampBackgroundDragVelocity({
		applyDrag(_backgroundDrag.Velocity.X, inputDragStep, linearDragCoeff, quadraticDragCoeff),
		applyDrag(_backgroundDrag.Velocity.Y, inputDragStep, linearDragCoeff, quadraticDragCoeff),
		0.0f
	});
	return _backgroundDrag.CameraPosition + _backgroundDrag.Velocity;
}

void Camera::_ApplyBackgroundDragPosition(utils::Position3d position) noexcept
{
	_backgroundDrag.CameraPosition = position;
	SetModelPosition(position);
}

void Camera::_SwitchBackgroundDragMode(utils::Position2d pointerPosition, unsigned int mouseButtonsDown) noexcept
{
	auto nextMode = _ResolveBackgroundDragMode(mouseButtonsDown);
	if (_backgroundDrag.Mode == nextMode)
	{
		_backgroundDrag.MouseButtonsDown = mouseButtonsDown;
		_backgroundDrag.LastPointerPosition = pointerPosition;
		if (BackgroundDragMode::InertialPan == nextMode)
		{
			std::cout << "Background drag inertial mode active at "
				<< pointerPosition.X << ", " << pointerPosition.Y
				<< " buttons=" << mouseButtonsDown << std::endl;
		}
		return;
	}

	auto previousMode = _backgroundDrag.Mode;
	_backgroundDrag.Mode = nextMode;
	_backgroundDrag.MouseButtonsDown = mouseButtonsDown;
	_backgroundDrag.PointerAnchor = pointerPosition;
	_backgroundDrag.LastPointerPosition = pointerPosition;
	_backgroundDrag.CameraAnchor = _backgroundDrag.CameraPosition;
	_backgroundDrag.RelativeBlendFrames = ((BackgroundDragMode::InertialPan == previousMode)
		&& (BackgroundDragMode::RelativePan == nextMode))
		? BackgroundDragRelativeBlendFrames
		: 0u;

	if (BackgroundDragMode::InertialPan == nextMode)
	{
		std::cout << "Background drag inertial mode active at "
			<< pointerPosition.X << ", " << pointerPosition.Y
			<< " buttons=" << mouseButtonsDown << std::endl;
	}
}

void Camera::_EndBackgroundDrag() noexcept
{
	_backgroundDrag = BackgroundDragState{};
}

void Camera::_CoastBackgroundDrag(unsigned int samps, unsigned int sampleRate)
{
	if (0u == samps || 0u == sampleRate)
		return;

	constexpr float framesPerSecond = 60.0f;
	constexpr float linearDragCoeff = 0.01f;
	constexpr float quadraticDragCoeff = 0.1f;
	const float deltaSeconds = static_cast<float>(samps) / static_cast<float>(sampleRate);
	const float motionScale = deltaSeconds * framesPerSecond;
	if (motionScale <= 0.0f)
		return;

	auto velocity = _backgroundDrag.Velocity;
	if ((0.0f == velocity.X) && (0.0f == velocity.Y))
		return;

	auto nextPosition = _backgroundDrag.CameraPosition + Position3d{
		velocity.X * motionScale,
		velocity.Y * motionScale,
		0.0f
	};

	auto applyDrag = [](float velocity, float step, float linearCoeff, float quadraticCoeff)
	{
		if (0.0f == velocity)
			return 0.0f;

		return velocity / (1.0f + ((linearCoeff + (quadraticCoeff * std::fabs(velocity))) * step));
	};

	_backgroundDrag.Velocity = _ClampBackgroundDragVelocity({
		applyDrag(velocity.X, motionScale, linearDragCoeff, quadraticDragCoeff),
		applyDrag(velocity.Y, motionScale, linearDragCoeff, quadraticDragCoeff),
		0.0f
	});

	if ((std::fabs(_backgroundDrag.Velocity.X) < 0.02f)
		&& (std::fabs(_backgroundDrag.Velocity.Y) < 0.02f))
	{
		_backgroundDrag.Velocity = { 0.0f, 0.0f, 0.0f };
	}

	_ApplyBackgroundDragPosition(nextPosition);

	if ((0u == _backgroundDrag.MouseButtonsDown) && (0.0f == _backgroundDrag.Velocity.X) && (0.0f == _backgroundDrag.Velocity.Y))
		_EndBackgroundDrag();
}

ActionResult Camera::HandleBackgroundDrag(TouchAction action)
{
	auto mouseButtonsDown = _BackgroundDragMouseButtonsDown(action.MouseButtonsDown, action.Index, TouchAction::TOUCH_DOWN == action.State);
	auto mode = _ResolveBackgroundDragMode(mouseButtonsDown);

	if (TouchAction::TOUCH_DOWN == action.State)
	{
		if (BackgroundDragMode::None == mode)
			return ActionResult::NoAction();

		if (BackgroundDragMode::None == _backgroundDrag.Mode)
		{
			_backgroundDrag = BackgroundDragState{};
			_backgroundDrag.CameraPosition = ModelPosition();
			_SwitchBackgroundDragMode(action.Position, mouseButtonsDown);
			return _BackgroundDragActionResult();
		}

		if ((BackgroundDragMode::InertialPan == _backgroundDrag.Mode)
			&& (BackgroundDragMode::InertialPan == mode))
		{
			_backgroundDrag.MouseButtonsDown = mouseButtonsDown;
			_backgroundDrag.PointerAnchor = action.Position;
			_backgroundDrag.LastPointerPosition = action.Position;
			_backgroundDrag.CameraAnchor = _backgroundDrag.CameraPosition;
			_backgroundDrag.Velocity = { 0.0f, 0.0f, 0.0f };
			return _BackgroundDragActionResult();
		}

		_SwitchBackgroundDragMode(action.Position, mouseButtonsDown);
		return _BackgroundDragActionResult();
	}

	if (TouchAction::TOUCH_UP == action.State)
	{
		if (BackgroundDragMode::None == _backgroundDrag.Mode)
			return ActionResult::NoAction();

		auto wasDragged = _backgroundDrag.Dragged;
		const auto releasedButton = static_cast<unsigned int>(action.Value);

		if (BackgroundDragMode::None == mode)
		{
			if ((BackgroundDragMode::InertialPan == _backgroundDrag.Mode)
				&& (0u != (releasedButton & BackgroundDragRightButtonMask))
				&& ((0.0f != _backgroundDrag.Velocity.X) || (0.0f != _backgroundDrag.Velocity.Y)))
			{
				_backgroundDrag.MouseButtonsDown = 0u;
				return _BackgroundDragActionResult();
			}

			_EndBackgroundDrag();
			return _BackgroundDragActionResult();
		}

		_SwitchBackgroundDragMode(action.Position, mouseButtonsDown);
		if ((BackgroundDragMode::None == _ResolveBackgroundDragMode(mouseButtonsDown))
			&& !wasDragged)
		{
			_EndBackgroundDrag();
		}

		return _BackgroundDragActionResult();
	}

	return ActionResult::NoAction();
}

ActionResult Camera::UpdateBackgroundDrag(TouchMoveAction action)
{
	if (BackgroundDragMode::None == _backgroundDrag.Mode)
		return ActionResult::NoAction();

	auto mouseButtonsDown = _BackgroundDragMouseButtonsDown(action.MouseButtonsDown, action.Index, false);
	if (BackgroundDragMode::None == _ResolveBackgroundDragMode(mouseButtonsDown))
	{
		_EndBackgroundDrag();
		return ActionResult::NoAction();
	}

	auto previousPointerPosition = _backgroundDrag.LastPointerPosition;
	if (_backgroundDrag.Mode != _ResolveBackgroundDragMode(mouseButtonsDown))
		_SwitchBackgroundDragMode(action.Position, mouseButtonsDown);

	auto pointerDelta = action.Position - previousPointerPosition;
	_backgroundDrag.LastPointerPosition = action.Position;

	auto previousCameraPosition = _backgroundDrag.CameraPosition;
	auto nextCameraPosition = previousCameraPosition;
	if (BackgroundDragMode::RelativePan == _backgroundDrag.Mode)
	{
		nextCameraPosition = _ApplyBackgroundDragBlend(_RelativeBackgroundDragPosition(action.Position));
		_backgroundDrag.Velocity = {
			nextCameraPosition.X - previousCameraPosition.X,
			nextCameraPosition.Y - previousCameraPosition.Y,
			0.0f
		};
	}
	else if (BackgroundDragMode::InertialPan == _backgroundDrag.Mode)
	{
		nextCameraPosition = _UpdateInertialBackgroundDrag(pointerDelta);
	}

	if ((nextCameraPosition.X != previousCameraPosition.X)
		|| (nextCameraPosition.Y != previousCameraPosition.Y)
		|| (nextCameraPosition.Z != previousCameraPosition.Z))
	{
		_backgroundDrag.Dragged = true;
	}

	_ApplyBackgroundDragPosition(nextCameraPosition);
	return ActionResult::NoAction();
}

void Camera::TickBackgroundDrag(unsigned int samps, unsigned int sampleRate)
{
	if (BackgroundDragMode::InertialPan != _backgroundDrag.Mode)
		return;

	_CoastBackgroundDrag(samps, sampleRate);
}

bool Camera::IsBackgroundDragging() const noexcept
{
	return BackgroundDragMode::None != _backgroundDrag.Mode;
}

bool Camera::BackgroundDragWasDragged() const noexcept
{
	return _backgroundDrag.Dragged;
}
