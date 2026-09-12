#include "AudioDevice.h"

using namespace audio;


void AudioStreamParams::PrintParams()
{
	std::cout << "Name : " << Name << std::endl; // The name of the device used for the stream
	std::cout << "SampleRate : " << SampleRate << std::endl; // The actual sample rate of the stream, in Hz
	std::cout << "NumBuffers : " << NumBuffers << std::endl; // The buffer size used by the device
	std::cout << "BufSize : " << BufSize << std::endl; // The buffer size used by the device
	std::cout << "InputLatency : " << InputLatency << std::endl; // The input latency reported by the device, in samples
	std::cout << "OutputLatency : " << OutputLatency << std::endl; // The output latency reported by the device, in samples
	std::cout << "NumInputChannels : " << NumInputChannels << std::endl; // The number of input channels used in the stream
	std::cout << "NumOutputChannels : " << NumOutputChannels << std::endl; // The number of output channels used in the stream
}

AudioDevice::AudioDevice() :
	_audioStreamParams(),
	_stream(std::unique_ptr<RtAudio>()),
	_streamState(StreamState::CLOSED)
{
}

AudioDevice::AudioDevice(AudioStreamParams audioStreamParams,
	std::unique_ptr<RtAudio> stream) :
	_audioStreamParams(audioStreamParams),
	_stream(std::move(stream)),
	_streamState(StreamState::CLOSED)
{
	if (_stream)
	{
		if (_stream->isStreamRunning())
			_streamState = StreamState::RUNNING;
		else if (_stream->isStreamOpen())
			_streamState = StreamState::STOPPED;
	}
}

AudioDevice::~AudioDevice()
{
	if (!_stream)
		return;

	Stop();
}

void AudioDevice::SetDevice(std::unique_ptr<RtAudio> device)
{
	_stream = std::move(device);
	_streamState = StreamState::CLOSED;

	if (_stream)
	{
		if (_stream->isStreamRunning())
			_streamState = StreamState::RUNNING;
		else if (_stream->isStreamOpen())
			_streamState = StreamState::STOPPED;
	}
}

void AudioDevice::Start()
{
	if (_stream)
	{
		_stream->startStream();
		_streamState = StreamState::RUNNING;
		_audioStreamParams.InputLatency = (unsigned int)_stream->getInputStreamLatency();
		_audioStreamParams.OutputLatency = (unsigned int)_stream->getOutputStreamLatency();
	}
}

void AudioDevice::Stop()
{
	if (_stream)
	{
		if (_stream->isStreamRunning())
			_stream->stopStream();
		_streamState = StreamState::STOPPED;

		if (_stream->isStreamOpen())
			_stream->closeStream();

		_streamState = StreamState::CLOSED;
	}
}

bool AudioDevice::Pause()
{
	if (!_stream)
		return false;

	try
	{
		if (!_stream->isStreamRunning())
			return false;

		_stream->stopStream();
		_streamState = StreamState::PAUSED;
		return true;
	}
	catch (RtAudioError& err)
	{
		std::cout << "Error pausing audio stream: " << err.getMessage() << std::endl;
		return false;
	}
}

bool AudioDevice::Resume()
{
	if (!_stream)
		return false;

	if (_streamState != StreamState::PAUSED)
		return false;

	try
	{
		if (_stream->isStreamOpen() && !_stream->isStreamRunning())
		{
			_stream->startStream();
			_audioStreamParams.InputLatency = (unsigned int)_stream->getInputStreamLatency();
			_audioStreamParams.OutputLatency = (unsigned int)_stream->getOutputStreamLatency();
		}
		_streamState = StreamState::RUNNING;
		return true;
	}
	catch (RtAudioError& err)
	{
		std::cout << "Error resuming audio stream: " << err.getMessage() << std::endl;
		return false;
	}
}

AudioStreamParams AudioDevice::GetAudioStreamParams()
{
	return _audioStreamParams;
}

std::optional<std::unique_ptr<AudioDevice>> AudioDevice::Open(
	std::function<int(void*,void*,unsigned int,double,RtAudioStreamStatus,void*)> onAudio,
	std::function<void(RtAudioError::Type,const std::string&)> onError,
	io::UserConfig::AudioSettings audioSettings,
	void* AudioSink)
{
	std::unique_ptr<RtAudio> discoveryAudio;
	try
	{
		discoveryAudio = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO);
	}
	catch (RtAudioError& err)
	{
		std::cout << "Error instantiating Audio API: " << err.getMessage() << std::endl;
		return std::nullopt;
	}

	auto candidates = ResolveAsioDeviceCandidates(*discoveryAudio, audioSettings);
	if (candidates.empty())
	{
		std::cout << "No usable ASIO devices were found." << std::endl;
		return std::nullopt;
	}

	for (const auto& candidate : candidates)
	{
		// An ASIO duplex stream must use one driver for both directions.
		const auto inputChannels = std::min(audioSettings.NumChannelsIn,
			candidate.Info.inputChannels);
		const auto outputChannels = std::min(audioSettings.NumChannelsOut,
			candidate.Info.outputChannels);
		if ((inputChannels == 0u) && (outputChannels == 0u))
		{
			std::cout << "Skipping ASIO device " << candidate.Id << " ("
				<< candidate.Info.name << "): no requested channels are available." << std::endl;
			continue;
		}

		if (candidate.Info.sampleRates.empty())
		{
			std::cout << "Skipping ASIO device " << candidate.Id << " ("
				<< candidate.Info.name << "): no supported sample rates were reported." << std::endl;
			continue;
		}

		io::UserConfig::AudioSettings candidateSettings = audioSettings;
		candidateSettings.NumChannelsIn = inputChannels;
		candidateSettings.NumChannelsOut = outputChannels;
		candidateSettings.SampleRate = FindClosest(candidate.Info.sampleRates,
			candidateSettings.SampleRate);

		RtAudio::StreamParameters inParams;
		inParams.deviceId = candidate.Id;
		inParams.firstChannel = 0;
		inParams.nChannels = inputChannels;

		RtAudio::StreamParameters outParams;
		outParams.deviceId = candidate.Id;
		outParams.firstChannel = 0;
		outParams.nChannels = outputChannels;

		RtAudio::StreamOptions streamOptions;
		streamOptions.numberOfBuffers = candidateSettings.NumBuffers;

		std::cout << "Opening ASIO device " << candidate.Id << " ("
			<< candidate.Info.name << ")" << std::endl;
		std::cout << "[Input] " << inParams.nChannels << "ch" << std::endl;
		std::cout << "[Output] " << outParams.nChannels << "ch" << std::endl;

		try
		{
			// A fresh RtAudio instance prevents a failed ASIO driver open from
			// contaminating the next fallback attempt.
			auto rtAudio = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO);
			rtAudio->openStream(outputChannels > 0u ? &outParams : nullptr,
				inputChannels > 0u ? &inParams : nullptr,
				RTAUDIO_FLOAT32,
				candidateSettings.SampleRate,
				&candidateSettings.BufSize,
				*onAudio.target<RtAudioCallback>(),
				AudioSink,
				&streamOptions,
				nullptr);
				//*onError.target<RtAudioErrorCallback>());

			if (!rtAudio->isStreamOpen())
			{
				std::cout << "ASIO device " << candidate.Id << " did not open a stream." << std::endl;
				continue;
			}

			AudioStreamParams audioStreamParams;
			audioStreamParams.Name = candidate.Info.name;
			audioStreamParams.SampleRate = candidateSettings.SampleRate;
			audioStreamParams.BufSize = candidateSettings.BufSize;
			audioStreamParams.NumBuffers = streamOptions.numberOfBuffers;
			audioStreamParams.InputLatency = (unsigned int)rtAudio->getInputStreamLatency();
			audioStreamParams.OutputLatency = (unsigned int)rtAudio->getOutputStreamLatency();
			audioStreamParams.NumInputChannels = inputChannels;
			audioStreamParams.NumOutputChannels = outputChannels;

			return std::make_unique<AudioDevice>(audioStreamParams, std::move(rtAudio));
		}
		catch (RtAudioError& err)
		{
			std::cout << "Error opening ASIO device " << candidate.Id << " ("
				<< candidate.Info.name << "): " << err.getMessage() << std::endl;
		}
	}

	std::cout << "Unable to open any available ASIO device." << std::endl;
	return std::nullopt;
}

std::vector<AudioDevice::AsioDeviceCandidate> AudioDevice::ResolveAsioDeviceCandidates(
	RtAudio& rtAudio,
	const io::UserConfig::AudioSettings& audioSettings)
{
	std::vector<AsioDeviceCandidate> devices;
	try
	{
		const auto deviceCount = rtAudio.getDeviceCount();
		for (auto deviceId = 0u; deviceId < deviceCount; deviceId++)
		{
			try
			{
				auto info = rtAudio.getDeviceInfo(deviceId);
				if (info.probed)
					devices.push_back({ deviceId, std::move(info) });
				else
					std::cout << "Skipping unprobed ASIO device " << deviceId << std::endl;
			}
			catch (RtAudioError& err)
			{
				std::cout << "Unable to probe ASIO device " << deviceId << ": "
					<< err.getMessage() << std::endl;
			}
		}
	}
	catch (RtAudioError& err)
	{
		std::cout << "Unable to enumerate ASIO devices: " << err.getMessage() << std::endl;
		return {};
	}

	std::vector<AsioDeviceCandidate> candidates;
	auto appendCandidate = [&candidates, &devices](unsigned int deviceId)
	{
		const auto device = std::find_if(devices.begin(), devices.end(),
			[deviceId](const AsioDeviceCandidate& candidate) { return candidate.Id == deviceId; });
		const auto alreadyAdded = std::any_of(candidates.begin(), candidates.end(),
			[deviceId](const AsioDeviceCandidate& candidate) { return candidate.Id == deviceId; });
		if ((device != devices.end()) && !alreadyAdded)
			candidates.push_back(*device);
	};

	if (!audioSettings.Name.empty())
	{
		for (const auto& device : devices)
		{
			if (device.Info.name == audioSettings.Name)
				appendCandidate(device.Id);
		}
	}

	try
	{
		const auto defaultInput = rtAudio.getDefaultInputDevice();
		const auto defaultOutput = rtAudio.getDefaultOutputDevice();
		if (audioSettings.NumChannelsOut > 0u)
		{
			appendCandidate(defaultOutput);
			appendCandidate(defaultInput);
		}
		else
		{
			appendCandidate(defaultInput);
			appendCandidate(defaultOutput);
		}
	}
	catch (RtAudioError& err)
	{
		std::cout << "Unable to resolve default ASIO device: " << err.getMessage() << std::endl;
	}

	for (const auto& device : devices)
	{
		const auto supportsRequestedDirections =
			((audioSettings.NumChannelsIn == 0u) || (device.Info.inputChannels > 0u)) &&
			((audioSettings.NumChannelsOut == 0u) || (device.Info.outputChannels > 0u));
		if (supportsRequestedDirections)
			appendCandidate(device.Id);
	}

	for (const auto& device : devices)
		appendCandidate(device.Id);

	return candidates;
}

unsigned int AudioDevice::FindClosest(const std::vector<unsigned int>& vec,
	unsigned int target)
{
	if (vec.empty()) {
		throw std::invalid_argument("The vector is empty");
	}

	unsigned int closest = vec[0];
	for (unsigned int num : vec) {
		if (std::abs(static_cast<int>(num) - static_cast<int>(target)) <
			std::abs(static_cast<int>(closest) - static_cast<int>(target))) {
			closest = num;
		}
	}

	return closest;
}
