#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "Trigger.h"
#include "../io/RigFile.h"

namespace engine
{
	struct RoutingGraph
	{
		std::uint64_t Revision = 1u;
		std::vector<io::RigRouting::TriggerResolution> Triggers;
	};

	struct RoutingRuntimeTrigger
	{
		size_t RigTriggerIndex = 0u;
		std::shared_ptr<Trigger> Instance;
		std::optional<size_t> StationIndex;
	};

	// Construction-time ownership for the initial resolved rig. Slice 3 extends
	// publication; this value deliberately has no pending/applied coordination.
	struct RoutingRuntime
	{
		io::RigFile Rig;
		RoutingGraph Graph;
		std::vector<RoutingRuntimeTrigger> Triggers;

		static RoutingRuntime BuildInitial(const io::RigFile& rig,
			const std::vector<io::JamFile::Station>& stations,
			unsigned int availableAdcChannels,
			const std::vector<std::string>& availableMidiDevices,
			const TriggerParams& triggerParams);
	};
}
