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
#include <cstddef>
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
		static constexpr unsigned int CurrentFormatMajor = 0u;
		static constexpr unsigned int CurrentFormatMinor = 1u;
		static constexpr unsigned int CurrentFormatPatch = 0u;
		static constexpr std::size_t MaxStations = 256u;
		static constexpr std::size_t MaxTakesPerStation = 256u;
		static constexpr std::size_t MaxLoopsPerTake = 1024u;
		static constexpr std::size_t MaxMidiStreamsPerTake = 128u;
		static constexpr unsigned long MaxLoopLengthSamps = 0x7fffffffu;

		// Sidecars must always be relative to the manifest directory.  This is
		// deliberately lexical: callers resolve the accepted path below the .jam
		// directory and never treat VST compatibility paths as sidecar paths.
		static bool IsSafeSidecarPath(const std::string& path) noexcept;
		static std::optional<std::uint64_t> ParseStrictUint64(const std::string& text) noexcept;
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
			// Base64-encoded plugin state blob (from IVstPlugin::GetState).
			// Self-describing per plugin type (VST2 param/chunk blob, or
			// VST3 component/controller blob — see Vst3StateBlob.h).
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
			std::string Id;
			unsigned int Channel = 0;
			unsigned long Length;
			unsigned long Index;
			unsigned long MasterLoopCount;
			// Current-schema body phase. Index is retained only for legacy readers.
			unsigned long BodyPlayIndex = 0;
			double Level;
			double Speed;
			unsigned int MuteGroups;
			unsigned int SelectGroups;
			bool Muted;
			LoopMix Mix;
			std::vector<VstEntry> VstChain;

			static std::optional<Loop> FromJson(Json::JsonPart json);
		};

		struct AutomationPoint
		{
			double Fraction = 0.0;
			double Value = 0.0;
		};

		struct AutomationLane
		{
			enum class MappingType : std::uint8_t { Cc, Editor };
			MappingType Mapping = MappingType::Cc;
			std::uint8_t Channel = 0;
			std::uint8_t Controller = 0;
			std::string TargetScope = "station";
			unsigned int TargetPluginIndex = 0;
			unsigned int TargetLoopIndex = 0;
			unsigned int TargetParameterIndex = 0;
			std::vector<AutomationPoint> Points;
		};

		// Metadata in the manifest for a native .jammidi sidecar. Event and lane
		// payloads live in NativeMidiSidecar so a corrupt asset can skip this stream
		// without invalidating other takes.
		struct MidiStream
		{
			std::string SidecarPath;
			unsigned int Channel = 0;
			std::string Device;
			unsigned long LogicalLength = 0;
			std::uint64_t AutomationGlobalSampleOrigin = 0;
		};

		struct MidiRoute
		{
			// OutputIndex is the flattened take/stream index. Live routes use IsLive.
			unsigned int OutputIndex = 0;
			bool IsLive = false;
			unsigned int PluginIndex = 0;
		};

		struct LoopTake
		{
			std::string Name;
			std::vector<Loop> Loops;
			std::vector<VstEntry> VstChain;
			bool MidiQuantEnabled = false;
			int MidiQuantFraction = static_cast<int>(midi::MidiQuantisationFraction::Quarter);
			std::int32_t TakePhaseOffsetSamps = 0;
			unsigned long MidiPlayIndex = 0;
			unsigned long MidiPlayLength = 0;
			std::uint64_t MidiQuantTransportStart = 0;
			std::vector<MidiStream> MidiStreams;

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
			std::vector<MidiRoute> MidiRoutes;

			static std::optional<Station> FromJson(Json::JsonPart json);
		};

		Version Version = VERSION_V;
		unsigned int FormatMajor = CurrentFormatMajor;
		unsigned int FormatMinor = CurrentFormatMinor;
		unsigned int FormatPatch = CurrentFormatPatch;
		std::string Name;
		std::optional<NinjamConfig> Ninjam;
		std::vector<Station> Stations;
		unsigned long TimerTicks = 0;
		unsigned int QuantiseSamps = 1;
		GlobalMidiQuantState GlobalMidiQuantStateValue = GlobalMidiQuantState::Off;
		std::int32_t GlobalPhaseOffsetSamps = 0;
		double TransportOffsetLoopFrac = 0.0;
		utils::Timer::QuantisationType Quantisation = utils::Timer::QUANTISE_OFF;
		// Absolute local master sample coordinate. It is independent of TimerTicks,
		// which remains legacy compatibility metadata only.
		unsigned long MasterLengthSamps = 1;
		std::uint64_t AbsoluteSamplePos = 0;
	};
}
