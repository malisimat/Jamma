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
		// Staged data is handed to the reused trigger only at the audio boundary.
		// It is mutable solely to exchange preallocated vectors without allocation.
		mutable std::shared_ptr<base::ActionReceiver> Receiver;
		mutable std::vector<unsigned int> InputChannels;
		mutable std::vector<std::string> MidiInputDevices;
		mutable io::RigFile::Trigger::MidiInputMode MidiInputMode = io::RigFile::Trigger::MidiInputMode::None;
		mutable std::unique_ptr<audio::MixBehaviour> OverdubBehaviour;
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
		// Indices of accepted-revision triggers that must be quiescent before this
		// revision can replace them. Unchanged trigger instances are retained so a
		// playing loop does not disable edits to unrelated routing.
		std::vector<size_t> ChangedTriggerIndices;
		// Reused trigger instances that receive a new capture route or station
		// receiver. Their history is retained, but the current action must be idle.
		std::vector<size_t> CaptureRoutingChangeTriggerIndices;
		std::vector<StationTriggerMembership> StationMemberships;
		RigInputDispatch InputDispatch;
	};
}
