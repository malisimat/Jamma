#pragma once

#include <iostream>
#include <string>
#include <any>
#include <algorithm>
#include <memory>
#include <optional>
#include <functional>
#include <vector>
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

	struct AsioDeviceInfo
	{
		unsigned int Id = 0;
		std::string Name;
		bool Probed = false;
		std::string ProbeError;
		unsigned int InputChannels = 0;
		unsigned int OutputChannels = 0;
		std::vector<unsigned int> SampleRates;
		unsigned int PreferredSampleRate = 0;
		bool DefaultInput = false;
		bool DefaultOutput = false;
	};

	struct AsioInventory
	{
		std::vector<AsioDeviceInfo> Devices;
		std::optional<unsigned int> DefaultInputId;
		std::optional<unsigned int> DefaultOutputId;
		std::string EnumerationError;
	};

	struct AsioAttempt
	{
		unsigned int DeviceId = 0;
		std::string Name;
		unsigned int InputChannels = 0;
		unsigned int OutputChannels = 0;
		unsigned int SampleRate = 0;
		unsigned int BufferSize = 0;
		unsigned int NumBuffers = 0;
		bool Opened = false;
		bool Started = false;
		std::string Error;
	};

	struct AsioOpenReport
	{
		std::vector<AsioAttempt> Attempts;
		std::optional<AudioStreamParams> StartedParams;
	};

	struct AsioConfiguration
	{
		unsigned int InputChannels;
		unsigned int OutputChannels;
		unsigned int SampleRate;
	};

	class AudioDevice
	{
	private:
		enum class StreamState
		{
			CLOSED,
			STOPPED,
			RUNNING,
			PAUSED
		};

	public:
		AudioDevice();
		AudioDevice(AudioStreamParams audioStreamParams,
			std::unique_ptr<RtAudio> stream);
		~AudioDevice();

	public:
		void SetDevice(std::unique_ptr<RtAudio> device);
		bool Start();
		void Stop();
		bool TryStop();
		bool Pause();
		bool Resume();
		AudioStreamParams GetAudioStreamParams();

	private:
		AudioStreamParams _audioStreamParams;
		std::unique_ptr<RtAudio> _stream;
		StreamState _streamState;

	public:
		static std::optional<std::unique_ptr<AudioDevice>> Open(
			std::function<int(void*, void*, unsigned int, double, RtAudioStreamStatus, void*)> onAudio,
			std::function<void(RtAudioError::Type, const std::string&)> onError,
			io::UserConfig::AudioSettings audioSettings,
			void* AudioSink,
			bool generatedRig = false,
			const AsioInventory* inventory = nullptr,
			AsioOpenReport* report = nullptr,
			const std::function<void(const AudioStreamParams&)>& prepareForStart = {});
		static AsioInventory DiscoverAsio();
		static std::vector<unsigned int> OrderedCandidateIds(const AsioInventory& inventory,
			const io::UserConfig::AudioSettings& settings, bool generatedRig);
		static std::vector<unsigned int> OrderedRates(const std::vector<unsigned int>& rates, unsigned int preferred);
		static std::vector<std::pair<unsigned int, unsigned int>> GeneratedChannelPairs(
			unsigned int inputChannels, unsigned int outputChannels);
		static std::vector<AsioConfiguration> GeneratedAttemptPlan(
			unsigned int inputChannels, unsigned int outputChannels,
			const std::vector<unsigned int>& rates, bool stereoPass,
			std::size_t maxAttempts = 2048u);

	};
}
