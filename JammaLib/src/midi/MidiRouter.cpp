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
#include "MidiBlockTiming.h"
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
		const auto takes = station->GetLoopTakeSnapshot();
		if (!takes.empty())
			take = takes.front();
	}
	if (!take)
		return { nullptr, nullptr };

	const auto midiLoops = take->GetMidiLoopSnapshot();
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

	constexpr unsigned int InsertKey = 45u;
	if (InsertKey == action.KeyChar)
	{
		if (isDown && !_automationRecordKeyHeld)
		{
			_automationRecordKeyHeld = true;
			_ResetEditorTouchStates();
			_automationRecordHeld.store(true, std::memory_order_release);
			std::cout << ">> Automation record armed (Insert) <<" << std::endl;
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

actions::ActionResult MidiRouter::HandleChannelOverrideKey(const actions::KeyAction& action,
	const std::vector<std::shared_ptr<engine::Station>>& stations)
{
	constexpr unsigned int PageUpKey = 0x21u;
	constexpr unsigned int PageDownKey = 0x22u;
	const bool isPageUp = (action.KeyChar == PageUpKey);
	const bool isPageDown = (action.KeyChar == PageDownKey);
	if (!isPageUp && !isPageDown)
	{
		return actions::ActionResult::NoAction();
	}

	auto eaten = actions::ActionResult::NoAction();
	eaten.IsEaten = true;

	if (action.KeyActionType == actions::KeyAction::KEY_UP)
	{
		if (isPageUp)
			_channelOverridePageUpHeld = false;
		if (isPageDown)
			_channelOverridePageDownHeld = false;
		return eaten;
	}

	if (action.KeyActionType != actions::KeyAction::KEY_DOWN)
		return eaten;

	const bool wasPageUpHeld = _channelOverridePageUpHeld;
	const bool wasPageDownHeld = _channelOverridePageDownHeld;

	if (isPageUp)
		_channelOverridePageUpHeld = true;
	if (isPageDown)
		_channelOverridePageDownHeld = true;

	if (_channelOverridePageUpHeld && _channelOverridePageDownHeld &&
		!(wasPageUpHeld && wasPageDownHeld))
	{
		ResetChannelOverride();
		return eaten;
	}

	if (isPageUp && !wasPageUpHeld)
	{
		for (const auto& station : stations)
		{
			if (station && !station->IsRemote())
				station->FlushLiveHeldMidiNotes();
		}
		StepChannelOverrideUp();
	}
	else if (isPageDown && !wasPageDownHeld)
	{
		for (const auto& station : stations)
		{
			if (station && !station->IsRemote())
				station->FlushLiveHeldMidiNotes();
		}
		StepChannelOverrideDown();
	}

	return eaten;
}

void MidiRouter::StepChannelOverrideUp() noexcept
{
	const auto current = ForcedChannelOverride();
	if (current < 16u)
	{
		const auto next = static_cast<std::uint8_t>(current + 1u);
		_forcedInputChannelOverride.store(next, std::memory_order_release);
		const auto config = _liveMidiDispatchNotification ? _liveMidiDispatchNotification->InputConfig.load(std::memory_order_acquire) : 0u;
		_PublishLiveMidiInputConfig(_LiveMidiConfigGeneration(config), next);
	}
}

void MidiRouter::StepChannelOverrideDown() noexcept
{
	const auto current = ForcedChannelOverride();
	if (current > 0u)
	{
		const auto next = static_cast<std::uint8_t>(current - 1u);
		_forcedInputChannelOverride.store(next, std::memory_order_release);
		const auto config = _liveMidiDispatchNotification ? _liveMidiDispatchNotification->InputConfig.load(std::memory_order_acquire) : 0u;
		_PublishLiveMidiInputConfig(_LiveMidiConfigGeneration(config), next);
	}
}

void MidiRouter::ResetChannelOverride() noexcept
{
	_forcedInputChannelOverride.store(0u, std::memory_order_release);
	const auto config = _liveMidiDispatchNotification ? _liveMidiDispatchNotification->InputConfig.load(std::memory_order_acquire) : 0u;
	_PublishLiveMidiInputConfig(_LiveMidiConfigGeneration(config), 0u);
}

void MidiRouter::SetForcedChannelOverride(std::uint8_t forcedChannelOverride,
	const std::vector<std::shared_ptr<engine::Station>>& stations) noexcept
{
	const auto clamped = (std::min)(forcedChannelOverride, static_cast<std::uint8_t>(16u));
	const auto current = ForcedChannelOverride();
	if (current == clamped)
		return;

	for (const auto& station : stations)
	{
		if (station && !station->IsRemote())
			station->FlushLiveHeldMidiNotes();
	}

	_forcedInputChannelOverride.store(clamped, std::memory_order_release);
	const auto config = _liveMidiDispatchNotification ? _liveMidiDispatchNotification->InputConfig.load(std::memory_order_acquire) : 0u;
	_PublishLiveMidiInputConfig(_LiveMidiConfigGeneration(config), clamped);
}

std::uint8_t MidiRouter::ForcedChannelOverride() const noexcept
{
	return _forcedInputChannelOverride.load(std::memory_order_acquire);
}

std::uint8_t MidiRouter::RewriteIncomingChannel(std::uint8_t status, std::uint8_t forcedChannelOverride) noexcept
{
	if (forcedChannelOverride == 0u)
		return status;

	const auto messageType = static_cast<std::uint8_t>(status & midi::MidiEvent::StatusMask);
	if ((messageType < 0x80u) || (messageType > 0xE0u))
		return status;

	return static_cast<std::uint8_t>(messageType | ((forcedChannelOverride - 1u) & midi::MidiEvent::ChannelMask));
}

bool MidiRouter::IsAutomationRecordHeld() noexcept
{
	return _automationRecordHeld.load(std::memory_order_acquire);
}

void MidiRouter::_ResetEditorTouchStates() noexcept
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

void MidiRouter::InitMidi(const io::UserConfig& cfg,
	const base::LoggingConfig& loggingConfig,
	midi::MidiClockAnchor& midiClockAnchor)
{
	CloseMidi();

	const auto initialAnchor = midi::ReadMidiClockAnchor(midiClockAnchor, {});
	_liveMidiDispatchNotification = std::make_shared<LiveMidiDispatchNotification>();
	_liveMidiDispatchNotification->WorkEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	_liveMidiStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!_liveMidiDispatchNotification->WorkEvent || !_liveMidiStopEvent)
	{
		CloseMidi();
		return;
	}
	_PublishLiveMidiInputConfig(++_nextLiveMidiRoutingGeneration, ForcedChannelOverride());

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
			[endpoint, sampleRate, midiClockAnchor = &midiClockAnchor, notification = _liveMidiDispatchNotification](std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
			{
				midi::MidiEvent ingress{};
				const auto nowMicros = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				const auto anchor = midi::ReadMidiClockAnchor(*midiClockAnchor, endpoint->LastClockAnchor);
				endpoint->LastClockAnchor = anchor;
				const auto mappedSample = midi::MapMidiTimestampToAudioSample(sampleRate,
					anchor.Sample,
					anchor.SteadyMicros,
					nowMicros);

				ingress.sampleOffset = static_cast<std::uint32_t>(mappedSample);
				const auto inputConfig = notification->InputConfig.load(std::memory_order_acquire);
				ingress.status = RewriteIncomingChannel(status, _LiveMidiConfigForcedChannel(inputConfig));
				ingress.data1 = data1;
				ingress.data2 = data2;
				ingress._pad = 0u;
				endpoint->Ingress.Push(ingress);
				endpoint->LiveIngress.Push({ ingress,
					_LiveMidiConfigGeneration(inputConfig), endpoint->NextLiveSequence++ });
				SetEvent(notification->WorkEvent);
			},
			loggingConfig.Midi == "verbose");

		if (!opened)
			continue;

		endpoint->LastClockAnchor = initialAnchor;
		endpoint->DeviceSlot = nextSlot++;
		midiInputs->push_back(endpoint);
	}

	_midiInputs.store(midiInputs, std::memory_order_release);
	_StartLiveMidiDispatcher();

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
	_PublishLiveMidiInputConfig(++_nextLiveMidiRoutingGeneration, ForcedChannelOverride());
	_liveMidiRoutes.store(std::make_shared<const LiveMidiRoutingSnapshot>(), std::memory_order_release);
	for (auto& route : _midiTriggerRoutes)
		route.DeviceSlot = UnresolvedMidiDeviceSlot;
	_PublishMidiTriggerRoutes();

	auto midiInputs = _midiInputs.exchange(std::make_shared<const std::vector<std::shared_ptr<MidiInputEndpoint>>>(), std::memory_order_acq_rel);
	if (midiInputs)
	{
		for (const auto& input : *midiInputs)
		{
			if (input && input->Device)
				input->Device->Close();
		}
	}

	_StopLiveMidiDispatcher();
}

void MidiRouter::PublishLiveMidiRoutes(const std::vector<std::shared_ptr<engine::Station>>& stations)
{
	auto routes = std::make_shared<LiveMidiRoutingSnapshot>();
	routes->Generation = ++_nextLiveMidiRoutingGeneration;

	const auto midiInputs = _midiInputs.load(std::memory_order_acquire);
	if (midiInputs)
	{
		routes->RecipientsByDeviceSlot.resize(midiInputs->size());
		for (const auto& input : *midiInputs)
		{
			if (!input || input->DeviceSlot >= routes->RecipientsByDeviceSlot.size())
				continue;

			auto& recipients = routes->RecipientsByDeviceSlot[input->DeviceSlot];
			for (const auto& station : stations)
			{
				if (!station || station->IsRemote() || !station->AcceptsLiveMidiFromDevice(input->ConfiguredName))
					continue;

				std::uint16_t channelMask = 0u;
				for (std::uint8_t channel = 0u; channel < 16u; ++channel)
				{
					if (station->AcceptsLiveMidiChannel(channel))
						channelMask = static_cast<std::uint16_t>(channelMask | (1u << channel));
				}
				if (channelMask != 0u)
					recipients.push_back({ station, channelMask });
			}
		}
	}

	_liveMidiRoutes.store(routes, std::memory_order_release);
	_PublishLiveMidiInputConfig(routes->Generation, ForcedChannelOverride());
	if (_liveMidiDispatchNotification && _liveMidiDispatchNotification->WorkEvent)
		SetEvent(_liveMidiDispatchNotification->WorkEvent);
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

void MidiRouter::_ConsumeEditorAutomation(const std::vector<std::shared_ptr<engine::Station>>& stations,
	std::uint64_t globalSampleNow,
	const audio::AudioStreamParams& audioParams) noexcept
{
	// Always advance the sequence cursor so that touches made before Insert
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

			std::shared_ptr<engine::Station> targetStation;
			std::shared_ptr<midi::MidiLoop> targetLoop;
			for (const auto& station : stations)
			{
				if (!station || station->IsRemote())
					continue;
				if (auto loop = station->ResolveEditorAutomationLoop(plugin))
				{
					targetLoop = loop;
					targetStation = station;
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
					const auto correction = targetStation
						? targetStation->ResolveMidiAnchorCorrectionFor(targetLoop.get()) : 0;
					const auto loopSample = static_cast<std::uint32_t>(
						(nowSample - targetLoop->LoopPhaseAnchor() - static_cast<std::uint32_t>(correction)) % loopLen);
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
							// Insert was just pressed (or because the previous cool-down
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
			const auto& effectiveIngress = ingress;

			auto dispatch = _DispatchMidiTriggerEvent(input->DeviceSlot, effectiveIngress, userConfig, audioParams);
			summary.Activated = summary.Activated || dispatch.Activated;
			summary.Ditched = summary.Ditched || dispatch.Ditched;

			const auto msgType = effectiveIngress.MessageType();
			if ((msgType >= 0x80u) && (msgType <= 0xE0u))
			{
				const auto& deviceName = input->ConfiguredName;
				for (const auto& station : stations)
				{
					if (station && !station->IsRemote() && station->AcceptsLiveMidiFromDevice(deviceName))
					{
						station->ObservePhysicalMidiForRecording(effectiveIngress, deviceName);
					}
				}
			}

			// Control Change handling: MIDI-learn capture and automation recording.
			constexpr std::uint8_t ControlChange = 0xB0u;
			if (msgType == ControlChange)
			{
				const auto channel = effectiveIngress.Channel();
				const auto cc = effectiveIngress.data1;

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
					const float value = static_cast<float>(effectiveIngress.data2) / 127.0f;
					for (const auto& station : stations)
					{
						if (!station || station->IsRemote())
							continue;

						for (const auto& take : station->GetLoopTakeSnapshot())
						{
							if (!take)
								continue;

							for (const auto& loop : take->GetMidiLoopSnapshot())
							{
								if (!loop)
									continue;

								const auto loopLen = loop->LoopLengthSamps();
								if (loopLen == 0u)
									continue;

							const double frac = std::fmod(
							static_cast<double>(static_cast<std::uint32_t>(globalSampleNow)
								- loop->LoopPhaseAnchor()
								- static_cast<std::uint32_t>(take->MidiAnchorCorrection())),
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
				for (const auto& take : station->GetLoopTakeSnapshot())
				{
					if (take->IsArmed())
						take->RecordMidiEvent(effectiveIngress, input->ConfiguredName, static_cast<std::uint32_t>(globalSampleNow));
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

std::uint64_t MidiRouter::_PackLiveMidiInputConfig(std::uint32_t generation,
	std::uint8_t forcedChannelOverride) noexcept
{
	return (static_cast<std::uint64_t>(generation) << 8u)
		| static_cast<std::uint64_t>(forcedChannelOverride);
}

std::uint32_t MidiRouter::_LiveMidiConfigGeneration(std::uint64_t config) noexcept
{
	return static_cast<std::uint32_t>(config >> 8u);
}

std::uint8_t MidiRouter::_LiveMidiConfigForcedChannel(std::uint64_t config) noexcept
{
	return static_cast<std::uint8_t>(config & LiveInputConfigChannelMask);
}

void MidiRouter::_PublishLiveMidiInputConfig(std::uint32_t generation,
	std::uint8_t forcedChannelOverride) noexcept
{
	if (_liveMidiDispatchNotification)
	{
		_liveMidiDispatchNotification->InputConfig.store(
			_PackLiveMidiInputConfig(generation, forcedChannelOverride), std::memory_order_release);
	}
}

void MidiRouter::_StartLiveMidiDispatcher()
{
	if (!_liveMidiDispatchNotification || !_liveMidiDispatchNotification->WorkEvent || !_liveMidiStopEvent)
		return;

	_liveMidiDispatchThread = std::thread([this]() { _LiveMidiDispatchLoop(); });
}

void MidiRouter::_StopLiveMidiDispatcher()
{
	if (_liveMidiStopEvent)
		SetEvent(_liveMidiStopEvent);
	if (_liveMidiDispatchThread.joinable())
		_liveMidiDispatchThread.join();

	if (_liveMidiStopEvent)
		CloseHandle(_liveMidiStopEvent);
	_liveMidiStopEvent = nullptr;
	if (_liveMidiDispatchNotification && _liveMidiDispatchNotification->WorkEvent)
		CloseHandle(_liveMidiDispatchNotification->WorkEvent);
	_liveMidiDispatchNotification.reset();
}

void MidiRouter::_LiveMidiDispatchLoop() noexcept
{
	_DispatchAvailableLiveMidi();
	const HANDLE handles[] = { _liveMidiDispatchNotification->WorkEvent, _liveMidiStopEvent };
	for (;;)
	{
		const auto waitResult = WaitForMultipleObjects(2u, handles, FALSE, INFINITE);
		if (waitResult == WAIT_OBJECT_0 + 1u || waitResult == WAIT_FAILED)
			break;
		if (waitResult != WAIT_OBJECT_0)
			continue;
		_DispatchAvailableLiveMidi();
	}
	_DispatchAvailableLiveMidi();
}

void MidiRouter::_DispatchAvailableLiveMidi() noexcept
{
	const auto midiInputs = _midiInputs.load(std::memory_order_acquire);
	if (!midiInputs)
		return;

	for (std::size_t dispatched = 0u; dispatched < MaxLiveMidiEventsPerDispatchPass; ++dispatched)
	{
		std::shared_ptr<MidiInputEndpoint> selectedInput;
		LiveMidiIngressEvent selectedEvent{};
		for (const auto& input : *midiInputs)
		{
			LiveMidiIngressEvent candidate{};
			if (!input || !input->LiveIngress.Peek(candidate))
				continue;

			if (!selectedInput
				|| static_cast<std::int32_t>(candidate.Event.sampleOffset - selectedEvent.Event.sampleOffset) < 0
				|| (candidate.Event.sampleOffset == selectedEvent.Event.sampleOffset
					&& (input->DeviceSlot < selectedInput->DeviceSlot
						|| (input->DeviceSlot == selectedInput->DeviceSlot && candidate.Sequence < selectedEvent.Sequence))))
			{
				selectedInput = input;
				selectedEvent = candidate;
			}
		}

		if (!selectedInput)
			return;

		LiveMidiIngressEvent dispatchedEvent{};
		if (!selectedInput->LiveIngress.Pop(dispatchedEvent))
			continue;

		const auto routes = _liveMidiRoutes.load(std::memory_order_acquire);
		if (!routes || dispatchedEvent.RoutingGeneration != routes->Generation)
			continue;
		if (selectedInput->DeviceSlot >= routes->RecipientsByDeviceSlot.size())
			continue;

		const auto channelBit = static_cast<std::uint16_t>(1u << dispatchedEvent.Event.Channel());
		for (const auto& recipient : routes->RecipientsByDeviceSlot[selectedInput->DeviceSlot])
		{
			if (recipient.Station && (recipient.AllowedChannelMask & channelBit) != 0u)
				recipient.Station->TryEnqueueImmediateLiveMidi(dispatchedEvent.Event);
		}
	}

	if (_liveMidiDispatchNotification && _liveMidiDispatchNotification->WorkEvent)
		SetEvent(_liveMidiDispatchNotification->WorkEvent);
}