#include "stdafx.h"
#include "AudioEngine.h"
#include "../engine/Timer.h"
#include <iostream>

namespace engine
{
	AudioEngine::AudioEngine(io::UserConfig userConfig) :
		_userConfig(userConfig),
		_channelMixer(std::make_shared<audio::ChannelMixer>(audio::ChannelMixerParams{}))
	{
	}

	AudioEngine::~AudioEngine()
	{
		Close();
	}

	bool AudioEngine::Init(std::shared_ptr<ninjam::NinjamController> ninjamController, TickCallback tickCallback)
	{
		std::scoped_lock lock(_audioMutex);

		_ninjamController = ninjamController;
		_tickCallback = tickCallback;

		auto dev = audio::AudioDevice::Open(AudioEngine::AudioCallback,
			[](RtAudioError::Type type, const std::string& err) { std::cout << "[" << type << " RtAudio Error] " << err << std::endl; },
			_userConfig.Audio,
			this);

		if (dev.has_value())
		{
			_audioDevice = std::move(dev.value());
			_audioSampleCounter.store(0u, std::memory_order_release);

			auto audioStreamParams = _audioDevice->GetAudioStreamParams();

			auto inLatency = (0u == audioStreamParams.InputLatency) ?
				_userConfig.Audio.LatencyIn :
				audioStreamParams.InputLatency;

			_channelMixer->SetParams(audio::ChannelMixerParams({
					_userConfig.AdcBufferDelay(inLatency) + audioStreamParams.BufSize,
					audio::ChannelMixer::DefaultBufferSize,
					audioStreamParams.NumInputChannels,
					audioStreamParams.NumOutputChannels }));

			auto stationsSnapshot = _audioStations.load(std::memory_order_acquire);
			if (stationsSnapshot)
			{
				for (auto& station : *stationsSnapshot)
				{
					if (station)
					{
						station->SetupBuffers(audioStreamParams.BufSize);
						station->SetSampleRate(static_cast<float>(audioStreamParams.SampleRate));
						station->SetNumAdcChannels(audioStreamParams.NumInputChannels);
						station->SetNumDacChannels(audioStreamParams.NumOutputChannels);
					}
				}
			}

			if (_ninjamController)
			{
				_ninjamController->SetAudioFormat(
					audioStreamParams.SampleRate,
					audioStreamParams.BufSize,
					audioStreamParams.NumInputChannels,
					audioStreamParams.NumOutputChannels);
			}

			_audioDevice->Start();
			_audioDevice->GetAudioStreamParams().PrintParams();
			return true;
		}
		return false;
	}

	void AudioEngine::Close()
	{
		if (_audioDevice)
			_audioDevice->Stop();

		std::scoped_lock lock(_audioMutex);

		if (_ninjamController)
			_ninjamController->Stop();

		if (_audioDevice)
			_audioDevice->Stop();
	}

	void AudioEngine::SetStations(std::shared_ptr<const std::vector<std::shared_ptr<Station>>> stations)
	{
		_audioStations.store(stations, std::memory_order_release);
	}

	int AudioEngine::AudioCallback(void* outBuffer,
		void* inBuffer,
		unsigned int numSamps,
		double streamTime,
		RtAudioStreamStatus status,
		void* userData)
	{
		AudioEngine* engine = (AudioEngine*)userData;
		engine->_OnAudio((float*)inBuffer, (float*)outBuffer, numSamps, streamTime);
		return 0;
	}

	void AudioEngine::_OnAudio(float* inBuf,
		float* outBuf,
		unsigned int numSamps,
		double streamTime)
	{
		const auto audioStreamParams = nullptr == _audioDevice ?
			audio::AudioStreamParams() : _audioDevice->GetAudioStreamParams();
		const auto blockStartSample = _audioSampleCounter.load(std::memory_order_relaxed);
		const auto stationsSnapshot = _audioStations.load(std::memory_order_acquire);
		static const std::vector<std::shared_ptr<Station>> emptyStations;
		const auto& stations = stationsSnapshot ? *stationsSnapshot : emptyStations;

		if (nullptr != inBuf)
		{
			auto inLatency = (0u == audioStreamParams.InputLatency) ?
				_userConfig.Audio.LatencyIn :
				audioStreamParams.InputLatency;

			_channelMixer->FromAdc(inBuf, audioStreamParams.NumInputChannels, numSamps);

			_channelMixer->InitPlay(0u, numSamps);
			_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_MONITOR);

			for (auto& station : stations)
			{
				if (station->IsRemote())
					continue;

				_channelMixer->WriteToSink(station, numSamps);
			}

			_channelMixer->InitPlay(_userConfig.AdcBufferDelay(inLatency), numSamps);
			_channelMixer->Source()->SetSourceType(Audible::AUDIOSOURCE_ADC);

			for (auto& station : stations)
			{
				if (station->IsRemote())
					continue;

				_channelMixer->WriteToSink(station, numSamps);

				station->SetSourceType(Audible::AUDIOSOURCE_MONITOR);
				station->OnBounce(numSamps, _userConfig, audioStreamParams);

				station->SetSourceType(Audible::AUDIOSOURCE_BOUNCE);
				station->OnBounce(numSamps, _userConfig, audioStreamParams);

				station->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_BOUNCE);
			}
		}

		_channelMixer->Source()->EndMultiPlay(numSamps);

		_channelMixer->Sink()->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
		
		if (_ninjamController)
			_ninjamController->ProcessAudioBlock(inBuf, numSamps, audioStreamParams.SampleRate);

		auto ingestRemoteStation = [&](const std::shared_ptr<Station>& stationBase) {
			auto station = std::dynamic_pointer_cast<StationRemote>(stationBase);
			if (!station || !station->IsConnectedRemote())
				return;

			const float* left = nullptr;
			const float* right = nullptr;
			unsigned int frameCount = 0u;
			if (_ninjamController && _ninjamController->ConsumeStereoPair(station->AssignedOutputChannel(), left, right, frameCount))
			{
				auto ingestFrames = frameCount < numSamps ? frameCount : numSamps;
				station->IngestStereoBlock(left, right, ingestFrames);
			}
		};

		if (nullptr != outBuf)
		{
			std::fill(outBuf, outBuf + numSamps * audioStreamParams.NumOutputChannels, 0.0f);

			for (auto& station : stations)
			{
				station->Zero(numSamps, Audible::AUDIOSOURCE_LOOPS);
				ingestRemoteStation(station);
				station->WriteBlock(_channelMixer->Sink(), nullptr, 0, numSamps,
					static_cast<std::uint32_t>(blockStartSample));
				station->EndMultiPlay(numSamps);
			}

			_channelMixer->ToDac(outBuf, audioStreamParams.NumOutputChannels, numSamps);
		}
		else
		{
			for (auto& station : stations)
			{
				ingestRemoteStation(station);
				station->WriteBlock(_channelMixer->Sink(), nullptr, 0, numSamps,
					static_cast<std::uint32_t>(blockStartSample));
				station->EndMultiPlay(numSamps);
			}
		}

		_channelMixer->Sink()->EndMultiWrite(numSamps, true, Audible::AUDIOSOURCE_LOOPS);

		if (_tickCallback)
		{
			_tickCallback(Timer::GetTime(), numSamps, _userConfig, audioStreamParams);
		}

		_audioSampleCounter.store(blockStartSample + numSamps, std::memory_order_release);
		_midiAnchorMicros.store(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);
	}
}
