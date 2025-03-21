///////////////////////////////////////////////////////////
//
// Author 2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "UserConfig.h"

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

	auto iter = json.KeyValues.find("fadeSamps");
	if (iter != json.KeyValues.end())
	{
		if (json.KeyValues["fadeSamps"].index() == 2)
			fadeSamps = std::get<unsigned long>(json.KeyValues["fadeSamps"]);
	}

	LoopSettings loop;
	loop.FadeSamps = fadeSamps;
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

	return playPos;
}