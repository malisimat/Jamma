#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "../actions/ActionResult.h"
#include "../actions/KeyAction.h"
#include "../audio/AudioDevice.h"
#include "../base/LoggingConfig.h"
#include "../io/SerialDevice.h"
#include "../io/SerialTriggerQueue.h"
#include "../midi/MidiDevice.h"
#include "../midi/MidiEvent.h"
#include "../midi/MidiQueue.h"

namespace io
{
	struct UserConfig;
}

namespace engine
{
	class LoopTake;
	class Station;
	class Trigger;
}

namespace midi
{
	class MidiLoop;

	static constexpr std::uint8_t LearnNothingCaptured = 0xffu;

	class MidiRouter
	{
	public:
		struct TriggerDispatchSummary
		{
			bool Activated = false;
			bool Ditched = false;
		};

		MidiRouter() = default;
		~MidiRouter() { CloseMidi(); CloseSerial(); }

		MidiRouter(const MidiRouter&) = delete;
		MidiRouter& operator=(const MidiRouter&) = delete;

		void InitMidi(const io::UserConfig& cfg,
			const base::LoggingConfig& loggingConfig,
			std::atomic<std::uint64_t>& audioSampleCounter,
			std::atomic<std::int64_t>& midiAnchorMicros);
		void CloseMidi();
		void InitSerial(const io::UserConfig& cfg);
		void CloseSerial();
		void RegisterTrigger(const std::string& deviceName, std::shared_ptr<engine::Trigger> trigger);
		void RegisterTriggerForTest(const std::string& deviceName,
			std::shared_ptr<engine::Trigger> trigger,
			std::uint8_t deviceSlot);
		void AddMidiInputDeviceForTest(const std::string& deviceName, std::uint8_t deviceSlot);
		void PushMidiEventForTest(std::uint8_t deviceSlot,
			std::uint8_t status,
			std::uint8_t data1,
			std::uint8_t data2) noexcept;
		bool HasMidiInputDeviceForTest(std::uint8_t deviceSlot) const noexcept;
		void PushSerialTriggerEventForTest(const io::SerialTriggerEvent& event);
		TriggerDispatchSummary DispatchMidiTriggerEventForTest(std::uint8_t deviceSlot,
			const midi::MidiEvent& event,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& audioParams);

		TriggerDispatchSummary PumpMidi(const std::vector<std::shared_ptr<engine::Station>>& stations,
			std::uint64_t globalSampleNow,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& audioParams) noexcept;

		TriggerDispatchSummary PumpSerial(const std::vector<std::shared_ptr<engine::Station>>& stations,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& audioParams) noexcept;

		actions::ActionResult HandleAutomationKey(const actions::KeyAction& action,
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			const std::vector<unsigned char>& hoverPath,
			const std::shared_ptr<engine::LoopTake>& hoveredTake);

		static bool IsAutomationRecordHeld() noexcept;
		static void SetAutomationRecordHeldForTest(bool held) noexcept;

	private:
		static constexpr std::uint8_t UnresolvedMidiDeviceSlot = 0xffu;

		struct MidiInputEndpoint
		{
			std::uint8_t DeviceSlot = 0u;
			std::string ConfiguredName;
			std::unique_ptr<midi::MidiDevice> Device;
			midi::MidiQueue<1024> Ingress;
			std::uint64_t LastDroppedCount = 0u;
		};

		struct MidiTriggerRoute
		{
			std::string DeviceName;
			std::uint8_t DeviceSlot = UnresolvedMidiDeviceSlot;
			std::shared_ptr<engine::Trigger> Trigger;
		};

		TriggerDispatchSummary _DispatchMidiTriggerEvent(std::uint8_t deviceSlot,
			const midi::MidiEvent& event,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& audioParams);
		std::pair<std::shared_ptr<engine::Station>, std::shared_ptr<midi::MidiLoop>> _ResolveAutomationTarget(
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			const std::vector<unsigned char>& hoverPath,
			const std::shared_ptr<engine::LoopTake>& hoveredTake) const;
		void _PublishMidiTriggerRoutes();

		std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<MidiInputEndpoint>>>> _midiInputs;
		std::vector<MidiTriggerRoute> _midiTriggerRoutes;
		std::atomic<std::shared_ptr<const std::vector<MidiTriggerRoute>>> _midiTriggerRoutesSnapshot;
		std::vector<std::unique_ptr<io::SerialDevice>> _serialDevices;
		io::SerialTriggerQueue<256> _serialIngress;
		std::mutex _serialIngressMutex;
		std::uint64_t _lastSerialDropCount = 0u;
		std::atomic<bool> _learnMidiCCMode{ false };
		std::atomic<std::uint8_t> _learnedCC{ LearnNothingCaptured };
		std::atomic<std::uint8_t> _learnedChannel{ LearnNothingCaptured };
		std::atomic<std::uint8_t> _selectedLaneIndex{ 0u };
		bool _automationRecordKeyHeld = false;
		static std::atomic<bool> _automationRecordHeld;
	};
}