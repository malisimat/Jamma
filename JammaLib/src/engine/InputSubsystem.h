#pragma once

#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include "../io/UserConfig.h"
#include "../io/SerialDevice.h"
#include "../midi/MidiRouter.h"
#include "Station.h"

namespace engine
{
	class InputSubsystem
	{
	public:
	    struct PumpResult
		{
			bool Activated = false;
			bool Ditched = false;
		};

		InputSubsystem(io::UserConfig userConfig, io::LoggingConfig loggingConfig);
		~InputSubsystem();

		void Init(std::atomic<std::uint64_t>& audioSampleCounter,
			std::atomic<std::int64_t>& midiAnchorMicros);
		void Close();

		PumpResult PumpMidi(std::vector<std::shared_ptr<Station>>& stations,
			std::uint64_t audioSampleCounter,
			const audio::AudioStreamParams& streamParams,
			std::mutex& audioMutex);

		PumpResult PumpSerial(std::vector<std::shared_ptr<Station>>& stations,
			const audio::AudioStreamParams& streamParams,
			std::mutex& audioMutex);

		void RegisterMidiTriggerRoute(const std::string& deviceName, std::shared_ptr<Trigger> trigger);

		midi::MidiRouter& GetMidiRouterForTest() { return _midiRouter; }

	private:
		io::UserConfig _userConfig;
		io::LoggingConfig _loggingConfig;
		midi::MidiRouter _midiRouter;
	};
}
