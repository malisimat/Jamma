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
#include "../include/Constants.h"
#include "../utils/MathUtils.h"

namespace io
{
	struct UserConfig
	{
		static std::optional<UserConfig> FromJson(Json::JsonPart json);

		struct SeedLoopTiming
		{
			unsigned int GrainSamps = 0u;
			unsigned int LoopGrains = 0u;
			unsigned int BeatsPerGrain = 0u;
			float Bpm = 0.0f;
			unsigned int Bpi = 0u;
		};

		struct AudioSettings
		{
			std::string Name; // The name of the device (used for matching rig preferences)
			unsigned int SampleRate; // The sample rate of the stream, in Hz
			unsigned int BufSize; // The buffer size used by the device
			unsigned int LatencyIn; // The input latency, in samples
			unsigned int LatencyOut; // The output latency, in samples
			unsigned int NumBuffers; // The number of buffers used by the device, if applicable
			unsigned int NumChannelsIn; // The number of input channels used in current scene
			unsigned int NumChannelsOut; // The number of output channels used in current scene

			static std::optional<AudioSettings> FromJson(Json::JsonPart json);
		};

		struct LoopSettings
		{
			unsigned int FadeSamps = constants::DefaultFadeSamps; // The number of samples to fade in/out the start/end of a loop
			unsigned int SeedGrainMinMs = 400u; // Never halve below this grain length
			unsigned int SeedGrainTargetMaxMs = 3000u; // Prefer grains below this duration
			unsigned int SeedBpmMin = 80u; // Choose beats-per-grain so BPM is at least this value
			bool SeedUsesPowers = true; // true => 1x,2x,4x... ; false => 1x,2x,3x...

			static std::optional<LoopSettings> FromJson(Json::JsonPart json);
		};

		struct TriggerSettings
		{
			unsigned int PreDelay; // How many samples to push trigger point to left (positive is earlier trig)
			unsigned int DebounceSamps; // How many samples over which to prevent trigger bounce

			static std::optional<TriggerSettings> FromJson(Json::JsonPart json);
		};

		// How much to (further) delay input signal from ADC, in samples
		unsigned int AdcBufferDelay(unsigned int inLatency) const;

		// How long to continue recording after trigger to end loop recording, in samples
		unsigned int EndRecordingSamps(int error) const;

		// The index at which to start playing a loop after trigger to end recording,
		// in samples (includes intro, so zero is first index of fade-in, not the loop)
		unsigned long LoopPlayPos(int error,
			unsigned long loopLength,
			unsigned int outLatency) const;
		std::optional<SeedLoopTiming> DeduceLoopTiming(unsigned long loopLengthSamps,
			unsigned int sampleRate) const;

		AudioSettings Audio;
		LoopSettings Loop;
		TriggerSettings Trigger;
	};
}
