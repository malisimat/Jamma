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
	_stream(std::unique_ptr<RtAudio>())
{
}

AudioDevice::AudioDevice(AudioStreamParams audioStreamParams,
	std::unique_ptr<RtAudio> stream) :
	_audioStreamParams(audioStreamParams),
	_stream(std::move(stream))
{
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
}

void AudioDevice::Start()
{
	if (_stream)
	{
		_stream->startStream();
		_audioStreamParams.InputLatency = (unsigned int)_stream->getInputStreamLatency();
		_audioStreamParams.OutputLatency = (unsigned int)_stream->getOutputStreamLatency();
	}
}

void AudioDevice::Stop()
{
	if (_stream->isStreamRunning())
		_stream->stopStream();

	if (_stream->isStreamOpen())
		_stream->closeStream();
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
	std::unique_ptr<RtAudio> rtAudio;

	try
	{
		rtAudio = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO);
	}
	catch (RtAudioError& err)
	{
		std::cout << "Error instantiating Audio API: " << err.getMessage() << std::endl;
		return std::nullopt;
	}

	auto deviceCount = rtAudio->getDeviceCount();
	auto inDeviceNum = rtAudio->getDefaultInputDevice();
	auto outDeviceNum = rtAudio->getDefaultOutputDevice();
	auto inDev = rtAudio->getDeviceInfo(inDeviceNum);
	auto outDev = rtAudio->getDeviceInfo(outDeviceNum);

	if ((inDev.inputChannels == 0) && (outDev.outputChannels == 0))
		return std::nullopt;

	// Correct the audioSettings so they work
	audioSettings.NumChannelsIn = std::min(inDev.inputChannels, audioSettings.NumChannelsIn);
	audioSettings.NumChannelsOut = std::min(outDev.outputChannels, audioSettings.NumChannelsOut);
	audioSettings.SampleRate = FindClosest(inDev.sampleRates, audioSettings.SampleRate);

	RtAudio::StreamParameters inParams;
	inParams.deviceId = inDeviceNum;
	inParams.firstChannel = 0;
	inParams.nChannels = audioSettings.NumChannelsIn;

	RtAudio::StreamParameters outParams;
	outParams.deviceId = outDeviceNum;
	outParams.firstChannel = 0;
	outParams.nChannels = audioSettings.NumChannelsOut;

	RtAudio::StreamOptions streamOptions;
	streamOptions.numberOfBuffers = audioSettings.NumBuffers;
	//streamOptions.flags = RTAUDIO_MINIMIZE_LATENCY;

	AudioStreamParams audioStreamParams;

	std::cout << "Opening audio stream" << std::endl;
	std::cout << "[Input Device] " << inParams.deviceId << " : " << inParams.nChannels << "ch" << std::endl;
	std::cout << "[Output Device] " << outParams.deviceId << " : " << outParams.nChannels << "ch" << std::endl;

	try
	{
		rtAudio->openStream(outParams.nChannels > 0 ? &outParams : nullptr,
			inParams.nChannels > 0 ? &inParams : nullptr,
			RTAUDIO_FLOAT32,
			audioSettings.SampleRate,
			&audioSettings.BufSize,
			*onAudio.target<RtAudioCallback>(),
			(void*)AudioSink,
			&streamOptions,
			nullptr);
			//*onError.target<RtAudioErrorCallback>());
	}
	catch (RtAudioError& err)
	{
		std::cout << "Error opening audio stream: " << err.getMessage() << std::endl;
		return std::nullopt;
	}
	
	if (!rtAudio->isStreamOpen())
		return std::nullopt;

	audioSettings.NumBuffers = streamOptions.numberOfBuffers;
	audioStreamParams.Name = streamOptions.streamName;
	audioStreamParams.SampleRate = audioSettings.SampleRate;
	audioStreamParams.BufSize = audioSettings.BufSize;
	audioStreamParams.NumBuffers = streamOptions.numberOfBuffers;
	audioStreamParams.InputLatency = (unsigned int)rtAudio->getInputStreamLatency();
	audioStreamParams.OutputLatency = (unsigned int) rtAudio->getOutputStreamLatency();
	audioStreamParams.NumInputChannels = inParams.nChannels;
	audioStreamParams.NumOutputChannels = outParams.nChannels;

	return std::make_unique<AudioDevice>(audioStreamParams, std::move(rtAudio));
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