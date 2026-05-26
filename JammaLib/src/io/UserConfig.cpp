///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "UserConfig.h"
#include <limits>

using namespace io;

std::optional<UserConfig> UserConfig::FromJson(Json::JsonPart json)
{
	UserConfig cfg;

	auto gotAudio = false;
	auto iter = json.KeyValues.find("audio");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["audio"].index() == 6)
		{
			auto audioJson = std::get<Json::JsonPart>(json.KeyValues["audio"]);
			auto audioOpt = AudioSettings::FromJson(audioJson);

			if (audioOpt.has_value())
			{
				cfg.Audio = audioOpt.value();
				gotAudio = true;
			}
		}
	}

	if (!gotAudio)
		return std::nullopt;

	iter = json.KeyValues.find("loop");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["loop"].index() == 6)
		{
			auto loopJson = std::get<Json::JsonPart>(json.KeyValues["loop"]);
			auto loopOpt = LoopSettings::FromJson(loopJson);

			if (loopOpt.has_value())
				cfg.Loop = loopOpt.value();
		}
	}

	iter = json.KeyValues.find("trigger");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["trigger"].index() == 6)
		{
			auto trigJson = std::get<Json::JsonPart>(json.KeyValues["trigger"]);
			auto trigOpt = TriggerSettings::FromJson(trigJson);

			if (trigOpt.has_value())
				cfg.Trigger = trigOpt.value();
		}
	}

	iter = json.KeyValues.find("midi");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["midi"].index() == 6)
		{
			auto midiJson = std::get<Json::JsonPart>(json.KeyValues["midi"]);
			auto midiOpt = MidiConfig::FromJson(midiJson);

			if (midiOpt.has_value())
				cfg.Midi = midiOpt.value();
			else
				return std::nullopt;
		}
		else
			return std::nullopt;
	}

	iter = json.KeyValues.find("serial");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["serial"].index() == 6)
		{
			auto serialJson = std::get<Json::JsonPart>(json.KeyValues["serial"]);
			auto serialOpt = SerialConfig::FromJson(serialJson);

			if (serialOpt.has_value())
				cfg.Serial = serialOpt.value();
		}
	}

	return cfg;
}

std::optional<UserConfig::AudioSettings> UserConfig::AudioSettings::FromJson(Json::JsonPart json)
{
	std::string name;
	unsigned int sampleRate = constants::DefaultSampleRate;
	unsigned int bufSize = constants::DefaultBufferSizeSamps;
	unsigned int inLatency = 512;
	unsigned int outLatency = 512;
	unsigned int numBuffers = 4;
	unsigned int numChannelsIn = 2;
	unsigned int numChannelsOut = 2;

	auto iter = json.KeyValues.find("name");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["name"].index() == 4)
			name = std::get<std::string>(json.KeyValues["name"]);
	}

	iter = json.KeyValues.find("samplerate");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["samplerate"].index() == 2)
			sampleRate = std::get<unsigned long>(json.KeyValues["samplerate"]);
	}

	iter = json.KeyValues.find("bufsize");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["bufsize"].index() == 2)
			bufSize = std::get<unsigned long>(json.KeyValues["bufsize"]);
	}

	iter = json.KeyValues.find("numbuffers");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["numbuffers"].index() == 2)
			numBuffers = std::get<unsigned long>(json.KeyValues["numbuffers"]);
	}

	iter = json.KeyValues.find("inlatency");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["inlatency"].index() == 2)
			inLatency = std::get<unsigned long>(json.KeyValues["inlatency"]);
	}

	iter = json.KeyValues.find("outlatency");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["outlatency"].index() == 2)
			outLatency = std::get<unsigned long>(json.KeyValues["outlatency"]);
	}

	iter = json.KeyValues.find("numchannelsin");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["numchannelsin"].index() == 2)
			numChannelsIn = std::get<unsigned long>(json.KeyValues["numchannelsin"]);
	}

	iter = json.KeyValues.find("numchannelsout");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["numchannelsout"].index() == 2)
			numChannelsOut = std::get<unsigned long>(json.KeyValues["numchannelsout"]);
	}

	AudioSettings audio;
	audio.Name = name;
	audio.SampleRate = sampleRate;
	audio.BufSize = bufSize;
	audio.LatencyIn = inLatency;
	audio.LatencyOut = outLatency;
	audio.NumBuffers = numBuffers;
	audio.NumChannelsIn = numChannelsIn;
	audio.NumChannelsOut = numChannelsOut;
	return audio;
}

std::optional<UserConfig::LoopSettings> UserConfig::LoopSettings::FromJson(Json::JsonPart json)
{
	unsigned int fadeSamps = constants::DefaultFadeSamps;
	unsigned int seedGrainMinMs = 400u;
	unsigned int seedGrainTargetMaxMs = 3000u;
	unsigned int seedBpmMin = 80u;
	auto seedUsesPowers = true;

	auto iter = json.KeyValues.find("fadeSamps");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["fadeSamps"].index() == 2)
			fadeSamps = std::get<unsigned long>(json.KeyValues["fadeSamps"]);
	}

	iter = json.KeyValues.find("seedGrainMinMs");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["seedGrainMinMs"].index() == 2)
			seedGrainMinMs = std::get<unsigned long>(json.KeyValues["seedGrainMinMs"]);
	}

	iter = json.KeyValues.find("seedGrainTargetMaxMs");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["seedGrainTargetMaxMs"].index() == 2)
			seedGrainTargetMaxMs = std::get<unsigned long>(json.KeyValues["seedGrainTargetMaxMs"]);
	}

	iter = json.KeyValues.find("seedBpmMin");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["seedBpmMin"].index() == 2)
			seedBpmMin = std::get<unsigned long>(json.KeyValues["seedBpmMin"]);
	}

	iter = json.KeyValues.find("seedQuantisation");
	if ((iter != json.KeyValues.end()) && (json.KeyValues["seedQuantisation"].index() == 4))
		seedUsesPowers = std::get<std::string>(json.KeyValues["seedQuantisation"]) != "multiple";

	LoopSettings loop;
	loop.FadeSamps = fadeSamps;
	loop.SeedGrainMinMs = seedGrainMinMs;
	loop.SeedGrainTargetMaxMs = seedGrainTargetMaxMs;
	loop.SeedBpmMin = seedBpmMin;
	loop.SeedUsesPowers = seedUsesPowers;
	return loop;
}

std::optional<UserConfig::TriggerSettings> UserConfig::TriggerSettings::FromJson(Json::JsonPart json)
{
	unsigned int preDelay = constants::DefaultPreDelaySamps;
	unsigned int debounceSamps = constants::DefaultDebounceSamps;

	auto iter = json.KeyValues.find("preDelay");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["preDelay"].index() == 2)
			preDelay = std::get<unsigned long>(json.KeyValues["preDelay"]);
	}

	iter = json.KeyValues.find("debounceSamps");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["debounceSamps"].index() == 2)
			debounceSamps = std::get<unsigned long>(json.KeyValues["debounceSamps"]);
	}

	TriggerSettings trig;
	trig.PreDelay = preDelay;
	trig.DebounceSamps = debounceSamps;
	return trig;
}

std::optional<UserConfig::MidiSettings> UserConfig::MidiSettings::FromJson(Json::JsonPart json)
{
	std::string name = "default";
	bool enabled = true;

	auto iter = json.KeyValues.find("name");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["name"].index() == 4)
			name = std::get<std::string>(json.KeyValues["name"]);
	}

	iter = json.KeyValues.find("enabled");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["enabled"].index() == 0)
			enabled = std::get<bool>(json.KeyValues["enabled"]);
	}

	MidiSettings midi;
	midi.Name = name;
	midi.Enabled = enabled;
	return midi;
}

std::optional<UserConfig::MidiConfig> UserConfig::MidiConfig::FromJson(Json::JsonPart json)
{
	auto iter = json.KeyValues.find("devices");
	if ((iter == json.KeyValues.end()) || (iter->second.index() != 5))
		return std::nullopt;

	auto devicesArray = std::get<Json::JsonArray>(iter->second);
	if (devicesArray.Array.index() != 5)
		return std::nullopt;

	MidiConfig midi;
	auto devices = std::get<std::vector<Json::JsonPart>>(devicesArray.Array);
	for (const auto& deviceJson : devices)
	{
		auto device = MidiSettings::FromJson(deviceJson);
		if (!device.has_value())
			continue;

		if (device->Name.empty())
			device->Name = "default";

		const auto exists = std::find_if(midi.Devices.begin(), midi.Devices.end(), [&](const MidiSettings& current) {
			return current.Name == device->Name;
		});
		if (exists == midi.Devices.end())
			midi.Devices.push_back(device.value());
	}

	if (!midi.Devices.empty())
	{
		auto active = std::find_if(midi.Devices.begin(), midi.Devices.end(), [](const MidiSettings& device) {
			return device.Enabled;
		});

		if (active != midi.Devices.end())
		{
			midi.Name = active->Name;
			midi.Enabled = true;
		}
		else
		{
			midi.Name = midi.Devices.front().Name;
			midi.Enabled = false;
		}
	}

	return midi;
}

std::optional<UserConfig::SerialSettings> UserConfig::SerialSettings::FromJson(Json::JsonPart json)
{
	std::string name = "default";
	std::string port = "COM3";
	unsigned int baudRate = 115200u;
	bool enabled = false;

	auto iter = json.KeyValues.find("name");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["name"].index() == 4)
			name = std::get<std::string>(json.KeyValues["name"]);
	}

	iter = json.KeyValues.find("port");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["port"].index() == 4)
			port = std::get<std::string>(json.KeyValues["port"]);
	}

	iter = json.KeyValues.find("baudrate");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["baudrate"].index() == 2)
			baudRate = std::get<unsigned long>(json.KeyValues["baudrate"]);
	}

	iter = json.KeyValues.find("enabled");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["enabled"].index() == 0)
			enabled = std::get<bool>(json.KeyValues["enabled"]);
	}

	SerialSettings serial;
	serial.Name = name.empty() ? "default" : name;
	serial.Port = port;
	serial.BaudRate = baudRate;
	serial.Enabled = enabled;
	return serial;
}

std::optional<UserConfig::SerialConfig> UserConfig::SerialConfig::FromJson(Json::JsonPart json)
{
	SerialConfig serial;

	auto iter = json.KeyValues.find("devices");
	if ((iter != json.KeyValues.end()) && (json.KeyValues["devices"].index() == 5))
	{
		auto devicesArray = std::get<Json::JsonArray>(json.KeyValues["devices"]);
		if (devicesArray.Array.index() == 5)
		{
			auto devices = std::get<std::vector<Json::JsonPart>>(devicesArray.Array);
			for (const auto& deviceJson : devices)
			{
				auto device = SerialSettings::FromJson(deviceJson);
				if (device.has_value())
					serial.Devices.push_back(device.value());
			}
		}

		return serial;
	}

	auto singleDevice = SerialSettings::FromJson(json);
	if (singleDevice.has_value())
		serial.Devices.push_back(singleDevice.value());

	return serial;
}



// How much to (further) delay input signal from ADC, in samples
unsigned int UserConfig::AdcBufferDelay(unsigned int inLatency) const {
	return inLatency > Trigger.PreDelay + constants::MaxLoopFadeSamps ?
		0 :
		Trigger.PreDelay + constants::MaxLoopFadeSamps - inLatency;
}

// How long to continue recording after trigger to end loop recording, in samples
unsigned int UserConfig::EndRecordingSamps(int error) const {
	if (error > 0)
	{
		if (error > (int)constants::MaxLoopFadeSamps)
			return 0;
	}

	return constants::MaxLoopFadeSamps - error;
}

// The index at which to start playing a loop after trigger to end recording,
// in samples (includes intro, so zero is first index of intro, not the loop)
unsigned long UserConfig::LoopPlayPos(int error,
	unsigned long loopLength,
	unsigned int outLatency) const {
	auto loopStart = (unsigned long)constants::MaxLoopFadeSamps;
	auto fadeSamps = 0u;// Loop.FadeSamps;
	auto playPos = (loopStart - fadeSamps) + Trigger.PreDelay + outLatency;
	auto bufSize = loopLength + loopStart;
	
	if (loopLength > 0)
	{
		if (error >= 0)
		{
			// If positive error, move playpos forward to skip time
			// already lost due to late trigger
			playPos += error;

			while (playPos >= bufSize)
				playPos -= loopLength;
		}
		else
		{
			// If negative error, then move position backwards
			// to fill in time until trigger should have been pressed
			auto appliedError = false;
			auto foundPos = false;

			while (!foundPos)
			{
				if (!appliedError)
				{
					if (playPos < (unsigned long)(-error))
						playPos += loopLength;
					else
					{
						playPos += error;
						appliedError = true;
					}
				}
				else
				{
					if (playPos < loopStart)
						playPos += loopLength;
					else
						foundPos = true;
				}
			}
		}

		// For first play, since buffers may still be filling up
		// then play from before the loop starts to avoid glitches
		if (playPos > loopLength)
			playPos -= loopLength;
	}

	return playPos;
}

unsigned int UserConfig::TriggerLoopAlignmentSamps() const
{
	return Trigger.PreDelay + constants::MaxLoopFadeSamps;
}

long UserConfig::OverdubSourceReadOffset(unsigned int outLatency) const
{
	return -static_cast<long>(TriggerLoopAlignmentSamps() + outLatency);
}

unsigned long UserConfig::OverdubPlayPos(int error, unsigned long loopLength) const
{
	return LoopPlayPos(error, loopLength, 0u);
}

std::optional<UserConfig::SeedLoopTiming> UserConfig::DeduceLoopTiming(unsigned long loopLengthSamps,
	unsigned int sampleRate) const
{
	if ((0ul == loopLengthSamps) || (0u == sampleRate))
		return std::nullopt;

	auto grainSamps = loopLengthSamps;
	auto loopGrains = 1u;
	const auto seedGrainMinMs = std::max(1u, Loop.SeedGrainMinMs);
	const auto seedGrainTargetMaxMs = std::max(seedGrainMinMs, Loop.SeedGrainTargetMaxMs);
	const auto minGrainSamps = static_cast<unsigned long>(std::max(1.0, (static_cast<double>(sampleRate) * static_cast<double>(seedGrainMinMs)) / 1000.0));
	const auto targetMaxGrainSamps = static_cast<unsigned long>(std::max(static_cast<double>(minGrainSamps),
		(static_cast<double>(sampleRate) * static_cast<double>(seedGrainTargetMaxMs)) / 1000.0));

	while ((grainSamps >= targetMaxGrainSamps) && ((grainSamps / 2ul) >= minGrainSamps))
	{
		grainSamps /= 2ul;
		loopGrains *= 2u;
	}

	auto bpm = (60.0f * static_cast<float>(sampleRate)) / static_cast<float>(grainSamps);
	auto beatsPerGrain = 1u;
	while (bpm < static_cast<float>(Loop.SeedBpmMin))
	{
		if (beatsPerGrain > (std::numeric_limits<unsigned int>::max() / 2u))
			return std::nullopt;
		beatsPerGrain *= 2u;
		bpm *= 2.0f;
	}

	if (grainSamps > std::numeric_limits<unsigned int>::max())
		return std::nullopt;

	if (loopGrains > (std::numeric_limits<unsigned int>::max() / beatsPerGrain))
		return std::nullopt;

	SeedLoopTiming timing;
	timing.GrainSamps = static_cast<unsigned int>(grainSamps);
	timing.LoopGrains = loopGrains;
	timing.BeatsPerGrain = beatsPerGrain;
	timing.Bpm = bpm;
	timing.Bpi = loopGrains * beatsPerGrain;
	return timing;
}
