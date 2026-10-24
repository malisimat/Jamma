#include "IoSessionLoader.h"
#include "Constants.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <utility>

using namespace io;
using namespace engine;
using namespace audio;

void IoSessionLoader::AssignMissingIdentities(JamFile& jam)
{
	std::set<std::string> stationIds, takeIds, audioLoopIds, midiLoopIds, pluginIds;
	for (const auto& station : jam.Stations)
	{
		if (!station.Id.empty()) stationIds.insert(station.Id);
		for (const auto& plugin : station.VstChain) if (!plugin.Id.empty()) pluginIds.insert(plugin.Id);
		for (const auto& take : station.LoopTakes)
		{
			if (!take.Id.empty()) takeIds.insert(take.Id);
			for (const auto& plugin : take.VstChain) if (!plugin.Id.empty()) pluginIds.insert(plugin.Id);
			for (const auto& loop : take.Loops)
			{
				if (!loop.Id.empty()) audioLoopIds.insert(loop.Id);
				for (const auto& plugin : loop.VstChain) if (!plugin.Id.empty()) pluginIds.insert(plugin.Id);
			}
			for (const auto& stream : take.MidiStreams)
				if (!stream.Id.empty()) midiLoopIds.insert(stream.Id);
		}
	}
	const auto assign = [](std::set<std::string>& used, std::string& id, std::string preferred, const char* kind)
	{
		if (!id.empty()) return;
		if (preferred.empty() || preferred.size() > 256u) preferred = kind;
		id = preferred;
		for (std::size_t suffix = 1u; !used.insert(id).second; ++suffix)
			id = std::string(kind) + ":" + std::to_string(suffix);
	};
	for (std::size_t stationIndex = 0u; stationIndex < jam.Stations.size(); ++stationIndex)
	{
		auto& station = jam.Stations[stationIndex];
		assign(stationIds, station.Id, "legacy:station:" + std::to_string(stationIndex), "station");
		for (std::size_t pluginIndex = 0u; pluginIndex < station.VstChain.size(); ++pluginIndex)
			assign(pluginIds, station.VstChain[pluginIndex].Id,
				station.Id + ":vst:" + std::to_string(pluginIndex), "vst");
		for (std::size_t takeIndex = 0u; takeIndex < station.LoopTakes.size(); ++takeIndex)
		{
			auto& take = station.LoopTakes[takeIndex];
			assign(takeIds, take.Id, take.Name.empty() ? station.Id + ":take:" + std::to_string(takeIndex) : take.Name, "take");
			for (std::size_t pluginIndex = 0u; pluginIndex < take.VstChain.size(); ++pluginIndex)
				assign(pluginIds, take.VstChain[pluginIndex].Id,
					take.Id + ":vst:" + std::to_string(pluginIndex), "vst");
			for (std::size_t loopIndex = 0u; loopIndex < take.Loops.size(); ++loopIndex)
			{
				auto& loop = take.Loops[loopIndex];
				assign(audioLoopIds, loop.Id, take.Id + ":loop:" + std::to_string(loopIndex), "loop");
				for (std::size_t pluginIndex = 0u; pluginIndex < loop.VstChain.size(); ++pluginIndex)
					assign(pluginIds, loop.VstChain[pluginIndex].Id,
						loop.Id + ":vst:" + std::to_string(pluginIndex), "vst");
			}
			for (std::size_t streamIndex = 0u; streamIndex < take.MidiStreams.size(); ++streamIndex)
				assign(midiLoopIds, take.MidiStreams[streamIndex].Id,
					take.Id + ":midi:" + std::to_string(streamIndex), "midi");
		}
	}
}

IoSessionLoader::ReconstructedSession IoSessionLoader::Reconstruct(const JamFile& jam,
	const RigFile& rig, const std::wstring& directory)
{
	ReconstructedSession result;
	JamFile restoredJam = jam;
	AssignMissingIdentities(restoredJam);
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
	result.Stations.reserve(restoredJam.Stations.size());
	result.StationDescriptors.reserve(restoredJam.Stations.size());

	size_t stationDescriptorIndex = 0u;
	for (const auto& stationStruct : restoredJam.Stations)
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
	std::cout << "[LOAD] Constructed " << result.Stations.size() << " of " << restoredJam.Stations.size()
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
