#include "RoutingRuntime.h"

using namespace engine;

RoutingRuntime RoutingRuntime::BuildInitial(const io::RigFile& rig,
	const std::vector<io::JamFile::Station>& stations,
	unsigned int availableAdcChannels,
	const std::vector<std::string>& availableMidiDevices,
	const TriggerParams& triggerParams)
{
	auto resolution = io::RigRouting::Resolve(rig, stations, availableAdcChannels, availableMidiDevices);

	RoutingRuntime runtime;
	// Scene construction may follow legacy positional routing, but it must not
	// silently adopt an unsaved migration candidate after persistence failed.
	runtime.Rig = rig;
	runtime.Graph.Triggers = std::move(resolution.Triggers);
	runtime.Triggers.reserve(runtime.Rig.Triggers.size());

	for (size_t triggerIndex = 0u; triggerIndex < runtime.Rig.Triggers.size(); ++triggerIndex)
	{
		auto trigger = Trigger::FromFile(triggerParams, runtime.Rig.Triggers[triggerIndex]);
		if (!trigger.has_value())
			continue;

		std::optional<size_t> stationIndex;
		if (triggerIndex < runtime.Graph.Triggers.size())
			stationIndex = runtime.Graph.Triggers[triggerIndex].StationIndex;

		runtime.Triggers.push_back({ triggerIndex, std::move(trigger.value()), stationIndex });
	}

	return runtime;
}
