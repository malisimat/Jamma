#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include "../audio/AudioDevice.h"
#include "../audio/ChannelMixer.h"
#include "../io/UserConfig.h"
#include "Station.h"
#include "StationRemote.h"
#include "../ninjam/NinjamController.h"

#include "Timer.h"

namespace engine
{
	class Scene; // Forward declaration for callback fallback if needed, or better, use std::function

	class AudioEngine
	{
	public:
		using TickCallback = std::function<void(Time streamTime, unsigned int numSamps, const io::UserConfig& cfg, const audio::AudioStreamParams& params)>;

		AudioEngine(io::UserConfig userConfig);
		~AudioEngine();

		bool Init(std::shared_ptr<ninjam::NinjamController> ninjamController, 
				  TickCallback tickCallback);
		void Close();

		void SetStations(std::shared_ptr<const std::vector<std::shared_ptr<Station>>> stations);

		std::shared_ptr<const std::vector<std::shared_ptr<Station>>> GetStationsSnapshot() const { return _audioStations.load(std::memory_order_acquire); }
		std::uint64_t GetAudioSampleCounter() const { return _audioSampleCounter.load(std::memory_order_relaxed); }
		std::atomic<std::uint64_t>& GetAudioSampleCounter_Ref() { return _audioSampleCounter; }
		std::int64_t GetMidiAnchorMicros() const { return _midiAnchorMicros.load(std::memory_order_relaxed); }
		std::atomic<std::int64_t>& GetMidiAnchorMicros_Ref() { return _midiAnchorMicros; }
		const io::UserConfig& GetUserConfig() const { return _userConfig; }

		audio::AudioStreamParams GetStreamParams() const 
		{ 
			return _audioDevice ? _audioDevice->GetAudioStreamParams() : audio::AudioStreamParams(); 
		}
		
		audio::AudioDevice* GetDevice() const { return _audioDevice.get(); }
		
		std::shared_ptr<audio::ChannelMixer> GetChannelMixer() { return _channelMixer; }

	private:
		static int AudioCallback(void* outBuffer,
			void* inBuffer,
			unsigned int numSamps,
			double streamTime,
			RtAudioStreamStatus status,
			void* userData);

		void _OnAudio(float* inBuffer,
			float* outBuffer,
			unsigned int numSamps,
			double streamTime);

	private:
		io::UserConfig _userConfig;
		std::mutex _audioMutex;
		std::unique_ptr<audio::AudioDevice> _audioDevice;
		std::shared_ptr<audio::ChannelMixer> _channelMixer;

		std::atomic<std::uint64_t> _audioSampleCounter{ 0 };
		std::atomic<std::int64_t> _midiAnchorMicros{ 0 };

		std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<Station>>>> _audioStations;
		std::shared_ptr<ninjam::NinjamController> _ninjamController;
		TickCallback _tickCallback;
	};
}
