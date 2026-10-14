#pragma once
#include "RigCoordinator.h"
#include <algorithm>

namespace engine
{
	// Scene serializes all controller access with its off-callback scene mutex.
	// UI owns request/consent; job owns boundary progress; UI commits presentation.
	// Strong pins are released on UI after the audio/input acknowledgements.
	class StationManagement
	{
	public:
		enum class Validation { Ready, Confirm, Stale, FinalStation, Busy, Limit };
		bool Busy() const noexcept { return static_cast<bool>(Candidate); }
		Validation Validate(const std::vector<std::shared_ptr<Station>>& stations,
			std::uint64_t revision, bool consent) const
		{
			if (Busy()) return Validation::Busy;
			if (!Target || Revision != revision || Target->IsRemote() ||
				std::find(stations.begin(), stations.end(), Target) == stations.end()) return Validation::Stale;
			if (LocalCount(stations) <= 1u) return Validation::FinalStation;
			return !consent && !Target->GetLoopTakeSnapshot().empty() ? Validation::Confirm : Validation::Ready;
		}
		static size_t LocalCount(const std::vector<std::shared_ptr<Station>>& stations)
		{ return std::count_if(stations.begin(), stations.end(), [](const auto& s) { return s && !s->IsRemote(); }); }
		void Cancel() { Target.reset(); Revision = 0u; }
		void Finish() { Candidate.reset(); Added.reset(); Cancel(); CancellationStarted = Committed = Confirmed = NeedsConfirmation = Failed = false; }
		std::shared_ptr<Station> Target;
		std::uint64_t Revision = 0u;
		RigCoordinator::SnapshotPtr Candidate;
		std::shared_ptr<Station> Added;
		bool CancellationStarted = false;
		bool Committed = false;
		bool Failed = false;
		bool Confirmed = false;
		bool NeedsConfirmation = false;
	};
}
