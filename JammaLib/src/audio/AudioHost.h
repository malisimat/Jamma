#pragma once

#include <array>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include "AudioDevice.h"
#include "ChannelMixer.h"
#include "NinjamMetronome.h"
#include "../io/UserConfig.h"
#include "../engine/Station.h"
#include "../engine/StationRemote.h"
#include "../midi/MidiClockAnchor.h"
#include "../ninjam/NinjamController.h"
#include "../ninjam/NinjamTimingObservationMailbox.h"
#include "../ninjam/NinjamAudioTimingCommand.h"
#include "../utils/Timer.h"

namespace audio
{
	class AudioHost
	{
	public:
		using TickCallback = std::function<void(Time streamTime, unsigned int numSamps,
			const std::optional<io::UserConfig>& cfg,
			const std::optional<AudioStreamParams>& params)>;

		AudioHost(io::UserConfig userConfig);
		~AudioHost();

		bool Init(std::shared_ptr<ninjam::NinjamController> ninjamController, 
				  TickCallback tickCallback);
		void Close();

		void SetStations(std::shared_ptr<const std::vector<std::shared_ptr<engine::Station>>> stations);

		std::shared_ptr<const std::vector<std::shared_ptr<engine::Station>>> GetStationsSnapshot() const { return _audioStations.load(std::memory_order_acquire); }
		std::uint64_t GetAudioSampleCounter() const { return _audioSampleCounter.load(std::memory_order_relaxed); }
		const midi::MidiClockAnchor& GetMidiClockAnchor() const { return _midiClockAnchor; }
		midi::MidiClockAnchor& GetMidiClockAnchor_Ref() { return _midiClockAnchor; }
		const io::UserConfig& GetUserConfig() const { return _userConfig; }

		AudioStreamParams GetStreamParams() const 
		{ 
			return _audioDevice ? _audioDevice->GetAudioStreamParams() : AudioStreamParams(); 
		}
		
		AudioDevice* GetDevice() const { return _audioDevice.get(); }
		
		std::shared_ptr<ChannelMixer> GetChannelMixer() { return _channelMixer; }
		void SetNinjamMetronomeEnabled(bool enabled) noexcept { _ninjamMetronomeEnabled.store(enabled, std::memory_order_release); }
		bool NinjamMetronomeEnabled() const noexcept { return _ninjamMetronomeEnabled.load(std::memory_order_acquire); }
		std::optional<ninjam::NinjamTiming> LatestNinjamTiming() const noexcept
		{
			return _ninjamTimingMailbox.ReadLatest();
		}
		// Job thread publishes one coherent transport command; the audio callback
		// consumes it once at the top of the block and applies it to the Timer and
		// every active local take together.
		void PublishTimingCommand(const ninjam::NinjamAudioTimingCommand& command) noexcept
		{
			_ninjamTimingCommandMailbox.Publish(command);
		}
		// Shares the master transport clock so the audio callback can apply unified
		// timing commands to it at the same boundary as the local takes.
		void SetTimingClock(std::shared_ptr<utils::Timer> clock) noexcept
		{
			_timingClock.store(std::move(clock), std::memory_order_release);
		}
		float GetAdcPeak(unsigned int channel) const noexcept
		{
			return channel < _AdcPeakChannels ? _adcPeaks[channel].load(std::memory_order_relaxed) : 0.0f;
		}

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
		std::optional<io::UserConfig> _tickUserConfig;
		std::optional<AudioStreamParams> _tickStreamParams;
		std::mutex _audioMutex;
		std::unique_ptr<AudioDevice> _audioDevice;
		std::shared_ptr<ChannelMixer> _channelMixer;
		NinjamMetronome _ninjamMetronome;
		ninjam::NinjamMetronomeTimingState _ninjamMetronomeTimingState;
		ninjam::NinjamTimingObservationMailbox _ninjamTimingMailbox;
		ninjam::NinjamAudioTimingCommandMailbox _ninjamTimingCommandMailbox;
		std::atomic<std::shared_ptr<utils::Timer>> _timingClock;
		std::uint64_t _ninjamTimingObservationSequence = 0u;
		std::atomic_bool _ninjamMetronomeEnabled{ true };

		std::atomic<std::uint64_t> _audioSampleCounter{ 0 };
		midi::MidiClockAnchor _midiClockAnchor;
		static constexpr unsigned int _AdcPeakChannels = 32u;
		std::array<std::atomic<float>, _AdcPeakChannels> _adcPeaks{};

		std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<engine::Station>>>> _audioStations;
		std::shared_ptr<ninjam::NinjamController> _ninjamController;
		TickCallback _tickCallback;
	};
}
