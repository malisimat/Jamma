#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "../utils/Timer.h"

namespace engine
{
	class LoopTake;
	class Station;

	struct CameraRecordingIdentity
	{
		std::weak_ptr<LoopTake> Take;
		std::uint64_t StartOrder = 0u;
	};

	struct CameraRecordingObservation
	{
		enum class Mode { Inactive, Record, Overdub, Punch, Tail };
		struct Capture
		{
			CameraRecordingIdentity Identity;
			Mode CaptureMode = Mode::Inactive;
		};
		std::uint64_t SceneGeneration = 1u;
		std::uint64_t LifecycleRevision = 0u;
		Time LastLifecycleChange{};
		CameraRecordingIdentity LatestStart;
		Time LatestStartAt{};
		std::weak_ptr<Station> LatestDitchStation;
		std::uint64_t LatestDitchOrder = 0u;
		Time LatestDitchAt{};
		std::optional<Time> LastCaptureEndAt;
		std::vector<Capture> ActiveCaptures;
	};

	// Scene owns this publisher. All mutation is on job/UI structural owners,
	// serialized by Scene::_sceneMutex. The window reads immutable publications;
	// audio never calls these methods or reads this state. Stations/takes hold weak
	// publisher references, so retirement cannot retain the Scene or its camera.
	class CameraRecordingPublisher
	{
	public:
		using Mode = CameraRecordingObservation::Mode;

		std::shared_ptr<const CameraRecordingObservation> Snapshot() const noexcept
		{
			return _published.load(std::memory_order_acquire);
		}

		static bool SameTake(const std::weak_ptr<LoopTake>& a,
			const std::weak_ptr<LoopTake>& b) noexcept
		{
			return !a.owner_before(b) && !b.owner_before(a);
		}

		std::uint64_t Observe(const std::weak_ptr<LoopTake>& take, Mode mode, bool active,
			Time now, bool newLayer = false, std::uint64_t sceneGeneration = 0u,
			std::uint64_t startOrder = 0u, bool lifecycleChanged = false)
		{
			if (sceneGeneration != 0u && sceneGeneration != _state.SceneGeneration) return 0u;
			auto& captures = _state.ActiveCaptures;
			const auto removed = std::erase_if(captures, [](const auto& capture) { return capture.Identity.Take.expired(); });
			auto found = std::find_if(captures.begin(), captures.end(),
				[&take](const auto& capture) { return SameTake(capture.Identity.Take, take); });
			if (newLayer)
			{
				startOrder = ++_nextStartOrder;
				_state.LatestStart = { take, startOrder };
				_state.LatestStartAt = now;
				if (found != captures.end()) found->Identity = _state.LatestStart;
			}
			const bool changed = lifecycleChanged || removed != 0u || newLayer || (active
				? found == captures.end() || found->CaptureMode != mode
				: found != captures.end());
			if (!changed) return startOrder;
			if (!active && found != captures.end()) _state.LastCaptureEndAt = now;
			if (active)
			{
				if (found == captures.end())
					captures.push_back({ { take, startOrder }, mode });
				else
					found->CaptureMode = mode;
			}
			else if (found != captures.end())
				captures.erase(found);
			_state.LastLifecycleChange = now;
			++_state.LifecycleRevision;
			_Publish();
			return startOrder;
		}

		void ObserveDitch(const std::weak_ptr<Station>& station, Time now)
		{
			_state.LatestDitchStation = station;
			_state.LatestDitchOrder = ++_nextStartOrder;
			_state.LatestDitchAt = now;
			_Publish();
		}

		void Reset(Time now)
		{
			++_state.SceneGeneration;
			_state.LatestStart = {};
			// Empty-session timing reset can immediately follow the final ditch.
			// Keep that station gesture through the reset; it has no take identity.
			if (_state.LatestDitchStation.expired() ||
				utils::Timer::GetElapsedSeconds(_state.LatestDitchAt, now) >= 2.0)
			{
				_state.LatestDitchStation.reset();
				_state.LatestDitchOrder = 0u;
			}
			_state.LatestStartAt = {};
			_state.LastCaptureEndAt.reset();
			_state.ActiveCaptures.clear();
			_state.LastLifecycleChange = now;
			++_state.LifecycleRevision;
			_Publish();
		}

	private:
		void _Publish()
		{
			_published.store(std::make_shared<const CameraRecordingObservation>(_state),
				std::memory_order_release);
		}
		CameraRecordingObservation _state;
		std::uint64_t _nextStartOrder = 0u;
		std::atomic<std::shared_ptr<const CameraRecordingObservation>> _published
			{ std::make_shared<const CameraRecordingObservation>() };
	};
}
