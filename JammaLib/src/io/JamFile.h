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
#include <cstdint>
#include "Json.h"
#include "Timer.h"
#include "../midi/MidiQuantisation.h"
#include "../audio/AudioMixer.h"

namespace io
{
	struct JamFile
	{
		enum class GlobalMidiQuantState : std::uint8_t
		{
			Off = 0,
			Mixed = 1,
			All = 2
		};

		enum Version
		{
			VERSION_V,
			VERSION_LEGACY
		};

		static std::optional<JamFile> FromStream(std::stringstream ss);
		static bool ToStream(JamFile jam, std::stringstream& ss);
		static const std::string DefaultJson;
		static std::int32_t ParseInt32Clamped(const Json::JsonValue& value, std::int32_t fallback) noexcept;

		struct NinjamConfig
		{
			std::string Host;
			std::string User;
			std::string Pass;
			std::string WorkDir;
			std::optional<double> Bpm;
			std::optional<unsigned long> Bpi;

			static std::optional<NinjamConfig> FromJson(Json::JsonPart json);
		};

		struct LoopMix
		{
			enum MixType
			{
				MIX_WIRE,
				MIX_PAN
			};

			MixType Mix;
			std::variant<std::vector<unsigned long>,std::vector<double>> Params;

			static std::optional<LoopMix> FromJson(Json::JsonPart json);
		};

		// Describes a single VST entry in a VstChain.
		struct VstEntry
		{
			// UTF-8 path to the .vst3 bundle or DLL.
			std::string Path;
			bool Bypass = false;
			// Base64-encoded VST2 state blob (from IVstPlugin::GetState).
			// Empty string means no saved state for this entry.
			std::string State;

			static std::optional<VstEntry> FromJson(Json::JsonPart json);

			// Encode a raw state blob to base64 for storage in State.
			static std::string EncodeState(const std::vector<std::uint8_t>& blob);
			// Decode State back to a raw blob for IVstPlugin::SetState.
			// Returns an empty vector when State is empty or malformed.
			std::vector<std::uint8_t> DecodeState() const;
		};

		struct Loop
		{
			std::string Name;
			unsigned long Length;
			unsigned long Index;
			unsigned long MasterLoopCount;
			double Level;
			double Speed;
			unsigned int MuteGroups;
			unsigned int SelectGroups;
			bool Muted;
			LoopMix Mix;
			std::vector<VstEntry> VstChain;

			static std::optional<Loop> FromJson(Json::JsonPart json);
		};

		struct LoopTake
		{
			std::string Name;
			std::vector<Loop> Loops;
			std::vector<VstEntry> VstChain;
			bool MidiQuantEnabled = false;
			int MidiQuantFraction = static_cast<int>(midi::MidiQuantisationFraction::Quarter);
			std::int32_t TakePhaseOffsetSamps = 0;

			static std::optional<LoopTake> FromJson(Json::JsonPart json);
		};

		struct Station
		{
			std::string Name;
			unsigned int StationType;
			std::vector<LoopTake> LoopTakes;
			std::vector<VstEntry> VstChain;
			std::int32_t StationPhaseOffsetSamps = 0;
			std::vector<int> AllowedMidiChannels;

			static std::optional<Station> FromJson(Json::JsonPart json);
		};

		Version Version;
		std::string Name;
		std::optional<NinjamConfig> Ninjam;
		std::vector<Station> Stations;
		unsigned long TimerTicks;
		unsigned int QuantiseSamps;
		GlobalMidiQuantState GlobalMidiQuantStateValue = GlobalMidiQuantState::Mixed;
		std::int32_t GlobalPhaseOffsetSamps = 0;
		utils::Timer::QuantisationType Quantisation;
	};
}
