#include "MidiDevice.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>

#include "rtmidi/RtMidi.h"

using namespace midi;

void MidiDevice::_LogMidiMessageDetail(std::ostream& out,
	const unsigned char* message, std::size_t size)
{
	if (size == 0u)
		return;

	constexpr std::uint8_t StatusMask    = 0xF0;
	constexpr std::uint8_t ChannelMask   = 0x0F;
	constexpr std::uint8_t NoteOff       = 0x80;
	constexpr std::uint8_t NoteOn        = 0x90;
	constexpr std::uint8_t CC            = 0xB0;
	constexpr std::uint8_t ProgramChange = 0xC0;

	const auto status = static_cast<std::uint8_t>(message[0]);
	const auto data1  = static_cast<std::uint8_t>(size > 1 ? message[1] : 0u);
	const auto data2  = static_cast<std::uint8_t>(size > 2 ? message[2] : 0u);
	const int  chan   = (status & ChannelMask) + 1;

	out << "  (chan " << chan << ", ";

	switch (status & StatusMask)
	{
	case NoteOn:
		out << (data2 != 0 ? "noteon" : "noteoff") << ": " << static_cast<int>(data1);
		break;
	case NoteOff:
		out << "noteoff: " << static_cast<int>(data1);
		break;
	case CC:
		out << "cc " << static_cast<int>(data1) << ": " << static_cast<int>(data2);
		break;
	case ProgramChange:
		out << "pc: " << static_cast<int>(data1);
		break;
	default:
		out << "0x" << std::hex << std::uppercase << static_cast<int>(status) << std::dec;
		break;
	}

	out << ")";
}

MidiDevice::MidiDevice()
	: _midiIn(nullptr),
	  _isOpen(false),
	  _deviceName(""),
	  _deviceId(0u),
	  _loggingVerbose(false),
	  _callback()
{
}

MidiDevice::~MidiDevice()
{
	Close();
}

std::vector<MidiInputDeviceInfo> MidiDevice::EnumerateInputDevices()
{
	return InventoryInputDevices().Devices;
}

MidiInputInventory MidiDevice::InventoryInputDevices()
{
	MidiInputInventory inventory;

	try
	{
		rt::midi::RtMidiIn midiIn;
		const auto count = midiIn.getPortCount();
		inventory.Devices.reserve(count);

		for (unsigned int i = 0; i < count; ++i)
			inventory.Devices.push_back({ i, midiIn.getPortName(i) });
	}
	catch (const rt::midi::RtMidiError& err)
	{
		inventory.Error = err.getMessage();
	}
	catch (const std::exception& err)
	{
		inventory.Error = err.what();
	}

	return inventory;
}

const MidiInputDeviceInfo* MidiDevice::FindExactInput(
	const std::vector<MidiInputDeviceInfo>& devices, const std::string& name) noexcept
{
	const auto found = std::find_if(devices.begin(), devices.end(),
		[&name](const MidiInputDeviceInfo& device) { return device.Name == name; });
	return found == devices.end() ? nullptr : &*found;
}

const MidiInputDeviceInfo* MidiDevice::SelectUnclaimedExactInput(
	const std::vector<MidiInputDeviceInfo>& devices, const std::string& name,
	const std::vector<unsigned int>& claimedIds, bool& ambiguous) noexcept
{
	const MidiInputDeviceInfo* selected = nullptr;
	unsigned int matchingNames = 0u;
	for (const auto& device : devices)
	{
		if (device.Name != name)
			continue;
		++matchingNames;
		if (!selected && std::find(claimedIds.begin(), claimedIds.end(), device.DeviceId) == claimedIds.end())
			selected = &device;
	}
	ambiguous = matchingNames > 1u;
	return selected;
}

const MidiInputDeviceInfo* MidiDevice::SelectStartupInput(
	const std::vector<MidiInputDeviceInfo>& devices, const std::string& name,
	const std::vector<unsigned int>& claimedIds, size_t requestIndex,
	bool generatedRig, bool& ambiguous) noexcept
{
	if (!generatedRig)
		return SelectUnclaimedExactInput(devices, name, claimedIds, ambiguous);
	ambiguous = std::count_if(devices.begin(), devices.end(),
		[&name](const MidiInputDeviceInfo& device) { return device.Name == name; }) > 1u;
	return requestIndex < devices.size() && devices[requestIndex].Name == name
		? &devices[requestIndex] : nullptr;
}

bool MidiDevice::OpenPort(const MidiInputDeviceInfo& port, MidiMessageCallback callback,
	std::string& error, bool loggingVerbose)
{
	Close();
	_verbosePackets.Clear();
	_lastVerboseDroppedCount = 0u;
	_callback = std::move(callback);
	_loggingVerbose = loggingVerbose;
	error.clear();

	try
	{
		_midiIn = std::make_unique<rt::midi::RtMidiIn>();
		const auto count = _midiIn->getPortCount();
		if (port.DeviceId >= count || _midiIn->getPortName(port.DeviceId) != port.Name)
		{
			error = "MIDI port changed after inventory";
			_midiIn.reset();
			_callback = MidiMessageCallback();
			return false;
		}
		_midiIn->ignoreTypes(false, false, false);
		if (_callback)
			_midiIn->setCallback(&MidiDevice::_RtMidiCallback, this);
		_midiIn->openPort(port.DeviceId, "Jamma MIDI In");
	}
	catch (const rt::midi::RtMidiError& err)
	{
		error = err.getMessage();
		_callback = MidiMessageCallback();
		_midiIn.reset();
		return false;
	}
	catch (const std::exception& err)
	{
		error = err.what();
		_callback = MidiMessageCallback();
		_midiIn.reset();
		return false;
	}
	_deviceName = port.Name;
	_deviceId = port.DeviceId;
	_isOpen = true;

	std::cout << "[MIDI] Connected input device #" << _deviceId
	          << " (" << _deviceName << ")" << std::endl;
	return true;
}

bool MidiDevice::Open(const std::string& preferredDeviceName,
	MidiMessageCallback callback, bool loggingVerbose)
{
	const auto inventory = InventoryInputDevices();
	const auto* port = FindExactInput(inventory.Devices, preferredDeviceName);
	if (!port)
	{
		Close();
		std::cout << "[MIDI] Exact input device missing: \"" << preferredDeviceName << "\""
			<< (inventory.Error.empty() ? "" : " (inventory error: " + inventory.Error + ")") << std::endl;
		return false;
	}
	std::string error;
	const auto opened = OpenPort(*port, std::move(callback), error, loggingVerbose);
	if (!opened)
		std::cout << "[MIDI] Failed to open input device #" << port->DeviceId
			<< " (" << port->Name << "): " << error << std::endl;
	return opened;
}

void MidiDevice::Close()
{
	if (!_midiIn)
		return;

	try
	{
		_midiIn->cancelCallback();
		if (_midiIn->isPortOpen())
			_midiIn->closePort();
	}
	catch (const rt::midi::RtMidiError& err)
	{
		std::cout << "[MIDI] Error while closing MIDI input: " << err.getMessage() << std::endl;
	}

	if (_isOpen)
	{
		std::cout << "[MIDI] Disconnected input device #" << _deviceId
		          << " (" << _deviceName << ")" << std::endl;
	}

	_callback = MidiMessageCallback();
	_midiIn.reset();
	_isOpen = false;
	_deviceName.clear();
	_deviceId = 0u;
}

void MidiDevice::_RtMidiCallback(double deltaSeconds,
                                 std::vector<unsigned char>* message,
                                 void* userData)
{
	const auto callbackArrivalMicros = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	if ((nullptr == userData) || (nullptr == message))
		return;

	auto self = reinterpret_cast<MidiDevice*>(userData);
	self->_OnMidiData(*message, deltaSeconds, callbackArrivalMicros);
}

void MidiDevice::DrainVerbosePackets(const std::string& configuredName)
{
	VerbosePacket packet;
	while (_verbosePackets.Pop(packet))
	{
		std::cout << "[MIDI] Device \"" << configuredName << "\" packet: ";
		for (std::size_t i = 0; i < packet.Size; ++i)
		{
			if (i > 0u) std::cout << ' ';
			std::cout << std::hex << std::setfill('0') << std::setw(2)
				<< static_cast<unsigned int>(packet.Bytes[i]);
		}
		std::cout << std::dec;
		if (packet.Truncated)
			std::cout << " ... (truncated)";
		_LogMidiMessageDetail(std::cout, packet.Bytes.data(), packet.Size);
		std::cout << '\n';
	}
	const auto dropped = _verbosePackets.DroppedCount();
	if (dropped != _lastVerboseDroppedCount)
	{
		std::cout << "[MIDI] Verbose packet queue dropped "
			<< (dropped - _lastVerboseDroppedCount) << " packets for device \""
			<< configuredName << "\"\n";
		_lastVerboseDroppedCount = dropped;
	}
}

void MidiDevice::_OnMidiData(const std::vector<unsigned char>& message,
	double deltaSeconds,
	std::int64_t callbackArrivalMicros) noexcept
{
	if (!_callback)
		return;
	if (message.empty())
		return;

	const auto status = static_cast<std::uint8_t>(message[0]);
	const auto data1 = static_cast<std::uint8_t>(message.size() > 1 ? message[1] : 0u);
	const auto data2 = static_cast<std::uint8_t>(message.size() > 2 ? message[2] : 0u);
	_callback(status, data1, data2, deltaSeconds, callbackArrivalMicros);
	if (_loggingVerbose)
	{
		VerbosePacket packet;
		packet.Size = std::min(message.size(), packet.Bytes.size());
		packet.Truncated = message.size() > packet.Size;
		std::copy_n(message.begin(), packet.Size, packet.Bytes.begin());
		_verbosePackets.Push(packet);
	}
}
