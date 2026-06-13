#include "stdafx.h"
#include "IoInputSubsystem.h"

using namespace engine;

namespace io
{
	IoInputSubsystem::IoInputSubsystem(io::UserConfig userConfig, io::LoggingConfig loggingConfig) :
		_userConfig(userConfig),
		_loggingConfig(loggingConfig)
	{
	}

	IoInputSubsystem::~IoInputSubsystem()
	{
		Close();
	}

	void IoInputSubsystem::Init(std::atomic<std::uint64_t>& audioSampleCounter,
		std::atomic<std::int64_t>& midiAnchorMicros)
	{
		_midiRouter.InitMidi(_userConfig, _loggingConfig, audioSampleCounter, midiAnchorMicros);
		_midiRouter.InitSerial(_userConfig);
	}

	void IoInputSubsystem::Close()
	{
		_midiRouter.CloseSerial();
		_midiRouter.CloseMidi();
	}

	IoInputSubsystem::PumpResult IoInputSubsystem::PumpMidi(std::vector<std::shared_ptr<Station>>& stations,
		std::uint64_t audioSampleCounter,
		const audio::AudioStreamParams& streamParams,
		std::mutex& audioMutex)
	{
		std::scoped_lock lock(audioMutex);
		PumpResult result;
		auto midiSummary = _midiRouter.PumpMidi(stations,
			static_cast<std::uint32_t>(audioSampleCounter),
			_userConfig,
			streamParams);
		if (midiSummary.Activated) result.Activated = true;
		if (midiSummary.Ditched) result.Ditched = true;
		return result;
	}

	IoInputSubsystem::PumpResult IoInputSubsystem::PumpSerial(std::vector<std::shared_ptr<Station>>& stations,
		const audio::AudioStreamParams& streamParams,
		std::mutex& audioMutex)
	{
		std::scoped_lock lock(audioMutex);
		PumpResult result;
		auto serialSummary = _midiRouter.PumpSerial(stations, _userConfig, streamParams);
		if (serialSummary.Activated) result.Activated = true;
		if (serialSummary.Ditched) result.Ditched = true;
		return result;
	}

	void IoInputSubsystem::RegisterMidiTriggerRoute(const std::string& deviceName, std::shared_ptr<Trigger> trigger)
	{
		_midiRouter.RegisterTrigger(deviceName, std::move(trigger));
	}
}
