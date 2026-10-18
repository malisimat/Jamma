#include "IoSessionLoader.h"
#include "Constants.h"
#include <algorithm>
#include <iostream>
#include <utility>

using namespace io;
using namespace engine;
using namespace audio;

IoSessionLoader::ReconstructedSession IoSessionLoader::Reconstruct(const JamFile& jam,
	const RigFile& rig, const std::wstring& directory)
{
	ReconstructedSession result;
	result.HudAudioInputCount = std::max(1u, rig.User.Audio.NumChannelsIn);
	for (const auto& triggerCfg : rig.Triggers)
	{
		for (const auto channel : triggerCfg.CaptureInputs.Audio)
			result.HudAudioInputCount = std::max(result.HudAudioInputCount, channel + 1u);
	}

	result.HudMidiInputs.reserve(rig.User.Midi.Devices.size());
	for (const auto& device : rig.User.Midi.Devices)
	{
		if (device.Enabled && !device.Id.empty())
			result.HudMidiInputs.push_back(device.Id);
	}

	result.TriggerParameters.DebounceMs = rig.User.Trigger.DebounceMs;

	StationParams stationParams;
	stationParams.Index = 0;
	stationParams.Position = { 20, 20 };
	stationParams.ModelPosition = { -50, -20 };
	stationParams.Size = { 136, 300 };
	stationParams.FadeSamps = rig.User.Loop.FadeSamps > constants::MaxLoopFadeSamps ?
		constants::MaxLoopFadeSamps :
		rig.User.Loop.FadeSamps;

	MergeMixBehaviourParams mergeParams;
	AudioMixerParams mixerParams = Station::GetMixerParams(stationParams.Size, mergeParams);
	result.Stations.reserve(jam.Stations.size());
	result.StationDescriptors.reserve(jam.Stations.size());

	size_t stationDescriptorIndex = 0u;
	for (const auto& stationStruct : jam.Stations)
	{
		auto station = Station::FromFile(stationParams, mixerParams, stationStruct, directory);
		if (station.has_value())
		{
			if (stationStruct.AllowedMidiChannels.empty() && !stationStruct.HasLiveVstRoutes)
			{
				const auto defaultChannel = static_cast<int>((stationParams.Index % 16u) + 1u);
				station.value()->SetAllowedMidiChannels({ defaultChannel });
			}

			result.Stations.push_back(station.value());
			result.StationDescriptors.push_back(stationStruct);
			stationParams.Index++;
			stationParams.Position += { 600, 0 };
			stationParams.ModelPosition += { 600, 0 };
		}
		else
			std::cout << "[LOAD] Station descriptor " << stationDescriptorIndex << " '" << stationStruct.Name
				<< "' was not constructed; continuing with the remaining stations." << std::endl;
		stationDescriptorIndex++;
	}
	std::cout << "[LOAD] Constructed " << result.Stations.size() << " of " << jam.Stations.size()
		<< " station descriptor(s)." << std::endl;
	if (jam.TransportInitialised && jam.Version == JamFile::VERSION_V && jam.MasterLengthSamps != 0ul)
		result.SavedMaster = MasterPosition{ jam.MasterLengthSamps, jam.AbsoluteSamplePos };
	return result;
}

void IoSessionLoader::RestoreTriggerHistories(const std::vector<JamFile::TriggerHistory>& histories,
	const RigSnapshot& initialRig, const std::vector<std::shared_ptr<Station>>& stations)
{
	// History refers to take identity in the saved session, even when the rig
	// has since selected a different station. Never guess an unavailable owner.
	const auto resolve = [&stations](const std::string& id)
		-> std::pair<std::shared_ptr<Station>, std::shared_ptr<LoopTake>> {
		if (id.empty()) return {};
		std::pair<std::shared_ptr<Station>, std::shared_ptr<LoopTake>> match;
		for (const auto& station : stations)
			for (const auto& take : station->GetLoopTakeSnapshot())
				if (take && take->Id() == id)
				{
					if (match.second)
					{
						std::cout << "[LOAD] Warning: ambiguous history take '" << id << "'." << std::endl;
						return {};
					}
					match = {station, take};
				}
		if (!match.second) std::cout << "[LOAD] Warning: unavailable history take '" << id << "'." << std::endl;
		return match;
	};
	for (const auto& runtime : initialRig.Triggers)
	{
		if (!runtime.Instance)
			continue;
		auto saved = std::find_if(histories.begin(), histories.end(),
			[&runtime](const io::JamFile::TriggerHistory& history) { return history.TriggerId == runtime.Id; });
		if (saved == histories.end())
			continue;
		std::vector<TriggerTake> takes;
		std::vector<std::pair<std::shared_ptr<base::TriggerPunchTarget>,
			std::shared_ptr<base::TriggerPunchTarget>>> pins;
		takes.reserve(saved->Takes.size());
		for (const auto& take : saved->Takes)
		{
			const auto source = resolve(take.SourceTakeId);
			const auto target = resolve(take.TargetTakeId);
			takes.push_back({ static_cast<decltype(TriggerTake{}.SourceType)>(take.SourceType), take.SourceTakeId, take.TargetTakeId, target.first });
			pins.emplace_back(source.second, target.second);
		}
		runtime.Instance->RestoreTakes(std::move(takes), std::move(pins));
	}
}
