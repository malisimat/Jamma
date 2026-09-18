#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "Station.h"
#include "../io/RigFile.h"

namespace engine
{
	struct RoutingGraph
	{
		std::uint64_t Revision = 0u;
		std::vector<io::RigFileRouting::TriggerResolution> Triggers;
	};

	struct RigSnapshotTrigger
	{
		size_t RigTriggerIndex = 0u;
		std::shared_ptr<Trigger> Instance;
		std::optional<size_t> StationIndex;
	};

	struct StationTriggerMembership
	{
		using StationPtr = std::shared_ptr<engine::Station>;
		StationPtr Station;
		std::shared_ptr<const Station::TriggerMembership> Triggers;
	};

	struct MidiTriggerDispatch
	{
		std::string DeviceName;
		std::shared_ptr<Trigger> TriggerInstance;
	};

	struct LiveMidiDispatch
	{
		std::string DeviceName;
		std::vector<std::shared_ptr<Station>> Recipients;
	};

	struct RigInputDispatch
	{
		std::uint64_t Revision = 0u;
		std::vector<MidiTriggerDispatch> MidiTriggers;
		std::vector<LiveMidiDispatch> LiveMidi;
		std::vector<std::shared_ptr<Trigger>> SerialTriggers;
		std::vector<std::shared_ptr<Trigger>> KeyboardTriggers;
	};

	// Complete immutable realization of one rig revision. The coordinator owns
	// snapshots, while audio and input readers retain shared references.
	struct RigSnapshot
	{
		std::uint64_t Revision = 0u;
		io::RigFile Rig;
		RoutingGraph Graph;
		std::vector<RigSnapshotTrigger> Triggers;
		std::vector<StationTriggerMembership> StationMemberships;
		RigInputDispatch InputDispatch;
	};
}
