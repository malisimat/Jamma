///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <vector>
#include <cstdint>
#include <stack>
#include <map>
#include <optional>
#include <variant>
#include <iostream>
#include <sstream>
#include "Json.h"
#include "../../include/Constants.h"
#include "../utils/MathUtils.h"

namespace io
{
	struct UserConfig
	{
		static std::optional<UserConfig> FromJson(Json::JsonPart json);
		bool operator==(const UserConfig&) const = default;

		struct SeedLoopTiming
		{
			unsigned int GrainSamps = 0u;
			unsigned int LoopGrains = 0u;
			float Bpm = 0.0f;
			unsigned int Bpi = 0u;
		};

		struct AudioSettings
		{
			std::string Name; // The name of the device (used for matching rig preferences)
			unsigned int SampleRate = constants::DefaultSampleRate; // The sample rate of the stream, in Hz
			unsigned int BufSize = 0u; // The buffer size used by the device
			unsigned int LatencyIn = 0u; // The input latency, in samples
			unsigned int LatencyOut = 0u; // The output latency, in samples
			unsigned int NumBuffers = 0u; // The number of buffers used by the device, if applicable
			unsigned int NumChannelsIn = 0u; // The number of input channels used in current scene
			unsigned int NumChannelsOut = 0u; // The number of output channels used in current scene

			static std::optional<AudioSettings> FromJson(Json::JsonPart json);
			bool operator==(const AudioSettings&) const = default;
		};

		struct LoopSettings
		{
			unsigned int FadeSamps = constants::DefaultFadeSamps; // The number of samples to fade in/out the start/end of a loop
			unsigned int MidiSeamMergeWindowMs = 30u; // Forward boundary-held note tolerance, 0..200 ms
			unsigned int SeedGrainMinMs = constants::DefaultSeedGrainMinMs; // Never halve below this grain length
			unsigned int SeedGrainTargetMaxMs = constants::DefaultSeedGrainTargetMaxMs; // Prefer grains below this duration
			unsigned int SeedBpmMin = constants::DefaultSeedBpmMin; // Choose beats-per-grain so BPM is at least this value
			bool SeedUsesPowers = true; // true => 1x,2x,4x... ; false => 1x,2x,3x...

			static std::optional<LoopSettings> FromJson(Json::JsonPart json);
			bool operator==(const LoopSettings&) const = default;
		};

		struct TriggerSettings
		{
			unsigned int PreDelaySamps = constants::DefaultPreDelaySamps; // Deprecated source member: only zero is supported; never used for timing
			unsigned int DebounceMs = constants::DefaultDebounceMs; // Input bounce threshold, in milliseconds

			static std::optional<TriggerSettings> FromJson(Json::JsonPart json);
			bool operator==(const TriggerSettings&) const = default;
		};

		struct MidiSettings
		{
			std::string Name = "default"; // Hardware selector; default selects the first unclaimed port
			bool Enabled = true;
			std::string Id; // Stable registry reference used by controls/capture and recorded streams

			static std::optional<MidiSettings> FromJson(Json::JsonPart json);
			bool operator==(const MidiSettings&) const = default;
		};

		struct MidiConfig
		{
			std::vector<MidiSettings> Devices;
			bool ChannelOverrideTriggers = false;
			bool ChannelOverrideLive = true;

			static std::optional<MidiConfig> FromJson(Json::JsonPart json);
			bool operator==(const MidiConfig&) const = default;
		};

		struct SerialSettings
		{
			std::string Name = "default";
			std::string Port = "COM3";
			unsigned int BaudRate = 115200u;
			bool Enabled = false;

			static std::optional<SerialSettings> FromJson(Json::JsonPart json);
			bool operator==(const SerialSettings&) const = default;
		};

		struct SerialConfig
		{
			std::vector<SerialSettings> Devices;

			static std::optional<SerialConfig> FromJson(Json::JsonPart json);
			bool operator==(const SerialConfig&) const = default;
		};

		// How much to (further) delay input signal from ADC, in samples
		unsigned int AdcBufferDelay(unsigned int inLatency, unsigned int sampleRate = 0u) const;
		static std::optional<std::int32_t> TriggerPreDelaySamples(double milliseconds, unsigned int sampleRate) noexcept;
		static std::optional<unsigned int> TriggerHistorySamples(unsigned int sampleRate) noexcept;

		// How long to continue recording after trigger to end loop recording, in samples
		unsigned int EndRecordingSamps(int error) const;

		// The index at which to start playing a loop after trigger to end recording,
		// in samples (includes intro, so zero is first index of fade-in, not the loop)
		unsigned long LoopPlayPos(int error,
			unsigned long loopLength,
			unsigned int outLatency) const;
		unsigned int TriggerLoopAlignmentSamps(unsigned int sampleRate = 0u) const;
		long OverdubSourceReadOffset(unsigned int outLatency, unsigned int sampleRate = 0u) const;
		unsigned long OverdubPlayPos(int error, unsigned long loopLength) const;
		std::optional<SeedLoopTiming> DeduceLoopTiming(unsigned long loopLengthSamps,
			unsigned int sampleRate) const;

		AudioSettings Audio;
		LoopSettings Loop;
		TriggerSettings Trigger;
		MidiConfig Midi;
		SerialConfig Serial;
	};
}
