#pragma once

#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include <windows.h>
#include "../io/UserConfig.h"
#include "../io/SerialDevice.h"
#include "../midi/MidiRouter.h"
#include "../engine/Station.h"

namespace io
{
	class IoInputSubsystem
	{
	public:
	    struct PumpResult
		{
			bool Activated = false;
			bool Ditched = false;
		};

		IoInputSubsystem(io::UserConfig userConfig, io::LoggingConfig loggingConfig);
		~IoInputSubsystem();

		void Init(std::atomic<std::uint64_t>& audioSampleCounter,
			std::atomic<std::int64_t>& midiAnchorMicros);
		void Close();
		bool InitGlobalInsertCapture();
		void CloseGlobalInsertCapture();
		bool PumpGlobalInsertCapture(actions::KeyAction& action) noexcept;

		PumpResult PumpMidi(std::vector<std::shared_ptr<engine::Station>>& stations,
			std::uint64_t audioSampleCounter,
			const audio::AudioStreamParams& streamParams,
			std::mutex& audioMutex);

		PumpResult PumpSerial(std::vector<std::shared_ptr<engine::Station>>& stations,
			const audio::AudioStreamParams& streamParams,
			std::mutex& audioMutex);

		actions::ActionResult HandleAutomationKey(const actions::KeyAction& action,
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			const std::vector<unsigned char>& hoverPath,
			const std::shared_ptr<engine::LoopTake>& hoveredTake);
		actions::ActionResult HandleChannelOverrideKey(const actions::KeyAction& action,
			const std::vector<std::shared_ptr<engine::Station>>& stations);
		void SetForcedChannelOverride(std::uint8_t forcedChannelOverride,
			const std::vector<std::shared_ptr<engine::Station>>& stations) noexcept;
		std::uint8_t ForcedChannelOverride() const noexcept;

		void RegisterMidiTriggerRoute(const std::string& deviceName, std::shared_ptr<engine::Trigger> trigger);

	private:
		static LRESULT CALLBACK _LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept;
		static HHOOK _globalInsertHook;
		static std::atomic<bool> _globalInsertDown;
		static std::atomic<bool> _globalInsertLastDispatchedDown;

		io::UserConfig _userConfig;
		io::LoggingConfig _loggingConfig;
		midi::MidiRouter _midiRouter;
	};
}
