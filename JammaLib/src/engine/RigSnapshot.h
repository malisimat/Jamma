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
		std::string Id;
		size_t RigTriggerIndex = 0u;
		std::shared_ptr<Trigger> Instance;
		std::optional<size_t> StationIndex;
		// Mutable only so prepared routing vectors can be swapped at the boundary without allocation.
		mutable std::shared_ptr<base::ActionReceiver> Receiver;
		mutable std::vector<unsigned int> InputChannels;
		mutable std::vector<std::string> MidiInputDevices;
		mutable io::RigFile::Trigger::MidiInputMode MidiInputMode = io::RigFile::Trigger::MidiInputMode::None;
		mutable std::shared_ptr<audio::AudioMixer> OverdubMixer;
		mutable std::shared_ptr<base::BounceWriter> OverdubWriter;
	};

	// A reused Trigger that receives a capture-route update at the audio boundary.
	struct TriggerRouteUpdate
	{
		std::shared_ptr<Trigger> Instance;
		size_t CandidateIndex = 0u;
	};

	// An outgoing Trigger instance that must be idle before replacement.
	struct TriggerReplacementCheck
	{
		std::shared_ptr<Trigger> AcceptedInstance;
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

	struct RigSnapshot
	{
		std::uint64_t Revision = 0u;
		io::RigFile Rig;
		RoutingGraph Graph;
		std::vector<RigSnapshotTrigger> Triggers;
		std::vector<TriggerReplacementCheck> TriggerReplacementChecks;
		std::vector<TriggerRouteUpdate> TriggerRouteUpdates;
		RigInputDispatch InputDispatch;
	};
}
