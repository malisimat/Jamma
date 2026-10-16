#pragma once

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include "RigFile.h"
#include "JamFile.h"

namespace io
{
	struct StartupConfig
	{
		static constexpr std::string_view GeneratedOrigin = "generated-v1";
		enum class FileState { Valid, Missing, Unreadable, Invalid };
		struct Decision
		{
			FileState State;
			bool UseExisting;
			bool Generate;
			bool Recovery;
			bool PublishedGenerated;
		};
		static constexpr FileState Classify(bool exists, bool readable, bool valid) noexcept
		{
			return !exists ? FileState::Missing : !readable ? FileState::Unreadable :
				valid ? FileState::Valid : FileState::Invalid;
		}
		static constexpr Decision Decide(FileState state, std::string_view origin) noexcept
		{
			const bool valid = state == FileState::Valid;
			return { state, valid, !valid, state == FileState::Unreadable || state == FileState::Invalid,
				valid && origin == GeneratedOrigin };
		}
		static bool ValidateRig(const RigFile& rig) { return RigFile::Validate(rig); }
		static bool ValidateJam(const JamFile& jam)
		{
			if (jam.Stations.empty()) return false;
			std::unordered_set<std::string> names;
			for (const auto& station : jam.Stations)
				if (station.Name.empty() || !names.insert(station.Name).second) return false;
			return true;
		}
		// Missing/ambiguous rig connections are availability, not malformed data.
		// Never replace a curated rig merely because a different jam is loaded.
		static bool ValidateRigForJam(const RigFile& rig, const JamFile& jam)
		{
			return ValidateRig(rig) && ValidateJam(jam);
		}
		static RigFile GeneratedRig(const RigFile& templateRig, unsigned int inputChannels,
			const std::vector<std::string>& midiHardwareNames, const std::string& stationTarget = "Station1")
		{
			auto rig = templateRig;
			rig.Name = "First-run rig";
			rig.User.Midi.Devices.clear();
			std::vector<std::string> registryIds;
			for (size_t i = 0u; i < midiHardwareNames.size(); ++i)
			{
				const auto id = "midi-" + std::to_string(i + 1u);
				rig.User.Midi.Devices.push_back({ midiHardwareNames[i], true, id });
				registryIds.push_back(id);
			}
			if (rig.Triggers.empty()) rig.Triggers.emplace_back();
			rig.Triggers.front().Id = "first-run-trigger-1";
			rig.Triggers.front().Name = "Trig1";
			return WithOpenedInputs(rig, inputChannels, registryIds, stationTarget);
		}
		// Keep registry IDs/configuration stable when opening only a subset of
		// generated requests. Successful endpoint IDs are not renumbered.
		static RigFile WithOpenedInputs(const RigFile& source, unsigned int inputChannels,
			const std::vector<std::string>& connectedRegistryIds, const std::string& stationTarget)
		{
			auto rig = source;
			if (rig.Triggers.empty()) return rig;
			auto& trigger = rig.Triggers.front();
			trigger.StationTarget = stationTarget;
			trigger.CaptureInputs.Audio.clear();
			for (unsigned int channel = 0u; channel < inputChannels && channel < 2u; ++channel)
				trigger.CaptureInputs.Audio.push_back(channel);
			trigger.CaptureInputs.Midi.clear();
			auto removeMidi = [](std::vector<RigFile::Binding>& bindings) {
				std::erase_if(bindings, [](const RigFile::Binding& binding) {
					return binding.Source == RigFile::BindingSource::Midi;
				});
			};
			removeMidi(trigger.ControlBindings.Activate);
			removeMidi(trigger.ControlBindings.Ditch);
			if (!connectedRegistryIds.empty())
			{
				auto button = [&](unsigned int note) {
					RigFile::Binding binding;
					binding.Source = RigFile::BindingSource::Midi;
					binding.Device = connectedRegistryIds.front();
					binding.Press = { note, 1u, RigFile::NOTE, 0u };
					binding.Release = { note, 0u, RigFile::NOTE, 0u };
					return binding;
				};
				trigger.ControlBindings.Activate.push_back(button(1u));
				trigger.ControlBindings.Ditch.push_back(button(2u));
			}
			return rig;
		}
	};
}
