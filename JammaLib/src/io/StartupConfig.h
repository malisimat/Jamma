///////////////////////////////////////////////////////////
//
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include "RigFile.h"
#include "JamFile.h"

namespace io
{
	// Pure startup decisions. Callers perform filesystem reads and semantic
	// validation before passing their results here; neither file depends on the
	// other one's validity.
	struct StartupConfig
	{
		static constexpr std::string_view GeneratedOrigin = "generated-v1";

		enum class FileState
		{
			Valid,
			Missing,
			Unreadable,
			Invalid
		};

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
			return !exists ? FileState::Missing
				: !readable ? FileState::Unreadable
				: valid ? FileState::Valid : FileState::Invalid;
		}

		static constexpr Decision Decide(FileState state, std::string_view origin) noexcept
		{
			const bool valid = state == FileState::Valid;
			return { state, valid, !valid, state == FileState::Unreadable || state == FileState::Invalid,
				valid && origin == GeneratedOrigin };
		}

		static bool ValidateRig(const RigFile& rig)
		{
			const auto& audio = rig.User.Audio;
			if (rig.Name.empty() || audio.Name.empty() || audio.SampleRate == 0u ||
				audio.BufSize == 0u || audio.NumBuffers == 0u || audio.NumChannelsOut == 0u ||
				rig.Triggers.empty())
				return false;
			std::unordered_set<std::string> names;
			std::unordered_set<std::string> ids;
			for (const auto& trigger : rig.Triggers)
			{
				if (trigger.Name.empty() || !names.insert(trigger.Name).second ||
					(!trigger.Id.empty() && !ids.insert(trigger.Id).second))
					return false;
			}
			return true;
		}

		static bool ValidateJam(const JamFile& jam)
		{
			if (jam.Stations.empty())
				return false;
			std::unordered_set<std::string> names;
			for (const auto& station : jam.Stations)
				if (station.Name.empty() || !names.insert(station.Name).second)
					return false;
			return true;
		}

		// Explicit empty targets are intentionally unbound. Absent targets keep
		// the legacy positional migration path and are validated when Scene loads.
		static bool ValidateRigForJam(const RigFile& rig, const JamFile& jam)
		{
			if (!ValidateRig(rig) || !ValidateJam(jam))
				return false;
			std::unordered_set<std::string> stationNames;
			stationNames.reserve(jam.Stations.size());
			for (const auto& station : jam.Stations)
				stationNames.insert(station.Name);
			for (const auto& trigger : rig.Triggers)
				if (trigger.StationTarget && !trigger.StationTarget->empty() &&
					!stationNames.contains(*trigger.StationTarget))
					return false;
			return true;
		}

		// The first-run route is based on opened endpoints, never advertised ports.
		static RigFile GeneratedRig(const RigFile& templateRig,
			unsigned int inputChannels, const std::vector<std::string>& connectedMidi,
			const std::string& stationTarget = "Station1")
		{
			RigFile rig = templateRig;
			rig.Name = "First-run rig";
			rig.User.Midi.Devices.clear();
			for (const auto& name : connectedMidi)
				rig.User.Midi.Devices.push_back({ name, true });
			auto& trigger = rig.Triggers.front();
			trigger.Id = "first-run-trigger-1";
			trigger.Name = "Trig1";
			trigger.StationTarget = stationTarget;
			trigger.InputChannels.clear();
			for (unsigned int channel = 0; channel < inputChannels && channel < 2u; ++channel)
				trigger.InputChannels.push_back(channel);
			trigger.MidiInputs = RigFile::Trigger::MidiInputMode::None;
			trigger.MidiInputDevices.clear();
			trigger.MidiTrigger.reset();
			if (!connectedMidi.empty())
			{
				RigFile::Trigger::MidiTriggerBinding binding{};
				binding.Device = connectedMidi.front();
				binding.Activate = { RigFile::NOTE, 0u, 1u, 1u, false };
				binding.Ditch = { RigFile::NOTE, 0u, 2u, 1u, false };
				trigger.MidiTrigger = std::move(binding);
			}
			return rig;
		}
	};
}
