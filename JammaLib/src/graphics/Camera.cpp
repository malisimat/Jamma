#include "Camera.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <glm/gtc/quaternion.hpp>

using namespace actions;
using namespace base;
using namespace graphics;
using namespace utils;

Camera::Camera(CameraParams params) :
	Moveable(params),
	_id(params.Id),
	_backgroundDrag(),
	_view(View::Front),
	_pose({ params.ModelPosition, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f } }),
	_stationInteriorFieldOfView(80.0f),
	_transitionStart(_pose),
	_transitionTarget(_pose),
	_transitionElapsedSeconds(0.0f),
	_transitioning(false),
	_rememberedPoses{},
	_hasRememberedPose{}
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
	velocity.Z = std::clamp(velocity.Z, -maxSpeed, maxSpeed);
	if (View::StationInterior == _view)
	{
		velocity.X = 0.0f;
		velocity.Z = 0.0f;
	}
	else if (View::TopDown == _view)
		velocity.Y = 0.0f;
	else
		velocity.Z = 0.0f;
	return velocity;
}

utils::Position3d Camera::_Normalise(utils::Position3d value) noexcept
{
	const auto length = std::sqrt((value.X * value.X) + (value.Y * value.Y) + (value.Z * value.Z));
	if (length <= 0.0001f)
		return { 0.0f, 0.0f, -1.0f };

	return { value.X / length, value.Y / length, value.Z / length };
}

utils::Position3d Camera::_Lerp(utils::Position3d from, utils::Position3d to, float amount) noexcept
{
	return {
		from.X + ((to.X - from.X) * amount),
		from.Y + ((to.Y - from.Y) * amount),
		from.Z + ((to.Z - from.Z) * amount)
	};
}

utils::Position3d Camera::_RelativeBackgroundDragPosition(utils::Position2d pointerPosition) const noexcept
{
	auto dPos = pointerPosition - _backgroundDrag.PointerAnchor;
	switch (_view)
	{
	case View::StationInterior:
		return _backgroundDrag.CameraAnchor - Position3d{ 0.0f, (float)dPos.Y, 0.0f };
	case View::TopDown:
		return _backgroundDrag.CameraAnchor + Position3d{ -(float)dPos.X, 0.0f, (float)dPos.Y };
	case View::Front:
		return _backgroundDrag.CameraAnchor - Position3d{ (float)dPos.X, (float)dPos.Y, 0.0f };
	}
	return _backgroundDrag.CameraAnchor;
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
	Position3d inputVelocity{};
	switch (_view)
	{
	case View::StationInterior:
		inputVelocity = { 0.0f, -(float)pointerDelta.Y * gain, 0.0f };
		break;
	case View::TopDown:
		inputVelocity = { -(float)pointerDelta.X * gain, 0.0f, (float)pointerDelta.Y * gain };
		break;
	case View::Front:
		inputVelocity = { -(float)pointerDelta.X * gain, -(float)pointerDelta.Y * gain, 0.0f };
		break;
	}
	_backgroundDrag.Velocity = _ClampBackgroundDragVelocity({
		(_backgroundDrag.Velocity.X * carry) + inputVelocity.X,
		(_backgroundDrag.Velocity.Y * carry) + inputVelocity.Y,
		(_backgroundDrag.Velocity.Z * carry) + inputVelocity.Z
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
		applyDrag(_backgroundDrag.Velocity.Z, inputDragStep, linearDragCoeff, quadraticDragCoeff)
	});
	return _backgroundDrag.CameraPosition + _backgroundDrag.Velocity;
}

void Camera::_ApplyBackgroundDragPosition(utils::Position3d position) noexcept
{
	position = _ConstrainDragPosition(position);
	_backgroundDrag.CameraPosition = position;
	_pose.Eye = position;
	_transitionTarget.Eye = position;
	SetModelPosition(_pose.Eye);
}

utils::Position3d Camera::_ConstrainDragPosition(utils::Position3d position) const noexcept
{
	switch (_view)
	{
	case View::StationInterior:
		position.X = _transitionTarget.Eye.X;
		position.Z = _transitionTarget.Eye.Z;
		break;
	case View::TopDown:
		position.Y = _transitionTarget.Eye.Y;
		break;
	case View::Front:
		break;
	}
	return position;
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
	if ((0.0f == velocity.X) && (0.0f == velocity.Y) && (0.0f == velocity.Z))
		return;

	auto nextPosition = _backgroundDrag.CameraPosition + Position3d{
		velocity.X * motionScale,
		velocity.Y * motionScale,
		velocity.Z * motionScale
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
		applyDrag(velocity.Z, motionScale, linearDragCoeff, quadraticDragCoeff)
	});

	if ((std::fabs(_backgroundDrag.Velocity.X) < 0.02f)
		&& (std::fabs(_backgroundDrag.Velocity.Y) < 0.02f)
		&& (std::fabs(_backgroundDrag.Velocity.Z) < 0.02f))
	{
		_backgroundDrag.Velocity = { 0.0f, 0.0f, 0.0f };
	}

	_ApplyBackgroundDragPosition(nextPosition);

	if ((0u == _backgroundDrag.MouseButtonsDown) && (0.0f == _backgroundDrag.Velocity.X) && (0.0f == _backgroundDrag.Velocity.Y) && (0.0f == _backgroundDrag.Velocity.Z))
		_EndBackgroundDrag();
}

ActionResult Camera::HandleBackgroundDrag(TouchAction action)
{
	if (_transitioning)
		return _BackgroundDragActionResult();

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
				&& ((0.0f != _backgroundDrag.Velocity.X) || (0.0f != _backgroundDrag.Velocity.Y) || (0.0f != _backgroundDrag.Velocity.Z)))
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
	if (_transitioning)
		return _BackgroundDragActionResult();

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

ActionResult Camera::HandleWheel(int wheelNotches)
{
	if (0 == wheelNotches)
		return ActionResult::NoAction();

	if (View::StationInterior == _view)
	{
		constexpr float fieldOfViewStep = 8.0f;
		constexpr float minFieldOfView = 20.0f;
		constexpr float maxFieldOfView = 120.0f;
		_stationInteriorFieldOfView = std::clamp(_stationInteriorFieldOfView
			- (static_cast<float>(wheelNotches) * fieldOfViewStep), minFieldOfView, maxFieldOfView);
		return _BackgroundDragActionResult();
	}

	if (_transitioning)
		return _BackgroundDragActionResult();

	constexpr float wheelStep = 144.0f;
	constexpr float frontMinZ = 80.0f;
	constexpr float frontMaxZ = 900.0f;
	constexpr float topDownMinY = 180.0f;
	constexpr float topDownMaxY = 1200.0f;
	auto target = _pose;
	if (View::Front == _view)
		target.Eye.Z = std::clamp(target.Eye.Z - (static_cast<float>(wheelNotches) * wheelStep), frontMinZ, frontMaxZ);
	else if (View::TopDown == _view)
		target.Eye.Y = std::clamp(target.Eye.Y - (static_cast<float>(wheelNotches) * wheelStep), topDownMinY, topDownMaxY);

	SetViewTarget(_view, target);
	return _BackgroundDragActionResult();
}

void Camera::_ApplyPose(Pose pose) noexcept
{
	pose.Forward = _Normalise(pose.Forward);
	pose.Up = _Normalise(pose.Up);
	_pose = pose;
	SetModelPosition(_pose.Eye);
}

void Camera::SetViewTarget(View view, Pose target) noexcept
{
	if (_view != view)
	{
		// An interrupted transition has a valid target pose but a transient current pose.
		_rememberedPoses[_ViewIndex(_view)] = _transitioning ? _transitionTarget : _pose;
		_hasRememberedPose[_ViewIndex(_view)] = true;
	}
	target.Forward = _Normalise(target.Forward);
	target.Up = _Normalise(target.Up);
	_view = view;
	_transitionStart = _pose;
	_transitionTarget = target;
	_transitionElapsedSeconds = 0.0f;
	_transitioning = true;
	_EndBackgroundDrag();
}

void Camera::_TickTransition(unsigned int samps, unsigned int sampleRate) noexcept
{
	if (!_transitioning || (0u == samps) || (0u == sampleRate))
		return;

	const auto deltaSeconds = static_cast<float>(samps) / static_cast<float>(sampleRate);
	_transitionElapsedSeconds = std::min(_transitionElapsedSeconds + std::min(deltaSeconds, 0.05f), TransitionDurationSeconds);
	const auto linearProgress = _transitionElapsedSeconds / TransitionDurationSeconds;
	const auto easedProgress = linearProgress * linearProgress * (3.0f - (2.0f * linearProgress));
	Pose pose;
	pose.Eye = _Lerp(_transitionStart.Eye, _transitionTarget.Eye, easedProgress);
	const auto startOrientation = glm::quatLookAt(
		glm::vec3(_transitionStart.Forward.X, _transitionStart.Forward.Y, _transitionStart.Forward.Z),
		glm::vec3(_transitionStart.Up.X, _transitionStart.Up.Y, _transitionStart.Up.Z));
	const auto targetOrientation = glm::quatLookAt(
		glm::vec3(_transitionTarget.Forward.X, _transitionTarget.Forward.Y, _transitionTarget.Forward.Z),
		glm::vec3(_transitionTarget.Up.X, _transitionTarget.Up.Y, _transitionTarget.Up.Z));
	const auto rotation = glm::mat3_cast(glm::slerp(startOrientation, targetOrientation, easedProgress));
	const auto forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
	const auto up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
	pose.Forward = { forward.x, forward.y, forward.z };
	pose.Up = { up.x, up.y, up.z };
	_ApplyPose(pose);

	if (_transitionElapsedSeconds >= TransitionDurationSeconds)
	{
		_ApplyPose(_transitionTarget);
		_transitioning = false;
	}
}

void Camera::TickBackgroundDrag(unsigned int samps, unsigned int sampleRate)
{
	if (_transitioning)
	{
		_TickTransition(samps, sampleRate);
		return;
	}

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

bool Camera::IsTransitioning() const noexcept
{
	return _transitioning;
}

Camera::View Camera::CurrentView() const noexcept
{
	return _view;
}

Camera::Pose Camera::CurrentPose() const noexcept
{
	return _pose;
}

float Camera::StationInteriorFieldOfView() const noexcept
{
	return _stationInteriorFieldOfView;
}

size_t Camera::_ViewIndex(View view) noexcept
{
	return static_cast<size_t>(view);
}

bool Camera::HasRememberedPose(View view) const noexcept
{
	return _hasRememberedPose[_ViewIndex(view)];
}

Camera::Pose Camera::RememberedPose(View view) const noexcept
{
	return _rememberedPoses[_ViewIndex(view)];
}
