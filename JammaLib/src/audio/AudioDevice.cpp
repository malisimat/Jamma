#include "AudioDevice.h"
#include <cmath>
#include <limits>
#include <set>

using namespace audio;

void AudioStreamParams::PrintParams()
{
	std::cout << "[ASIO] running " << Name << " rate=" << SampleRate
		<< " input=" << NumInputChannels << " output=" << NumOutputChannels
		<< " buffer=" << BufSize << " buffers=" << NumBuffers
		<< " inputLatency=" << InputLatency << " outputLatency=" << OutputLatency << std::endl;
}

AudioDevice::AudioDevice() : _audioStreamParams(), _stream(), _streamState(StreamState::CLOSED) {}

AudioDevice::AudioDevice(AudioStreamParams params, std::unique_ptr<RtAudio> stream) :
	_audioStreamParams(std::move(params)), _stream(std::move(stream)), _streamState(StreamState::CLOSED)
{
	if (_stream)
		_streamState = _stream->isStreamRunning() ? StreamState::RUNNING :
			(_stream->isStreamOpen() ? StreamState::STOPPED : StreamState::CLOSED);
}

AudioDevice::~AudioDevice() { Stop(); }

void AudioDevice::SetDevice(std::unique_ptr<RtAudio> device)
{
	Stop();
	_stream = std::move(device);
	_streamState = _stream && _stream->isStreamRunning() ? StreamState::RUNNING :
		(_stream && _stream->isStreamOpen() ? StreamState::STOPPED : StreamState::CLOSED);
}

bool AudioDevice::Start()
{
	if (!_stream) return false;
	try
	{
		_stream->startStream();
		_streamState = StreamState::RUNNING;
		_audioStreamParams.InputLatency = static_cast<unsigned int>(_stream->getInputStreamLatency());
		_audioStreamParams.OutputLatency = static_cast<unsigned int>(_stream->getOutputStreamLatency());
		return true;
	}
	catch (const RtAudioError& err)
	{
		std::cout << "[ASIO] start failed: " << err.getMessage() << std::endl;
		_streamState = StreamState::STOPPED;
		return false;
	}
}

void AudioDevice::Stop()
{
	(void)TryStop();
}

bool AudioDevice::TryStop()
{
	if (!_stream) return true;
	try
	{
		if (_stream->isStreamRunning()) _stream->stopStream();
		if (_stream->isStreamOpen()) _stream->closeStream();
	}
	catch (const RtAudioError& err)
	{
		std::cout << "[ASIO] stop failed: " << err.getMessage() << std::endl;
		return false;
	}
	_streamState = StreamState::CLOSED;
	return true;
}

bool AudioDevice::Pause()
{
	if (!_stream || !_stream->isStreamRunning()) return false;
	try { _stream->stopStream(); _streamState = StreamState::PAUSED; return true; }
	catch (const RtAudioError& err) { std::cout << "[ASIO] pause failed: " << err.getMessage() << std::endl; return false; }
}

bool AudioDevice::Resume()
{
	if (!_stream || _streamState != StreamState::PAUSED) return false;
	return Start();
}

AudioStreamParams AudioDevice::GetAudioStreamParams() { return _audioStreamParams; }

AsioInventory AudioDevice::DiscoverAsio()
{
	AsioInventory inventory;
	try
	{
		RtAudio api(RtAudio::WINDOWS_ASIO);
		const auto count = api.getDeviceCount();
		try { inventory.DefaultInputId = api.getDefaultInputDevice(); }
		catch (const RtAudioError& err) { std::cout << "[ASIO] default input unavailable: " << err.getMessage() << std::endl; }
		try { inventory.DefaultOutputId = api.getDefaultOutputDevice(); }
		catch (const RtAudioError& err) { std::cout << "[ASIO] default output unavailable: " << err.getMessage() << std::endl; }
		for (auto id = 0u; id < count; ++id)
		{
			AsioDeviceInfo device;
			device.Id = id;
			device.DefaultInput = inventory.DefaultInputId == id;
			device.DefaultOutput = inventory.DefaultOutputId == id;
			try
			{
				const auto info = api.getDeviceInfo(id);
				device.Name = info.name;
				device.Probed = info.probed;
				device.InputChannels = info.inputChannels;
				device.OutputChannels = info.outputChannels;
				device.SampleRates = info.sampleRates;
				device.PreferredSampleRate = info.preferredSampleRate;
				if (!info.probed) device.ProbeError = "driver did not probe successfully";
			}
			catch (const RtAudioError& err) { device.ProbeError = err.getMessage(); }
			inventory.Devices.push_back(std::move(device));
		}
	}
	catch (const RtAudioError& err) { inventory.EnumerationError = err.getMessage(); }
	if (!inventory.EnumerationError.empty())
		std::cout << "[ASIO] enumeration failed: " << inventory.EnumerationError << std::endl;
	if (inventory.Devices.empty()) std::cout << "[ASIO] no devices found" << std::endl;
	for (const auto& d : inventory.Devices)
	{
		std::cout << "[ASIO] id=" << d.Id << " name=" << d.Name << " probed=" << d.Probed
			<< " input=" << d.InputChannels << " output=" << d.OutputChannels
			<< " preferredRate=" << d.PreferredSampleRate
			<< " defaultInput=" << d.DefaultInput << " defaultOutput=" << d.DefaultOutput
			<< " rates=";
		for (const auto rate : d.SampleRates) std::cout << rate << ",";
		if (!d.ProbeError.empty()) std::cout << " error=" << d.ProbeError;
		std::cout << std::endl;
	}
	return inventory;
}

std::vector<unsigned int> AudioDevice::OrderedRates(const std::vector<unsigned int>& rates, unsigned int preferred)
{
	auto ordered = rates;
	std::sort(ordered.begin(), ordered.end(), [preferred](unsigned int a, unsigned int b)
	{
		const auto distanceA = a > preferred ? a - preferred : preferred - a;
		const auto distanceB = b > preferred ? b - preferred : preferred - b;
		return distanceA == distanceB ? a < b : distanceA < distanceB;
	});
	ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
	return ordered;
}

std::vector<unsigned int> AudioDevice::OrderedCandidateIds(const AsioInventory& inventory,
	const io::UserConfig::AudioSettings& settings, bool generatedRig)
{
	std::vector<unsigned int> ids;
	const auto append = [&ids](const AsioDeviceInfo& device)
	{
		if (device.Probed && std::find(ids.begin(), ids.end(), device.Id) == ids.end())
			ids.push_back(device.Id);
	};
	if (!generatedRig && !settings.Name.empty())
		for (const auto& device : inventory.Devices)
			if (device.Name == settings.Name) append(device);
	const auto appendId = [&](const std::optional<unsigned int>& id)
	{
		if (id)
			for (const auto& device : inventory.Devices)
				if (device.Id == *id) append(device);
	};
	if (generatedRig || settings.NumChannelsOut > 0u)
	{
		appendId(inventory.DefaultOutputId);
		appendId(inventory.DefaultInputId);
	}
	else
	{
		appendId(inventory.DefaultInputId);
		appendId(inventory.DefaultOutputId);
	}
	for (const auto& device : inventory.Devices)
		if (device.OutputChannels > 0u && (device.InputChannels > 0u || !generatedRig))
			append(device);
	for (const auto& device : inventory.Devices) append(device);
	if (generatedRig)
	{
		std::stable_partition(ids.begin(), ids.end(), [&inventory](unsigned int id)
		{
			const auto found = std::find_if(inventory.Devices.begin(), inventory.Devices.end(),
				[id](const AsioDeviceInfo& device) { return device.Id == id; });
			return found != inventory.Devices.end() && found->InputChannels >= 2u && found->OutputChannels >= 2u;
		});
	}
	return ids;
}

std::vector<std::pair<unsigned int, unsigned int>> AudioDevice::GeneratedChannelPairs(
	unsigned int inputChannels, unsigned int outputChannels)
{
	std::vector<std::pair<unsigned int, unsigned int>> pairs;
	if (outputChannels == 0u) return pairs;
	const auto add = [&pairs](unsigned int in, unsigned int out)
	{
		const auto pair = std::make_pair(in, out);
		if (std::find(pairs.begin(), pairs.end(), pair) == pairs.end()) pairs.push_back(pair);
	};
	// Descend one direction at a time so a driver accepting 25 of 32 inputs
	// is tested before dropping to a stereo pair. Keep the other direction full.
	for (auto in = inputChannels; in > 1u; --in) add(in, outputChannels);
	for (auto out = outputChannels; out > 1u; --out) add(inputChannels, out);
	// Some drivers require both directions to shrink together.
	for (auto reduction = 1u; reduction < std::min(inputChannels, outputChannels); ++reduction)
		add(inputChannels - reduction, outputChannels - reduction);
	add(std::min(inputChannels, 2u), std::min(outputChannels, 2u));
	add(std::min(inputChannels, 1u), std::min(outputChannels, 2u));
	add(0u, std::min(outputChannels, 2u));
	add(std::min(inputChannels, 2u), 1u);
	add(std::min(inputChannels, 1u), 1u);
	add(0u, 1u);
	return pairs;
}

std::vector<AsioConfiguration> AudioDevice::GeneratedAttemptPlan(
	unsigned int inputChannels, unsigned int outputChannels,
	const std::vector<unsigned int>& rates, bool stereoPass, std::size_t maxAttempts)
{
	std::vector<AsioConfiguration> plan;
	const auto pairs = GeneratedChannelPairs(inputChannels, outputChannels);
	const bool hasStereo = inputChannels >= 2u && outputChannels >= 2u;
	// Reserve one attempt per rate for stereo before the bounded descent can run out.
	const auto stereoReserve = stereoPass && hasStereo ? std::min(maxAttempts, rates.size()) : 0u;
	for (const auto [inputs, outputs] : pairs)
	{
		const bool stereo = inputs >= 2u && outputs >= 2u;
		if (stereo != stereoPass || (stereoPass && inputs == 2u && outputs == 2u)) continue;
		const auto limit = maxAttempts - stereoReserve;
		for (const auto rate : rates)
		{
			if (plan.size() >= limit) break;
			plan.push_back({ inputs, outputs, rate });
		}
		if (plan.size() >= limit) break;
	}
	if (stereoReserve)
		for (const auto rate : rates)
		{
			if (plan.size() >= maxAttempts) break;
			plan.push_back({ 2u, 2u, rate });
		}
	return plan;
}

std::optional<std::unique_ptr<AudioDevice>> AudioDevice::Open(
	std::function<int(void*, void*, unsigned int, double, RtAudioStreamStatus, void*)> onAudio,
	std::function<void(RtAudioError::Type, const std::string&)> onError,
	io::UserConfig::AudioSettings settings, void* audioSink, bool generatedRig,
	const AsioInventory* suppliedInventory, AsioOpenReport* report,
	const std::function<void(const AudioStreamParams&)>& prepareForStart)
{
	(void)onError; // RtAudio's callback is asynchronous and handled by the host.
	AsioInventory discovered;
	if (!suppliedInventory) { discovered = DiscoverAsio(); suppliedInventory = &discovered; }
	if (report) *report = {};
	const auto candidateIds = OrderedCandidateIds(*suppliedInventory, settings, generatedRig);

	// Each generated pass has its own per-driver budget. Try stereo on every
	// candidate before accepting degraded capture or playback on any candidate.
	constexpr std::size_t maxAttemptsPerDevice = 2048u;
	for (unsigned int pass = 0u; pass < (generatedRig ? 2u : 1u); ++pass)
	{
	for (const auto candidateId : candidateIds)
	{
		const auto found = std::find_if(suppliedInventory->Devices.begin(), suppliedInventory->Devices.end(),
			[candidateId](const AsioDeviceInfo& device) { return device.Id == candidateId; });
		if (found == suppliedInventory->Devices.end()) continue;
		const auto* candidate = &*found;
		if (candidate->SampleRates.empty() || (generatedRig && candidate->OutputChannels == 0u))
		{
			std::cout << "[ASIO] skip " << candidate->Name << ": no output or supported rate" << std::endl;
			continue;
		}
		const auto rates = OrderedRates(candidate->SampleRates, settings.SampleRate);
		auto plan = generatedRig ? GeneratedAttemptPlan(candidate->InputChannels,
			candidate->OutputChannels, rates, pass == 0u, maxAttemptsPerDevice) :
			std::vector<AsioConfiguration>{};
		if (!generatedRig)
			for (const auto rate : rates)
			{
				if (plan.size() >= maxAttemptsPerDevice) break;
				plan.push_back({ std::min(settings.NumChannelsIn, candidate->InputChannels),
					std::min(settings.NumChannelsOut, candidate->OutputChannels), rate });
			}
		for (const auto& configuration : plan)
		{
			const auto inputs = configuration.InputChannels;
			const auto outputs = configuration.OutputChannels;
			const auto rate = configuration.SampleRate;
			if (inputs == 0u && outputs == 0u) continue;
				AsioAttempt attempt{ candidate->Id, candidate->Name, inputs, outputs,
					rate, settings.BufSize, settings.NumBuffers };
				std::cout << "[ASIO] attempt id=" << attempt.DeviceId << " name=" << attempt.Name
					<< " input=" << inputs << " output=" << outputs << " rate=" << rate
					<< " buffer=" << settings.BufSize << " buffers=" << settings.NumBuffers << std::endl;
				try
				{
					auto stream = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO);
					RtAudio::StreamParameters inParams;
					inParams.deviceId = candidate->Id;
					inParams.nChannels = inputs;
					inParams.firstChannel = 0u;
					RtAudio::StreamParameters outParams;
					outParams.deviceId = candidate->Id;
					outParams.nChannels = outputs;
					outParams.firstChannel = 0u;
					RtAudio::StreamOptions options;
					options.numberOfBuffers = settings.NumBuffers;
					auto bufferSize = settings.BufSize;
					stream->openStream(outputs ? &outParams : nullptr, inputs ? &inParams : nullptr,
						RTAUDIO_FLOAT32, rate, &bufferSize, *onAudio.target<RtAudioCallback>(),
						audioSink, &options, nullptr);
					attempt.Opened = stream->isStreamOpen();
					if (!attempt.Opened) { attempt.Error = "stream did not open"; }
					else
					{
						// ASIO publishes its latencies during openStream, before the first
						// callback. Prepare the host with those same values before startStream.
						AudioStreamParams params{ candidate->Name, rate, options.numberOfBuffers,
							bufferSize,
							static_cast<unsigned int>(stream->getInputStreamLatency()),
							static_cast<unsigned int>(stream->getOutputStreamLatency()),
							inputs, outputs };
						if (prepareForStart) prepareForStart(params);
						try
						{
							stream->startStream();
							attempt.Started = stream->isStreamRunning();
							if (attempt.Started)
							{
								params.InputLatency = static_cast<unsigned int>(stream->getInputStreamLatency());
								params.OutputLatency = static_cast<unsigned int>(stream->getOutputStreamLatency());
								if (report) { report->Attempts.push_back(attempt); report->StartedParams = params; }
								params.PrintParams();
								return std::make_unique<AudioDevice>(params, std::move(stream));
							}
							attempt.Error = "stream did not start";
						}
						catch (const RtAudioError& err) { attempt.Error = err.getMessage(); }
					}
				}
				catch (const RtAudioError& err) { attempt.Error = err.getMessage(); }
				std::cout << "[ASIO] failed id=" << attempt.DeviceId << " name=" << attempt.Name
					<< " opened=" << attempt.Opened << " reason=" << attempt.Error << std::endl;
				if (report) report->Attempts.push_back(std::move(attempt));
		}
	}
	}
	std::cout << "[ASIO] no stream started" << std::endl;
	return std::nullopt;
}
