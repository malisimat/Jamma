#include "MidiRouter.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <set>
#include "../base/Action.h"
#include "../engine/LoopTake.h"
#include "../engine/Station.h"
#include "../engine/Trigger.h"
#include "../io/UserConfig.h"
#include "../vst/IVstPlugin.h"
#include "MidiTimestampMapper.h"
#include "MidiLoop.h"
using namespace midi;

namespace midi
{
	std::atomic<bool> MidiRouter::_automationRecordHeld{ false };
	std::array<MidiRouter::AutomationSuppressionSlot, MidiRouter::MaxAutomationSuppressions> MidiRouter::_automationSuppressions{};
	std::atomic<std::uint8_t> MidiRouter::_automationSuppressionCount{ 0u };
}

std::pair<std::shared_ptr<engine::Station>, std::shared_ptr<midi::MidiLoop>> MidiRouter::_ResolveAutomationTarget(
	const std::vector<std::shared_ptr<engine::Station>>& stations,
	const std::vector<unsigned char>& hoverPath,
	const std::shared_ptr<engine::LoopTake>& hoveredTake) const
{
	if (hoverPath.empty())
		return { nullptr, nullptr };

	const auto stationIndex = hoverPath[0];
	if (stationIndex >= stations.size())
		return { nullptr, nullptr };

	auto station = stations[stationIndex];
	if (!station || station->IsRemote())
		return { nullptr, nullptr };

	std::shared_ptr<engine::LoopTake> take = hoveredTake;
	if (!take)
	{
		const auto& takes = station->GetLoopTakes();
		if (!takes.empty())
			take = takes.front();
	}
	if (!take)
		return { nullptr, nullptr };

	const auto& midiLoops = take->GetMidiLoops();
	if (midiLoops.empty() || !midiLoops.front())
		return { nullptr, nullptr };

	return { station, midiLoops.front() };
}

actions::ActionResult MidiRouter::HandleAutomationKey(const actions::KeyAction& action,
	const std::vector<std::shared_ptr<engine::Station>>& stations,
	const std::vector<unsigned char>& hoverPath,
	const std::shared_ptr<engine::LoopTake>& hoveredTake)
{
	const bool ctrlShift = (base::Action::MODIFIER_CTRL & action.Modifiers)
		&& (base::Action::MODIFIER_SHIFT & action.Modifiers);
	const bool isDown = (actions::KeyAction::KEY_DOWN == action.KeyActionType);
	const bool isUp = (actions::KeyAction::KEY_UP == action.KeyActionType);

	auto eaten = actions::ActionResult::NoAction();
	eaten.IsEaten = true;

	if (65 == action.KeyChar)
	{
		if (isDown && ctrlShift && !_automationRecordKeyHeld)
		{
			_automationRecordKeyHeld = true;
			_ResetEditorOverwriteSessions();
			_automationRecordHeld.store(true, std::memory_order_release);
			std::cout << ">> Automation record armed (Ctrl+Shift+A) <<" << std::endl;
			return eaten;
		}
		if (isUp && _automationRecordKeyHeld)
		{
			_automationRecordKeyHeld = false;
			_automationRecordHeld.store(false, std::memory_order_release);
			for (const auto& station : stations)
			{
				if (station && !station->IsRemote())
					station->RebuildAutomationDispatch();
			}
			std::cout << ">> Automation record released <<" << std::endl;
			return eaten;
		}
		return actions::ActionResult::NoAction();
	}

	if (!isDown || !ctrlShift)
		return actions::ActionResult::NoAction();

	switch (action.KeyChar)
	{
	case 76: // 'L'
	{
		const bool nowOn = !_learnMidiCCMode.load(std::memory_order_relaxed);
		_learnMidiCCMode.store(nowOn, std::memory_order_relaxed);
		if (!nowOn)
		{
			_learnedCC.store(LearnNothingCaptured, std::memory_order_relaxed);
			_learnedChannel.store(LearnNothingCaptured, std::memory_order_relaxed);
		}
		std::cout << ">> MIDI learn mode " << (nowOn ? "ON" : "OFF") << " <<" << std::endl;
		return eaten;
	}
	case 87: // 'W'
	{
		const auto learnedCC = _learnedCC.load(std::memory_order_relaxed);
		auto* plugin = vst::_lastTouchedParam.Plugin.load(std::memory_order_relaxed);
		if (learnedCC == LearnNothingCaptured || !plugin)
		{
			std::cout << "Automation wire ignored: no captured CC or touched parameter" << std::endl;
			return actions::ActionResult::NoAction();
		}

		auto [station, loop] = _ResolveAutomationTarget(stations, hoverPath, hoveredTake);
		if (!loop || !station)
			return actions::ActionResult::NoAction();

		const auto channel = _learnedChannel.load(std::memory_order_relaxed);
		const auto paramIdx = vst::_lastTouchedParam.ParameterIndex.load(std::memory_order_relaxed);
		const auto laneIdx = _selectedLaneIndex.load(std::memory_order_relaxed);

		auto& lane = loop->GetLane(laneIdx);
		lane.Mapping.TargetPlugin = plugin;
		lane.Mapping.TargetParameterIndex = paramIdx;
		lane.Mapping.MatchKey.store(
			midi::AutomationMapping::MakeMatchKey(channel & 0x0Fu, learnedCC),
			std::memory_order_relaxed);

		_learnMidiCCMode.store(false, std::memory_order_relaxed);
		_learnedCC.store(LearnNothingCaptured, std::memory_order_relaxed);
		_learnedChannel.store(LearnNothingCaptured, std::memory_order_relaxed);

		station->RebuildAutomationDispatch();
		std::cout << ">> Automation wired: lane " << static_cast<int>(laneIdx)
			<< " <- CC " << static_cast<int>(learnedCC) << " ch " << static_cast<int>(channel)
			<< " -> param " << paramIdx << " <<" << std::endl;
		return eaten;
	}
	case 88: // 'X'
	{
		auto [station, loop] = _ResolveAutomationTarget(stations, hoverPath, hoveredTake);
		if (!loop || !station)
			return actions::ActionResult::NoAction();

		const auto laneIdx = _selectedLaneIndex.load(std::memory_order_relaxed);
		loop->ClearAutomationLane(laneIdx);
		station->RebuildAutomationDispatch();
		std::cout << ">> Automation lane " << static_cast<int>(laneIdx) << " cleared <<" << std::endl;
		return eaten;
	}
	case 91: // '['
	{
		auto laneIdx = _selectedLaneIndex.load(std::memory_order_relaxed);
		laneIdx = (laneIdx == 0u)
			? static_cast<std::uint8_t>(midi::MidiLoop::MaxAutomationLanes - 1u)
			: static_cast<std::uint8_t>(laneIdx - 1u);
		_selectedLaneIndex.store(laneIdx, std::memory_order_relaxed);
		std::cout << ">> Selected automation lane " << static_cast<int>(laneIdx) << " <<" << std::endl;
		return eaten;
	}
	case 93: // ']'
	{
		auto laneIdx = _selectedLaneIndex.load(std::memory_order_relaxed);
		laneIdx = static_cast<std::uint8_t>((laneIdx + 1u) % midi::MidiLoop::MaxAutomationLanes);
		_selectedLaneIndex.store(laneIdx, std::memory_order_relaxed);
		std::cout << ">> Selected automation lane " << static_cast<int>(laneIdx) << " <<" << std::endl;
		return eaten;
	}
	default:
		break;
	}

	return actions::ActionResult::NoAction();
}

bool MidiRouter::IsAutomationRecordHeld() noexcept
{
	return _automationRecordHeld.load(std::memory_order_acquire);
}

void MidiRouter::SetAutomationRecordHeldForTest(bool held) noexcept
{
	_automationRecordHeld.store(held, std::memory_order_release);
}

void MidiRouter::_ResetEditorOverwriteSessions() noexcept
{
	for (auto& state : _editorTouchStates)
		state.Active = false;
}

bool MidiRouter::IsParameterSuppressed(const vst::IVstPlugin* plugin,
	unsigned int paramIdx,
	std::uint32_t blockStartSample) noexcept
{
	if (!plugin)
		return false;

	const auto count = _automationSuppressionCount.load(std::memory_order_acquire);
	for (std::uint8_t i = 0u; i < count; ++i)
	{
		const auto& slot = _automationSuppressions[i];
		if (slot.Plugin.load(std::memory_order_relaxed) != plugin)
			continue;
		if (slot.ParamIndex.load(std::memory_order_relaxed) != paramIdx)
			continue;

		const auto expiry = slot.ExpirySample.load(std::memory_order_relaxed);
		// Signed sample-domain comparison tolerates uint32 wraparound.
		if (static_cast<std::int32_t>(expiry - blockStartSample) > 0)
			return true;
	}
	return false;
}

void MidiRouter::RefreshAutomationSuppression(const vst::IVstPlugin* plugin,
	unsigned int paramIdx,
	std::uint32_t nowSample,
	std::uint32_t expirySample) noexcept
{
	if (!plugin)
		return;

	const auto count = _automationSuppressionCount.load(std::memory_order_relaxed);

	// 1) Refresh an existing entry for this exact (plugin, parameter).
	for (std::uint8_t i = 0u; i < count; ++i)
	{
		auto& slot = _automationSuppressions[i];
		if (slot.Plugin.load(std::memory_order_relaxed) == plugin
			&& slot.ParamIndex.load(std::memory_order_relaxed) == paramIdx)
		{
			slot.ExpirySample.store(expirySample, std::memory_order_release);
			return;
		}
	}

	// 2) Reclaim an already-expired slot.
	for (std::uint8_t i = 0u; i < count; ++i)
	{
		auto& slot = _automationSuppressions[i];
		const auto expiry = slot.ExpirySample.load(std::memory_order_relaxed);
		if (static_cast<std::int32_t>(expiry - nowSample) <= 0)
		{
			slot.Plugin.store(plugin, std::memory_order_relaxed);
			slot.ParamIndex.store(paramIdx, std::memory_order_relaxed);
			slot.ExpirySample.store(expirySample, std::memory_order_release);
			return;
		}
	}

	// 3) Claim a fresh slot, publishing the fields before bumping the count.
	if (count < MaxAutomationSuppressions)
	{
		auto& slot = _automationSuppressions[count];
		slot.Plugin.store(plugin, std::memory_order_relaxed);
		slot.ParamIndex.store(paramIdx, std::memory_order_relaxed);
		slot.ExpirySample.store(expirySample, std::memory_order_release);
		_automationSuppressionCount.store(static_cast<std::uint8_t>(count + 1u), std::memory_order_release);
		return;
	}

	// 4) Table full (16 distinct live parameters): overwrite the first slot.
	auto& slot = _automationSuppressions[0];
	slot.Plugin.store(plugin, std::memory_order_relaxed);
	slot.ParamIndex.store(paramIdx, std::memory_order_relaxed);
	slot.ExpirySample.store(expirySample, std::memory_order_release);
}

void MidiRouter::ResetAutomationSuppressionForTest() noexcept
{
	for (auto& slot : _automationSuppressions)
	{
		slot.Plugin.store(nullptr, std::memory_order_relaxed);
		slot.ParamIndex.store(0u, std::memory_order_relaxed);
		slot.ExpirySample.store(0u, std::memory_order_relaxed);
	}
	_automationSuppressionCount.store(0u, std::memory_order_release);
}

void MidiRouter::InitMidi(const io::UserConfig& cfg,
	const base::LoggingConfig& loggingConfig,
	std::atomic<std::uint64_t>& audioSampleCounter,
	std::atomic<std::int64_t>& midiAnchorMicros)
{
	CloseMidi();

	midiAnchorMicros.store(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);

	if (cfg.Midi.Devices.empty())
	{
		std::cout << "[MIDI] No MIDI devices configured." << std::endl;
		_PublishMidiTriggerRoutes();
		return;
	}

	const auto sampleRate = cfg.Audio.SampleRate;
	auto midiInputs = std::make_shared<std::vector<std::shared_ptr<MidiInputEndpoint>>>();
	std::uint8_t nextSlot = 0u;

	for (const auto& midiConfig : cfg.Midi.Devices)
	{
		if (!midiConfig.Enabled)
		{
			std::cout << "[MIDI] Device \"" << midiConfig.Name << "\" disabled by rig settings." << std::endl;
			continue;
		}

		if (nextSlot == UnresolvedMidiDeviceSlot)
		{
			std::cout << "[MIDI] Too many enabled MIDI input devices; remaining devices ignored." << std::endl;
			break;
		}

		auto endpoint = std::make_shared<MidiInputEndpoint>();
		endpoint->ConfiguredName = midiConfig.Name.empty() ? "default" : midiConfig.Name;
		endpoint->Device = std::make_unique<midi::MidiDevice>();

		auto opened = endpoint->Device->Open(
			endpoint->ConfiguredName,
			[endpoint, sampleRate, audioSampleCounter = &audioSampleCounter, midiAnchorMicros = &midiAnchorMicros](std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
			{
				midi::MidiEvent ingress{};
				const auto nowMicros = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				const auto anchorSample = audioSampleCounter->load(std::memory_order_acquire);
				const auto anchorMicros = midiAnchorMicros->load(std::memory_order_acquire);
				const auto mappedSample = midi::MapMidiTimestampToAudioSample(sampleRate,
					anchorSample,
					anchorMicros,
					nowMicros);

				ingress.sampleOffset = static_cast<std::uint32_t>(mappedSample);
				ingress.status = status;
				ingress.data1 = data1;
				ingress.data2 = data2;
				ingress._pad = 0u;
				endpoint->Ingress.Push(ingress);
			},
			loggingConfig.Midi == "verbose");

		if (!opened)
			continue;

		endpoint->DeviceSlot = nextSlot++;
		midiInputs->push_back(endpoint);
	}

	_midiInputs.store(midiInputs, std::memory_order_release);

	std::set<std::string> activeMidiInputNames;
	for (const auto& input : *midiInputs)
	{
		if (input)
			activeMidiInputNames.insert(input->ConfiguredName);
	}

	for (auto& route : _midiTriggerRoutes)
	{
		route.DeviceSlot = UnresolvedMidiDeviceSlot;
		for (const auto& input : *midiInputs)
		{
			if (input && (input->ConfiguredName == route.DeviceName))
			{
				route.DeviceSlot = input->DeviceSlot;
				break;
			}
		}

		if (route.DeviceSlot == UnresolvedMidiDeviceSlot)
			std::cout << "[MIDI] No active MIDI input matches trigger device \"" << route.DeviceName << "\"." << std::endl;

		if (route.Trigger)
		{
			for (const auto& midiInputDevice : route.Trigger->MidiInputDevices())
			{
				if (!midiInputDevice.empty() && (activeMidiInputNames.find(midiInputDevice) == activeMidiInputNames.end()))
				{
					std::cout << "[MIDI] No active MIDI input matches loop-record device \""
						<< midiInputDevice << "\" for trigger \"" << route.Trigger->Name() << "\"." << std::endl;
				}
			}
		}
	}

	_PublishMidiTriggerRoutes();

	if (midiInputs->empty())
		std::cout << "[MIDI] No active MIDI input connection." << std::endl;
}

void MidiRouter::CloseMidi()
{
	for (auto& route : _midiTriggerRoutes)
		route.DeviceSlot = UnresolvedMidiDeviceSlot;
	_PublishMidiTriggerRoutes();

	auto midiInputs = _midiInputs.exchange(std::make_shared<const std::vector<std::shared_ptr<MidiInputEndpoint>>>(), std::memory_order_acq_rel);
	if (!midiInputs)
		return;

	for (const auto& input : *midiInputs)
	{
		if (input && input->Device)
			input->Device->Close();
	}
}

void MidiRouter::InitSerial(const io::UserConfig& cfg)
{
	CloseSerial();
	{
		std::scoped_lock lock(_serialIngressMutex);
		_serialIngress.Clear();
	}
	_lastSerialDropCount = 0u;

	if (cfg.Serial.Devices.empty())
		return;

	auto availablePorts = io::SerialDevice::EnumeratePorts();
	std::cout << "[Serial] Ports found: " << availablePorts.size() << std::endl;
	for (const auto& port : availablePorts)
		std::cout << "[Serial]   " << port << std::endl;

	unsigned int activeConnections = 0u;
	for (const auto& serialConfig : cfg.Serial.Devices)
	{
		if (!serialConfig.Enabled)
		{
			std::cout << "[Serial] Device \"" << serialConfig.Name << "\" disabled by rig settings." << std::endl;
			continue;
		}

		if (serialConfig.Port.empty())
		{
			std::cout << "[Serial] Device \"" << serialConfig.Name << "\" has no port configured." << std::endl;
			continue;
		}

		auto serialDevice = std::make_unique<io::SerialDevice>();
		auto opened = serialDevice->Open(
			serialConfig.Name,
			serialConfig.Port,
			serialConfig.BaudRate,
			[this](const io::SerialTriggerEvent& event)
			{
				std::scoped_lock lock(_serialIngressMutex);
				_serialIngress.Push(event);
			});

		if (!opened)
			continue;

		_serialDevices.push_back(std::move(serialDevice));
		activeConnections++;
	}

	if (0u == activeConnections)
		std::cout << "[Serial] No active serial trigger connections." << std::endl;
}

void MidiRouter::CloseSerial()
{
	for (auto& serialDevice : _serialDevices)
	{
		if (serialDevice)
			serialDevice->Close();
	}

	_serialDevices.clear();
	{
		std::scoped_lock lock(_serialIngressMutex);
		_serialIngress.Clear();
	}
}

void MidiRouter::RegisterTrigger(const std::string& deviceName, std::shared_ptr<engine::Trigger> trigger)
{
	if (!trigger)
		return;

	_midiTriggerRoutes.push_back({ deviceName.empty() ? "default" : deviceName, UnresolvedMidiDeviceSlot, trigger });
	_PublishMidiTriggerRoutes();
}

void MidiRouter::RegisterTriggerForTest(const std::string& deviceName,
	std::shared_ptr<engine::Trigger> trigger,
	std::uint8_t deviceSlot)
{
	if (!trigger)
		return;

	_midiTriggerRoutes.push_back({ deviceName.empty() ? "default" : deviceName, deviceSlot, std::move(trigger) });
	_PublishMidiTriggerRoutes();
}

void MidiRouter::AddMidiInputDeviceForTest(const std::string& deviceName, std::uint8_t deviceSlot)
{
	auto currentInputs = _midiInputs.load(std::memory_order_acquire);
	auto updatedInputs = std::make_shared<std::vector<std::shared_ptr<MidiInputEndpoint>>>();
	if (currentInputs)
		updatedInputs->insert(updatedInputs->end(), currentInputs->begin(), currentInputs->end());

	auto endpoint = std::make_shared<MidiInputEndpoint>();
	endpoint->DeviceSlot = deviceSlot;
	endpoint->ConfiguredName = deviceName;
	updatedInputs->push_back(endpoint);

	_midiInputs.store(updatedInputs, std::memory_order_release);
}

void MidiRouter::PushMidiEventForTest(std::uint8_t deviceSlot,
	std::uint8_t status,
	std::uint8_t data1,
	std::uint8_t data2) noexcept
{
	auto midiInputs = _midiInputs.load(std::memory_order_acquire);
	if (!midiInputs)
		return;

	for (const auto& input : *midiInputs)
	{
		if (!input || (input->DeviceSlot != deviceSlot))
			continue;

		midi::MidiEvent ingress{};
		ingress.sampleOffset = 0u;
		ingress.status = status;
		ingress.data1 = data1;
		ingress.data2 = data2;
		ingress._pad = 0u;
		input->Ingress.Push(ingress);
		break;
	}
}

bool MidiRouter::HasMidiInputDeviceForTest(std::uint8_t deviceSlot) const noexcept
{
	auto midiInputs = _midiInputs.load(std::memory_order_acquire);
	if (!midiInputs)
		return false;

	for (const auto& input : *midiInputs)
	{
		if (input && (input->DeviceSlot == deviceSlot))
			return true;
	}

	return false;
}

void MidiRouter::PushSerialTriggerEventForTest(const io::SerialTriggerEvent& event)
{
	std::scoped_lock lock(_serialIngressMutex);
	_serialIngress.Push(event);
}

MidiRouter::TriggerDispatchSummary MidiRouter::DispatchMidiTriggerEventForTest(std::uint8_t deviceSlot,
	const midi::MidiEvent& event,
	const io::UserConfig& userConfig,
	const audio::AudioStreamParams& audioParams)
{
	return _DispatchMidiTriggerEvent(deviceSlot, event, userConfig, audioParams);
}

void MidiRouter::_ConsumeEditorAutomation(const std::vector<std::shared_ptr<engine::Station>>& stations,
	std::uint64_t globalSampleNow,
	const audio::AudioStreamParams& audioParams) noexcept
{
	// Always advance the sequence cursor so that touches made before Ctrl+Shift+A
	// was pressed are not replayed as soon as record mode is armed.
	const auto seq = vst::_lastTouchedParam.Sequence.load(std::memory_order_acquire);
	const bool newTouch = (seq != _lastEditorAutomationSeq);
	if (newTouch)
		_lastEditorAutomationSeq = seq;

	if (!_automationRecordHeld.load(std::memory_order_acquire))
		return;

	const unsigned int sampleRate = (audioParams.SampleRate > 0u) ? audioParams.SampleRate : 48000u;
	const auto cooldownSamples = static_cast<std::uint32_t>(
		(AutomationSuppressionCooldownMs * static_cast<double>(sampleRate)) / 1000.0);
	const auto nowSample = static_cast<std::uint32_t>(globalSampleNow);
	const auto expirySample = nowSample + cooldownSamples;

	// -----------------------------------------------------------------------
	// Part A — new VST touch: resolve (plugin, param, loop, lane) and replace
	// that parameter's next cool-down-sized future window with a held value.
	// -----------------------------------------------------------------------
	if (newTouch)
	{
		auto* plugin = vst::_lastTouchedParam.Plugin.load(std::memory_order_acquire);
		if (plugin)
		{
			const auto paramIdx = vst::_lastTouchedParam.ParameterIndex.load(std::memory_order_acquire);
			const auto value    = vst::_lastTouchedParam.Value.load(std::memory_order_acquire);

			std::shared_ptr<midi::MidiLoop> targetLoop;
			for (const auto& station : stations)
			{
				if (!station || station->IsRemote())
					continue;
				if (auto loop = station->ResolveEditorAutomationLoop(plugin))
				{
					targetLoop = loop;
					break;
				}
			}

			if (!targetLoop)
			{
				std::cout << "[Automation] editor drag ignored: no recording loop owns plugin "
					<< static_cast<const void*>(plugin) << " (param " << paramIdx << ")\n";
			}
			else
			{
				const auto loopLen = targetLoop->LoopLengthSamps();
				if (loopLen > 0u)
				{
					const auto loopSample = static_cast<std::uint32_t>(
						(nowSample - targetLoop->LoopPhaseAnchor()) % loopLen);
					const auto laneOpt = targetLoop->ResolveAutomationLaneFor(plugin, paramIdx);
					if (!laneOpt)
					{
						std::cout << "[Automation] editor drag ignored: no free automation lane for plugin "
							<< static_cast<const void*>(plugin) << " (param " << paramIdx << ")\n";
					}
					else
					{
						const auto laneIdx = *laneOpt;

						EditorTouchState* touchState = nullptr;
						for (auto& state : _editorTouchStates)
						{
							if (state.Active && state.Plugin == plugin && state.ParamIndex == paramIdx)
							{
								touchState = &state;
								break;
							}
						}
						if (!touchState)
						{
							for (auto& state : _editorTouchStates)
							{
								if (!state.Active)
								{
									touchState = &state;
									break;
								}
							}
						}

						if (!touchState)
						{
							std::cout << "[Automation] editor drag ignored: no free touch-state slot for plugin "
								<< static_cast<const void*>(plugin) << " (param " << paramIdx << ")\n";
						}
						else
						{
							// First touch in this record session: the slot was inactive because
							// Ctrl+Shift+A was just pressed (or because the previous cool-down
							// expired). Each real touch rewrites one bounded future hold window.
							const bool freshDrag = !touchState->Active;
							if (freshDrag)
							{
								touchState->Active     = true;
								touchState->Plugin     = plugin;
								touchState->ParamIndex = paramIdx;
								std::cout << "[Automation] fresh drag: lane " << laneIdx
									<< " overwrite started for param " << paramIdx << "\n";
							}

							touchState->Loop           = targetLoop;
							touchState->LaneIdx        = laneIdx;
							touchState->LastKnownValue = value;
							touchState->LastTouchSample = nowSample;

							if (targetLoop->WireEditorAutomationLane(laneIdx, plugin, paramIdx))
							{
								std::cout << "[Automation] editor drag wired lane " << laneIdx
									<< " -> plugin " << static_cast<const void*>(plugin)
									<< " param " << paramIdx << "\n";
							}

							targetLoop->OverwriteAutomationWindow(laneIdx, loopSample, cooldownSamples, value);

							RefreshAutomationSuppression(plugin, paramIdx, nowSample, expirySample);
						}
					}
				}
			}
		}
	}

	// -----------------------------------------------------------------------
	// Part B — expire stale touch sessions. Between actual VST touch events we
	// only age out state; we do not write more points or extend suppression.
	// -----------------------------------------------------------------------
	for (auto& state : _editorTouchStates)
	{
		if (!state.Active)
			continue;

		if (static_cast<std::int32_t>(nowSample - state.LastTouchSample)
			> static_cast<std::int32_t>(cooldownSamples))
		{
			state.Active = false;
			continue;
		}

		auto loop = state.Loop.lock();
		if (!loop)
		{
			state.Active = false;
			continue;
		}
	}
}

MidiRouter::TriggerDispatchSummary MidiRouter::PumpMidi(const std::vector<std::shared_ptr<engine::Station>>& stations,
	std::uint64_t globalSampleNow,
	const io::UserConfig& userConfig,
	const audio::AudioStreamParams& audioParams) noexcept
{
	TriggerDispatchSummary summary;
	midi::MidiEvent ingress{};
	const auto midiInputs = _midiInputs.load(std::memory_order_acquire);
	if (!midiInputs)
	{
		_ConsumeEditorAutomation(stations, globalSampleNow, audioParams);
		return summary;
	}
	for (const auto& input : *midiInputs)
	{
		if (!input)
			continue;

		while (input->Ingress.Pop(ingress))
		{
			auto dispatch = _DispatchMidiTriggerEvent(input->DeviceSlot, ingress, userConfig, audioParams);
			summary.Activated = summary.Activated || dispatch.Activated;
			summary.Ditched = summary.Ditched || dispatch.Ditched;

			const auto msgType = ingress.MessageType();
			if ((msgType >= 0x80u) && (msgType <= 0xE0u))
			{
				const auto& deviceName = input->ConfiguredName;
				for (const auto& station : stations)
				{
					if (station && !station->IsRemote() && station->AcceptsLiveMidiFromDevice(deviceName))
						station->EnqueueLiveMidiEvent(ingress, deviceName);
				}
			}

			// Control Change handling: MIDI-learn capture and automation recording.
			constexpr std::uint8_t ControlChange = 0xB0u;
			if (msgType == ControlChange)
			{
				const auto channel = ingress.Channel();
				const auto cc = ingress.data1;

				// Learn capture: the user moved a physical knob while in learn mode.
				if (_learnMidiCCMode.load(std::memory_order_relaxed))
				{
					_learnedChannel.store(channel, std::memory_order_relaxed);
					_learnedCC.store(cc, std::memory_order_relaxed);
				}

				// Recording: route the CC value into every mapped lane across stations.
				if (_automationRecordHeld.load(std::memory_order_acquire))
				{
					const auto matchKey = AutomationMapping::MakeMatchKey(channel, cc);
					const float value = static_cast<float>(ingress.data2) / 127.0f;
					for (const auto& station : stations)
					{
						if (!station || station->IsRemote())
							continue;

						for (const auto& take : station->GetLoopTakes())
						{
							if (!take)
								continue;

							for (const auto& loop : take->GetMidiLoops())
							{
								if (!loop)
									continue;

								const auto loopLen = loop->LoopLengthSamps();
								if (loopLen == 0u)
									continue;

							const double frac = std::fmod(
								static_cast<double>(static_cast<std::uint32_t>(globalSampleNow) - loop->LoopPhaseAnchor()),
								static_cast<double>(loopLen)) / static_cast<double>(loopLen);
								for (std::size_t laneIdx = 0u; laneIdx < MidiLoop::MaxAutomationLanes; ++laneIdx)
								{
									auto& lane = loop->GetLane(laneIdx);
									if (lane.Mapping.MatchKey.load(std::memory_order_relaxed) == matchKey)
										loop->SetAutomationValueAtFrac(laneIdx, frac, value);
								}
							}
						}
					}
				}
			}

			if ((msgType != midi::MidiEvent::NoteOn) && (msgType != midi::MidiEvent::NoteOff))
				continue;

			for (const auto& station : stations)
			{
				for (const auto& take : station->GetLoopTakes())
				{
					if (take->IsArmed())
						take->RecordMidiEvent(ingress, input->ConfiguredName, static_cast<std::uint32_t>(globalSampleNow));
				}
			}
		}

		auto dropped = input->Ingress.DroppedCount();
		if (dropped != input->LastDroppedCount)
		{
			std::cout << "[MIDI] Ingress queue dropped " << (dropped - input->LastDroppedCount)
				<< " event(s) on device \"" << input->ConfiguredName
				<< "\", total dropped=" << dropped << std::endl;
			input->LastDroppedCount = dropped;
		}
	}

	_ConsumeEditorAutomation(stations, globalSampleNow, audioParams);

	return summary;
}

MidiRouter::TriggerDispatchSummary MidiRouter::PumpSerial(const std::vector<std::shared_ptr<engine::Station>>& stations,
	const io::UserConfig& userConfig,
	const audio::AudioStreamParams& audioParams) noexcept
{
	TriggerDispatchSummary summary;
	static const std::string EmptyDevice;
	while (true)
	{
		io::SerialTriggerEvent ev{};
		{
			std::scoped_lock lock(_serialIngressMutex);
			if (!_serialIngress.Pop(ev))
				break;
		}

		base::Action action;
		action.SetActionTime(utils::Timer::GetTime());
		action.SetUserConfig(userConfig);
		action.SetAudioParams(audioParams);
		const auto& device = ev.Device ? *ev.Device : EmptyDevice;

		for (const auto& station : stations)
		{
			auto res = station->OnTriggerEvent(
				engine::TriggerSource::TRIGGER_SERIAL,
				ev.ButtonIndex,
				ev.IsPressed ? 1u : 0u,
				action,
				device);
			if (!res.IsEaten)
				continue;
			if (res.ResultType == actions::ACTIONRESULT_ACTIVATE)
				summary.Activated = true;
			else if (res.ResultType == actions::ACTIONRESULT_DITCH)
				summary.Ditched = true;
		}
	}

	std::uint64_t dropped = 0u;
	{
		std::scoped_lock lock(_serialIngressMutex);
		dropped = _serialIngress.DroppedCount();
	}
	if (dropped != _lastSerialDropCount)
	{
		std::cout << "[Serial] Ingress queue dropped " << (dropped - _lastSerialDropCount)
			<< " event(s), total dropped=" << dropped << std::endl;
		_lastSerialDropCount = dropped;
	}

	return summary;
}

MidiRouter::TriggerDispatchSummary MidiRouter::_DispatchMidiTriggerEvent(std::uint8_t deviceSlot,
	const midi::MidiEvent& event,
	const io::UserConfig& userConfig,
	const audio::AudioStreamParams& audioParams)
{
	TriggerDispatchSummary summary;
	base::Action triggerAction;
	triggerAction.SetUserConfig(userConfig);
	triggerAction.SetAudioParams(audioParams);
	triggerAction.SetActionTime(utils::Timer::GetTime());

	auto routes = _midiTriggerRoutesSnapshot.load(std::memory_order_acquire);
	if (!routes)
		return summary;

	for (const auto& route : *routes)
	{
		if ((route.DeviceSlot != deviceSlot) || !route.Trigger)
			continue;

		auto res = route.Trigger->OnEvent(event, triggerAction);
		if (!res.IsEaten)
			continue;

		if (res.ResultType == actions::ACTIONRESULT_ACTIVATE)
			summary.Activated = true;
		else if (res.ResultType == actions::ACTIONRESULT_DITCH)
			summary.Ditched = true;

		std::cout << "[MIDI Trigger] trigger=\"" << route.Trigger->Name()
			<< "\" " << engine::Trigger::ActionLabel(res.ResultType)
			<< midi::MidiEvent::Direction(event) << " (";
		midi::MidiEvent::LogDetail(std::cout, route.DeviceSlot, event);
		std::cout << ")\n";
	}

	return summary;
}

void MidiRouter::_PublishMidiTriggerRoutes()
{
	auto routes = std::make_shared<const std::vector<MidiTriggerRoute>>(_midiTriggerRoutes.begin(), _midiTriggerRoutes.end());
	_midiTriggerRoutesSnapshot.store(routes, std::memory_order_release);
}