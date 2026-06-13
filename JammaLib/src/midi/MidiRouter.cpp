#include "MidiRouter.h"

#include <chrono>
#include <iostream>
#include <set>
#include "../engine/Station.h"
#include "../engine/Trigger.h"
#include "../io/UserConfig.h"
#include "MidiTimestampMapper.h"

using namespace midi;

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

MidiRouter::TriggerDispatchSummary MidiRouter::PumpMidi(const std::vector<std::shared_ptr<engine::Station>>& stations,
	std::uint64_t globalSampleNow,
	const io::UserConfig& userConfig,
	const audio::AudioStreamParams& audioParams) noexcept
{
	TriggerDispatchSummary summary;
	midi::MidiEvent ingress{};
	const auto midiInputs = _midiInputs.load(std::memory_order_acquire);
	if (!midiInputs)
		return summary;

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