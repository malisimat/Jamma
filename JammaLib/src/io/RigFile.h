///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <vector>
#include <stack>
#include <map>
#include <optional>
#include <variant>
#include <iostream>
#include <sstream>
#include "Json.h"
#include "UserConfig.h"
#include "JamFile.h"

namespace io
{
	struct RigFile
	{
		static constexpr const char* CurrentVersion = "1.0";

		enum MidiTriggerEvent
		{
			NOTE,
			CC
		};

		static std::optional<RigFile> FromStream(std::stringstream ss);
		static bool ToStream(RigFile jam, std::stringstream& ss);
		static bool ToJsonStream(const RigFile& rig, std::stringstream& ss);
		static const std::string DefaultJson;


		struct TriggerPair
		{
			enum BindingSource
			{
				SOURCE_KEYBOARD,
				SOURCE_SERIAL
			};

			unsigned int ActivateDown;
			unsigned int ActivateUp;
			unsigned int DitchDown;
			unsigned int DitchUp;
			BindingSource Source = SOURCE_KEYBOARD;
			std::string Device;

			static std::optional<TriggerPair> FromJson(Json::JsonPart json);
		};

		struct Trigger
		{
			enum class MidiInputMode
			{
				None,
				Selected
			};
			struct MidiTriggerBindingSpec
			{
				MidiTriggerEvent Kind;
				unsigned int Channel;
				unsigned int Id;
				unsigned int State;
				bool MatchAnyChannel;

				static std::optional<MidiTriggerBindingSpec> FromJson(Json::JsonPart json);
			};

			struct MidiTriggerBinding
			{
				std::string Device;
				MidiTriggerBindingSpec Activate;
				MidiTriggerBindingSpec Ditch;

				static std::optional<MidiTriggerBinding> FromJson(Json::JsonPart json);
			};

			// Routing normalization fills empty IDs from legacy files.
			std::string Id;
			std::string Name;
			unsigned int StationType;
			std::vector<TriggerPair> TriggerPairs;
			std::vector<unsigned int> InputChannels;
			std::vector<std::string> MidiInputDevices;
			std::optional<std::string> StationTarget;
			// Live MIDI capture is opt-in: an empty device list captures no MIDI.
			MidiInputMode MidiInputs = MidiInputMode::None;
			std::optional<MidiTriggerBinding> MidiTrigger;

			static std::optional<Trigger> FromJson(Json::JsonPart json);
		};

		// Store versions as strings so migrations do not depend on enum ordinals.
		std::string Version = CurrentVersion;
		std::string Name;
		UserConfig User;
		std::vector<Trigger> Triggers;
	};

	struct RigFileRouting
	{
		enum class SourceKind { Adc, Midi };
		enum class Warning { None, LegacyStationTargetMigrated, TargetMissing, TargetAmbiguous };

		struct Source
		{
			SourceKind Kind;
			unsigned int AdcChannel = 0u;
			std::string MidiDevice;
			bool Available = false;
		};

		struct TriggerResolution
		{
			size_t TriggerIndex = 0u;
			std::string TriggerName;
			std::optional<std::string> TargetName;
			std::optional<size_t> StationIndex;
			std::vector<Source> Sources;
			Warning Reason = Warning::None;
		};

		struct Resolution
		{
			RigFile CandidateRig;
			std::vector<TriggerResolution> Triggers;
			bool RequiresSave = false;
			bool IsValid = true;
		};

		static Resolution Resolve(const RigFile& rig,
			const std::vector<JamFile::Station>& stations,
			unsigned int availableAdcChannels,
			const std::vector<std::string>& availableMidiDevices);
		static std::string NextTriggerName(const RigFile& rig);
		static RigFile WithUnboundTrigger(const RigFile& rig);
		static std::optional<RigFile> WithoutTrigger(const RigFile& rig, size_t triggerIndex);
		static std::optional<RigFile> WithStationTarget(const RigFile& rig, size_t triggerIndex, std::string target);
		static std::optional<RigFile> WithAdcInput(const RigFile& rig, size_t triggerIndex, unsigned int channel);
		static std::optional<RigFile> WithoutAdcInput(const RigFile& rig, size_t triggerIndex, unsigned int channel);
		static std::optional<RigFile> WithMidiInput(const RigFile& rig, size_t triggerIndex, std::string device);
		static std::optional<RigFile> WithoutMidiInput(const RigFile& rig, size_t triggerIndex, const std::string& device);
	};
}
