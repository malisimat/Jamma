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
		enum class State { Waiting, Overview, RecordingFocus, QuietHold };
		struct Decision
		{
			State CurrentState = State::Waiting;
			engine::CameraRecordingIdentity Focus;
			bool IsAutomatic() const noexcept { return CurrentState != State::Waiting; }
		};
		static constexpr double IdleDelaySeconds = 20.0;
		static constexpr double QuietDelaySeconds = 5.0;

		void HumanActivity(Time now) noexcept
		{
			_idleSince = now;
			_decision = {};
		}

		Decision Update(Time now, bool eligible,
			const engine::CameraRecordingObservation& observation,
			std::span<const std::weak_ptr<engine::LoopTake>> liveTakes) noexcept
		{
			if (_sceneGeneration != observation.SceneGeneration)
			{
				_sceneGeneration = observation.SceneGeneration;
				_seenStartOrder = observation.LatestStart.StartOrder;
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
				if (!valid(capture.Identity)) continue;
				hasCapture = true;
				if (capture.Identity.StartOrder > newestActive.StartOrder)
					newestActive = capture.Identity;
			}
			const bool newStart = observation.LatestStart.StartOrder > _seenStartOrder;
			_seenStartOrder = observation.LatestStart.StartOrder;
			if (!eligible)
			{
				HumanActivity(now);
				_wasEligible = false;
				return _decision;
			}
			if (!_wasEligible || !_idleSince)
			{
				HumanActivity(now);
				_wasEligible = true;
			}
			const bool entering = !_decision.IsAutomatic();
			if (entering && utils::Timer::GetElapsedSeconds(*_idleSince, now) < IdleDelaySeconds)
				return _decision;
			if (entering)
			{
				// A start after the real idle deadline may already have ended during a
				// frame stall. Completed starts from the manual period are not replayed.
				const bool unattendedStart = newStart && valid(observation.LatestStart)
					&& utils::Timer::GetElapsedSeconds(*_idleSince, observation.LatestStartAt) >= IdleDelaySeconds;
				_decision = { State::Overview, unattendedStart ? observation.LatestStart : newestActive };
			}
			else if (newStart && valid(observation.LatestStart))
				_decision.Focus = observation.LatestStart;
			if (!valid(_decision.Focus)) _decision.Focus = newestActive;
			if (hasCapture)
				_decision.CurrentState = valid(_decision.Focus) ? State::RecordingFocus : State::Overview;
			else if (valid(_decision.Focus) &&
				utils::Timer::GetElapsedSeconds(observation.LastLifecycleChange, now) < QuietDelaySeconds)
				_decision.CurrentState = State::QuietHold;
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
		Decision _decision;
	};
}
