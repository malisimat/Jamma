#pragma once

#include <optional>
#include <span>
#include "../engine/CameraRecordingObservation.h"

namespace graphics
{
	// Window-thread-only policy. Time is Timer's monotonic clock, never a sum of
	// capped physics steps. Weak identities are validated against live membership
	// on every update, including while manual/excluded. No engine commands here.
	class UnattendedCameraController
	{
	public:
		enum class State { Waiting, Overview, RecordingFocus, StationFocus };
		struct Decision
		{
			State CurrentState = State::Waiting;
			engine::CameraRecordingIdentity Focus;
			std::weak_ptr<engine::Station> Station;
			bool IsAutomatic() const noexcept { return CurrentState != State::Waiting; }
		};
		static constexpr double IdleDelaySeconds = 8.0;
		static constexpr double StationFocusSeconds = 2.0;
		bool IsAutomatic() const noexcept { return _decision.IsAutomatic(); }
		double IdleSeconds(Time now) const noexcept
		{
			return _idleSince ? utils::Timer::GetElapsedSeconds(*_idleSince, now) : 0.0;
		}

		void HumanActivity(Time now) noexcept
		{
			_idleSince = now;
			_decision = {};
		}

		void DiscardRecordingStartsThrough(std::uint64_t startOrder, std::uint64_t ditchOrder) noexcept
		{
			_seenStartOrder = (std::max)(_seenStartOrder, startOrder);
			_seenDitchOrder = (std::max)(_seenDitchOrder, ditchOrder);
		}

		Decision Update(Time now, bool eligible, bool heldInteractionInEligibleView,
			const engine::CameraRecordingObservation& observation,
			std::span<const std::weak_ptr<engine::LoopTake>> liveTakes) noexcept
		{
			if (_sceneGeneration != observation.SceneGeneration)
			{
				_sceneGeneration = observation.SceneGeneration;
				_seenStartOrder = observation.LatestStart.StartOrder;
				_seenDitchOrder = 0u;
				HumanActivity(now);
				_wasEligible = false;
			}
			const auto valid = [&liveTakes](const engine::CameraRecordingIdentity& identity)
			{
				return !identity.Take.expired() && std::any_of(liveTakes.begin(), liveTakes.end(),
					[&identity](const auto& take) {
						return engine::CameraRecordingPublisher::SameTake(take, identity.Take);
					});
			};
			engine::CameraRecordingIdentity newestActive;
			bool hasCapture = false;
			for (const auto& capture : observation.ActiveCaptures)
			{
				if (!valid(capture.Identity) || capture.CaptureMode == engine::CameraRecordingObservation::Mode::Tail) continue;
				hasCapture = true;
				if (capture.Identity.StartOrder > newestActive.StartOrder)
					newestActive = capture.Identity;
			}
			const bool newStart = observation.LatestStart.StartOrder > _seenStartOrder;
			if (!eligible)
			{
				// Keep a new start through the held input that may have initiated it,
				// but discard starts observed in hidden/excluded camera modes.
				if (!heldInteractionInEligibleView)
				{
					_seenStartOrder = observation.LatestStart.StartOrder;
					_seenDitchOrder = observation.LatestDitchOrder;
				}
				if (!heldInteractionInEligibleView)
					HumanActivity(now);
				else if (!_idleSince)
					HumanActivity(now);
				// Held input pauses motion, but only an explicit HumanActivity
				// notification restarts idle timing, so selection keeps its deadline.
				if (!heldInteractionInEligibleView) _wasEligible = false;
				return _decision;
			}
			_seenStartOrder = observation.LatestStart.StartOrder;
			const bool newDitch = observation.LatestDitchOrder > _seenDitchOrder;
			_seenDitchOrder = observation.LatestDitchOrder;
			if (!_wasEligible || !_idleSince)
			{
				HumanActivity(now);
				_wasEligible = true;
			}
			const bool newRecordingStart = newStart && valid(observation.LatestStart);
			if (newDitch && observation.LatestDitchOrder > observation.LatestStart.StartOrder
				&& !observation.LatestDitchStation.expired())
				_decision = { State::StationFocus, {}, observation.LatestDitchStation };
			// Starts retarget an existing wander immediately, but cannot bypass
			// the idle deadline while manual control is still active.
			else if (newRecordingStart && _decision.IsAutomatic())
				_decision = { State::RecordingFocus, observation.LatestStart, {} };
			else if (!_decision.IsAutomatic())
			{
				if (utils::Timer::GetElapsedSeconds(*_idleSince, now) < IdleDelaySeconds)
					return _decision;
				_decision = { State::Overview, newestActive, {} };
			}
			if (_decision.CurrentState == State::StationFocus && !_decision.Station.expired()
				&& utils::Timer::GetElapsedSeconds(observation.LatestDitchAt, now) < StationFocusSeconds)
				return _decision;
			_decision.Station.reset();
			// A stopped take no longer owns the close target, even if another
			// station is still capturing. Fall back to its newest active take.
			const bool focusActive = std::any_of(observation.ActiveCaptures.begin(), observation.ActiveCaptures.end(),
				[this](const auto& capture) {
					return capture.CaptureMode != engine::CameraRecordingObservation::Mode::Tail
						&& engine::CameraRecordingPublisher::SameTake(capture.Identity.Take, _decision.Focus.Take);
				});
			if (!valid(_decision.Focus) || !focusActive) _decision.Focus = newestActive;
			if (hasCapture)
				_decision.CurrentState = valid(_decision.Focus) ? State::RecordingFocus : State::Overview;
			else
			{
				_decision.CurrentState = State::Overview;
				_decision.Focus = {};
			}
			return _decision;
		}

	private:
		std::optional<Time> _idleSince;
		bool _wasEligible = false;
		std::uint64_t _sceneGeneration = 0u;
		std::uint64_t _seenStartOrder = 0u;
		std::uint64_t _seenDitchOrder = 0u;
		Decision _decision;
	};
}
