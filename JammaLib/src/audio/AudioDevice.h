#pragma once

#include <iostream>
#include <string>
#include <any>
#include <algorithm>
#include <memory>
#include <optional>
#include <functional>
#include "../base/AudioSource.h"
#include "../io/UserConfig.h"
#include "rtaudio/RtAudio.h"

namespace audio
{
	struct AudioStreamParams
	{
		std::string Name; // The actual name of the device
		unsigned int SampleRate; // The actual sample rate of the stream, in Hz
		unsigned int NumBuffers; // The buffer size used by the device
		unsigned int BufSize; // The buffer size used by the device
		unsigned int InputLatency; // The input latency reported by the device, in samples
		unsigned int OutputLatency; // The output latency reported by the device, in samples
		unsigned int NumInputChannels; // The number of input channels for the stream
		unsigned int NumOutputChannels; // The number of output channels for the stream

		void PrintParams();
	};

	class AudioDevice
	{
	public:
		AudioDevice();
		AudioDevice(AudioStreamParams audioStreamParams,
			std::unique_ptr<RtAudio> stream);
		~AudioDevice();

	public:
		void SetDevice(std::unique_ptr<RtAudio> device);
		void Start();
		void Stop();
		AudioStreamParams GetAudioStreamParams();

	private:
		AudioStreamParams _audioStreamParams;
		std::unique_ptr<RtAudio> _stream;

	public:
		static std::optional<std::unique_ptr<AudioDevice>> Open(
			std::function<int(void*, void*, unsigned int, double, RtAudioStreamStatus, void*)> onAudio,
			std::function<void(RtAudioError::Type, const std::string&)> onError,
			io::UserConfig::AudioSettings audioSettings,
			void* AudioSink);

	private:
		static unsigned int FindClosest(const std::vector<unsigned int>& vec, unsigned int target);
	};
}
