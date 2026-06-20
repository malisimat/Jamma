#pragma once

#include <array>
#include <atomic>
#include <cstddef>
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

namespace vst
{
	class IVstPlugin;
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

		// --- Editor-driven automation feedback suppression ---
		// Per (plugin, parameter) cool-down published by the non-RT MIDI pump and
		// read by the audio-thread automation player. While suppressed, recorded
		// automation playback skips that one parameter so a live editor drag is not
		// fought by its own freshly recorded curve. Deadlines live in the audio
		// sample domain so the audio thread never reads a wall clock.

		// Cool-down window after the last editor-origin change, in milliseconds.
		static constexpr double AutomationSuppressionCooldownMs = 800.0;

		// Audio thread: true while (plugin, paramIdx) is within its cool-down at
		// blockStartSample. Real-time safe: bounded flat scan, no locks/allocation,
		// no wall-clock read.
		static bool IsParameterSuppressed(const vst::IVstPlugin* plugin,
			unsigned int paramIdx,
			std::uint32_t blockStartSample) noexcept;

		// Non-RT: refresh (or claim) the suppression entry for (plugin, paramIdx)
		// with an absolute sample-domain expiry. nowSample lets stale entries be
		// reclaimed.
		static void RefreshAutomationSuppression(const vst::IVstPlugin* plugin,
			unsigned int paramIdx,
			std::uint32_t nowSample,
			std::uint32_t expirySample) noexcept;

		// Test hook: drop all suppression entries.
		static void ResetAutomationSuppressionForTest() noexcept;

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

		// Non-RT: poll vst::_lastTouchedParam for a fresh editor-origin parameter
		// change and, while automation record is held, record it into the owning
		// station's last recorded MIDI loop (auto-creating/reusing a lane) and
		// refresh that parameter's playback suppression. Called once per pump.
		void _ConsumeEditorAutomation(const std::vector<std::shared_ptr<engine::Station>>& stations,
			std::uint64_t globalSampleNow,
			const audio::AudioStreamParams& audioParams) noexcept;
		void _ResetEditorTouchStates() noexcept;

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

		// Highest editor-origin sequence already consumed by _ConsumeEditorAutomation.
		// Touched only on the (single) MIDI pump thread.
		std::uint64_t _lastEditorAutomationSeq = 0u;

		// Per-(plugin, param) state for active editor-touch cooldown windows. Each
		// real VST editor change writes one bounded overwrite window and refreshes
		// suppression once; idle pump ticks only age out stale sessions.
		//
		// Lifecycle:
		//  • _ResetEditorTouchStates() clears all states (called on Ctrl+Shift+A press).
		//  • First VST touch after reset: freshDrag → state activated and a hold window written.
		//  • Subsequent touches in the same record session: state stays active and writes a fresh window.
		//  • No new touch for > cooldown samples: state expires.
		//  • Next VST touch after expiry: freshDrag again → new cooldown session.
		struct EditorTouchState
		{
			bool Active = false;
			const vst::IVstPlugin* Plugin = nullptr;
			unsigned int ParamIndex = 0u;
			std::uint32_t LastTouchSample = 0u;
			std::weak_ptr<midi::MidiLoop> Loop;
			std::size_t LaneIdx = 0u;
		};
		static constexpr std::size_t MaxEditorTouchStates = 16u;
		std::array<EditorTouchState, MaxEditorTouchStates> _editorTouchStates{};

		// Fixed-capacity per-parameter suppression table. Written on the non-RT MIDI
		// pump thread, read on the audio thread; each field is independently atomic
		// and a momentarily stale read is harmless (best-effort cool-down).
		struct AutomationSuppressionSlot
		{
			std::atomic<const vst::IVstPlugin*> Plugin{ nullptr };
			std::atomic<unsigned int>           ParamIndex{ 0u };
			std::atomic<std::uint32_t>          ExpirySample{ 0u };
		};
		static constexpr std::size_t MaxAutomationSuppressions = 16u;
		static std::array<AutomationSuppressionSlot, MaxAutomationSuppressions> _automationSuppressions;
		static std::atomic<std::uint8_t> _automationSuppressionCount;
	};
}