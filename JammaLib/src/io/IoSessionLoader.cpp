#include "IoSessionLoader.h"
#include "Constants.h"
#include "NativeMidiSidecar.h"
#include "../midi/MidiLoop.h"
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

std::optional<std::shared_ptr<Station>> IoSessionLoader::ReconstructStation(StationParams stationParams,
	AudioMixerParams mixerParams, JamFile::Station stationStruct, std::wstring dir)
{
	std::cout << "[LOAD] Constructing station " << stationParams.Index << " '" << stationStruct.Name
		<< "' (takes=" << stationStruct.LoopTakes.size() << ", VSTs=" << stationStruct.VstChain.size() << ")." << std::endl;
	stationParams.Name = stationStruct.Name;
	stationParams.Id = stationStruct.Id;
	auto station = std::make_shared<Station>(stationParams, mixerParams);
	station->SetStationPhaseOffsetSamps(stationStruct.StationPhaseOffsetSamps);
	station->SetAllowedMidiChannels(stationStruct.AllowedMidiChannels);
	std::vector<std::string> identities;
	bool rackInactive = stationStruct.VstChain.size() > Station::MaxRackPlugins;
	for (std::size_t index = 0; index < stationStruct.VstChain.size(); ++index)
	{
		auto& entry = stationStruct.VstChain[index];
		// Legacy entries without identity receive a deterministic local identity.
		if (entry.Id.empty()) entry.Id = station->Id() + ":vst:" + std::to_string(index);
		if (!io::NativeMidiSidecar::ValidatePluginIdentity(entry.Id) || std::find(identities.begin(), identities.end(), entry.Id) != identities.end()) rackInactive = true;
		identities.push_back(entry.Id);
	}
	for (const auto& route : stationStruct.LiveVstRoutes)
	{
		if (route.PluginIds.size() > Station::MaxRackPlugins) rackInactive = true;
		for (const auto& identity : route.PluginIds)
			if (identity.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(identity)) rackInactive = true;
	}
	station->RestoreSavedRack(stationStruct.VstChain, stationStruct.LiveVstRoutes, rackInactive);
	// Legacy automation sidecars contain indices only. Give saved processors
	// deterministic IDs before reconstruction, including unavailable Take/Loop
	// slots, so later compaction cannot change the meaning of those bindings.
	for (std::size_t takeIndex = 0u; takeIndex < stationStruct.LoopTakes.size(); ++takeIndex)
	{
		auto& savedTake = stationStruct.LoopTakes[takeIndex];
		if (savedTake.Id.empty()) savedTake.Id = !savedTake.Name.empty() ? savedTake.Name
			: station->Id() + ":take:" + std::to_string(takeIndex);
		for (std::size_t pluginIndex = 0u; pluginIndex < savedTake.VstChain.size(); ++pluginIndex)
			if (savedTake.VstChain[pluginIndex].Id.empty()) savedTake.VstChain[pluginIndex].Id
				= savedTake.Id + ":vst:" + std::to_string(pluginIndex);
		for (std::size_t loopIndex = 0u; loopIndex < savedTake.Loops.size(); ++loopIndex)
			for (std::size_t pluginIndex = 0u; pluginIndex < savedTake.Loops[loopIndex].VstChain.size(); ++pluginIndex)
				if (savedTake.Loops[loopIndex].VstChain[pluginIndex].Id.empty()) savedTake.Loops[loopIndex].VstChain[pluginIndex].Id
					= savedTake.Id + ":loop:" + std::to_string(loopIndex) + ":vst:" + std::to_string(pluginIndex);
	}
	for (const auto& vstEntry : stationStruct.VstChain)
	{
		if (rackInactive) continue;
		std::cout << "[LOAD] Station '" << stationStruct.Name << "': loading VST " << vstEntry.Path << std::endl;
		if (!station->LoadVstPluginSynchronously(utils::DecodeUtf8(vstEntry.Path), vstEntry.DecodeState(), vstEntry.Bypass, vstEntry.Id))
		{
			// Restore the station and rig target even when this optional processor is missing.
			std::cout << "[LOAD] Station '" << stationStruct.Name << "': failed VST " << vstEntry.Path
				<< "; continuing without it." << std::endl;
		}
	}

	auto numTakes = (unsigned int)stationStruct.LoopTakes.size();
	utils::Size2d gap = { 4, 4 };
	auto takeHeight = numTakes > 0 ?
		stationParams.Size.Height - ((2 + numTakes - 1) * gap.Height) / numTakes :
		stationParams.Size.Height - (2 * gap.Height);
	takeHeight = takeHeight < 1u ? 1u : takeHeight;
	utils::Size2d takeSize = { stationParams.Size.Width - (2 * gap.Width), takeHeight };
	LoopTakeParams takeParams;
	takeParams.Size = { 80, 80 };

	auto takeCount = 0u;
	for (auto takeStruct : stationStruct.LoopTakes)
	{
		takeParams.ModelPosition = { (float)gap.Width, (float)(takeCount * takeHeight + gap.Height), 0.0 };
		// Legacy descriptors without identity receive one during import. Keep
		// the display name separate from job deduplication/group identity.
		// Legacy JAM manifests stored take identity in Name. Retain it so trigger
		// histories and source references survive migration to the explicit Id.
		takeParams.Id = !takeStruct.Id.empty() ? takeStruct.Id :
			!takeStruct.Name.empty() ? takeStruct.Name : utils::GetGuid();
		auto take = LoopTake::FromFile(takeParams, takeStruct, dir);

		if (take.has_value())
		{
			station->AddTake(take.value());
			// AddTake establishes the station bus count. Saved routes are applied
			// afterwards so its normal one-to-one defaults cannot overwrite them.
			if (takeStruct.HasAudioRoutes && !take.value()->RestoreAudioRoutes(takeStruct.AudioRoutes))
			{
				std::cout << "Load: invalid audio routes in take " << takeStruct.Name << std::endl;
				return std::nullopt;
			}
		}

		takeCount++;
	}
	if (stationStruct.HasAudioRoutes && !station->RestoreAudioRoutes(stationStruct.AudioRoutes))
	{
		std::cout << "Load: invalid audio routes in station " << stationStruct.Name << std::endl;
		return std::nullopt;
	}
	if (!station->RestoreMixerLevels(stationStruct.MasterLevel, stationStruct.BusLevels))
		return std::nullopt;
	// Empty stations are valid saved state: a station may contain its VST chain,
	// routing configuration, or simply be ready for a new take. Only malformed
	// station data should prevent the station from being reconstructed.
	for (const auto& take : station->GetLoopTakes())
	{
		if (!take)
			continue;
		const auto savedTake = std::find_if(stationStruct.LoopTakes.begin(), stationStruct.LoopTakes.end(),
			[&](const auto& saved) { return saved.Id == take->Id(); });

		for (const auto& binding : take->PendingAutomationBindings())
		{
			auto midiLoops = take->GetMidiLoopSnapshot();
			if (binding.MidiStreamIndex >= midiLoops.size() || binding.LaneIndex >= midi::MidiLoop::MaxAutomationLanes)
				return std::nullopt;
			if (!midiLoops[binding.MidiStreamIndex]) return std::nullopt;
			auto& savedTarget = midiLoops[binding.MidiStreamIndex]->GetLane(binding.LaneIndex).Mapping.SavedTarget;
			// Migrate old station indices through the saved chain even if the rack
			// is inactive/missing: compaction must never redirect this lane later.
			if (savedTarget.PluginId.empty() && binding.TargetScope == "station"
				&& binding.TargetPluginIndex < stationStruct.VstChain.size())
				savedTarget.PluginId = stationStruct.VstChain[binding.TargetPluginIndex].Id;
			if (savedTarget.PluginId.empty() && savedTake != stationStruct.LoopTakes.end())
			{
				if (binding.TargetScope == "take" && binding.TargetPluginIndex < savedTake->VstChain.size())
					savedTarget.PluginId = savedTake->VstChain[binding.TargetPluginIndex].Id;
				else if (binding.TargetScope == "loop" && binding.TargetLoopIndex < savedTake->Loops.size()
					&& binding.TargetPluginIndex < savedTake->Loops[binding.TargetLoopIndex].VstChain.size())
					savedTarget.PluginId = savedTake->Loops[binding.TargetLoopIndex].VstChain[binding.TargetPluginIndex].Id;
			}

			// Keep raw malformed rack metadata, scope/index and curve, but migrate
			// its legacy positional target to an explicit valid unavailable ID.
			if (savedTarget.PluginId.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(savedTarget.PluginId))
				savedTarget.PluginId = "unavailable:" + utils::GetGuid();
			std::shared_ptr<vst::IVstPlugin> plugin;
			if (binding.TargetScope == "station")
				plugin = station->GetVstPlugin(binding.TargetPluginIndex);
			else if (binding.TargetScope == "take")
				plugin = take->GetVstPlugin(binding.TargetPluginIndex);
			else if (binding.TargetScope == "loop")
			{
				const auto& loops = take->GetLoops();
				if (binding.TargetLoopIndex < loops.size() && loops[binding.TargetLoopIndex])
					plugin = loops[binding.TargetLoopIndex]->GetVstPlugin(binding.TargetPluginIndex);
			}

			if (plugin && !savedTarget.PluginId.empty() && plugin->StateId() != savedTarget.PluginId) plugin.reset();
			if (!plugin || !midiLoops[binding.MidiStreamIndex]->BindAutomationLaneTarget(
				binding.LaneIndex, plugin.get(), plugin->StateId()))
			{
				std::cout << "[LOAD] Station '" << stationStruct.Name
					<< "': retaining unavailable automation target." << std::endl;
			}
		}
		take->ClearPendingAutomationBindings();
	}
	// Publish the completed pointer bindings as one immutable dispatch list before
	// this startup-built station is made visible to audio.
	station->RebuildAutomationDispatch();

	if (!stationStruct.MidiRoutes.empty())
	{
		std::size_t outputCount = 0u;
		for (const auto& take : station->GetLoopTakes())
			if (take)
				outputCount += take->GetMidiLoopSnapshot().size();

		midi::MidiVstRoutingSnapshot routes;
		routes.PluginByMidiOutput.assign(outputCount, midi::MidiVstRoutingSnapshot::NoPlugin);
		auto liveRoutes = station->LiveVstChannelRoutes();
		for (const auto& route : stationStruct.MidiRoutes)
		{
			if ((route.PluginIndex >= stationStruct.VstChain.size() && route.PluginIndex != midi::MidiVstRoutingSnapshot::MissingPlugin)
				|| (!route.IsLive && route.OutputIndex >= outputCount))
			{
				std::cout << "Load: invalid MIDI VST route in station " << stationStruct.Name << std::endl;
				return std::nullopt;
			}
			if (route.IsLive)
			{
				if (!stationStruct.HasLiveVstRoutes)
					for (auto& channel : liveRoutes)
					{
						channel.Explicit = true;
						if (route.PluginIndex < stationStruct.VstChain.size())
							channel.PluginIds = {stationStruct.VstChain[route.PluginIndex].Id};
					}
			}
			else
				routes.PluginByMidiOutput[route.OutputIndex] = route.PluginIndex;
		}
		station->RestoreLiveVstRoutesBeforePublication(std::move(liveRoutes));
		if (!station->RestoreMidiVstRoutes(routes, stationStruct.VstChain.size()))
		{
			std::cout << "Load: failed MIDI VST routes for station " << stationStruct.Name << std::endl;
			return std::nullopt;
		}
	}
	// Validate the complete pending take hierarchy before Scene admits audio.
	station->RefreshRackLatency();
	std::cout << "[LOAD] Constructed station '" << stationStruct.Name << "'." << std::endl;

	return station;
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
		auto station = ReconstructStation(stationParams, mixerParams, stationStruct, directory);
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
