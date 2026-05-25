#include "MidiDevice.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>

#include "rtmidi/RtMidi.h"

using namespace audio;

namespace
{
	std::string ToLower(std::string str)
	{
		std::transform(str.begin(),
		               str.end(),
		               str.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return str;
	}
}

MidiDevice::MidiDevice()
	: _midiIn(nullptr),
	  _isOpen(false),
	  _deviceName(""),
	  _deviceId(0u),
	  _callback()
{
}

MidiDevice::~MidiDevice()
{
	Close();
}

std::vector<MidiInputDeviceInfo> MidiDevice::EnumerateInputDevices()
{
	std::vector<MidiInputDeviceInfo> devices;

	try
	{
		rt::midi::RtMidiIn midiIn;
		const auto count = midiIn.getPortCount();
		devices.reserve(count);

		for (unsigned int i = 0; i < count; ++i)
			devices.push_back({ i, midiIn.getPortName(i) });
	}
	catch (const rt::midi::RtMidiError& err)
	{
		std::cout << "[MIDI] Failed to enumerate MIDI input devices: " << err.getMessage() << std::endl;
	}
	catch (const std::exception& err)
	{
		std::cout << "[MIDI] Failed to enumerate MIDI input devices: " << err.what() << std::endl;
	}

	return devices;
}

bool MidiDevice::Open(const std::string& preferredDeviceName,
                      MidiMessageCallback callback)
{
	_callback = std::move(callback);
	Close();

	try
	{
		_midiIn = std::make_unique<rt::midi::RtMidiIn>();
	}
	catch (const rt::midi::RtMidiError& err)
	{
		std::cout << "[MIDI] Failed to create RtMidiIn instance: " << err.getMessage() << std::endl;
		return false;
	}

	std::vector<MidiInputDeviceInfo> devices;
	const auto count = _midiIn->getPortCount();
	devices.reserve(count);
	for (unsigned int i = 0; i < count; ++i)
		devices.push_back({ i, _midiIn->getPortName(i) });

	std::cout << "[MIDI] Input devices found: " << devices.size() << std::endl;
	for (const auto& d : devices)
		std::cout << "[MIDI]   #" << d.DeviceId << "  " << d.Name << std::endl;

	if (devices.empty())
	{
		std::cout << "[MIDI] No MIDI input devices available." << std::endl;
		return false;
	}

	auto selected = devices.front();
	if (!preferredDeviceName.empty() && (preferredDeviceName != "default"))
	{
		const auto wanted = ToLower(preferredDeviceName);

		auto exact = std::find_if(devices.begin(), devices.end(), [&](const MidiInputDeviceInfo& d) {
			return ToLower(d.Name) == wanted;
		});
		if (exact != devices.end())
		{
			selected = *exact;
		}
		else
		{
			auto partial = std::find_if(devices.begin(), devices.end(), [&](const MidiInputDeviceInfo& d) {
				return ToLower(d.Name).find(wanted) != std::string::npos;
			});
			if (partial != devices.end())
			{
				selected = *partial;
			}
			else
			{
				std::cout << "[MIDI] Preferred device not found: \"" << preferredDeviceName
				          << "\". Falling back to #" << selected.DeviceId << " (" << selected.Name << ")"
				          << std::endl;
			}
		}
	}

	try
	{
		_midiIn->ignoreTypes(false, false, false);
		if (_callback)
			_midiIn->setCallback(&MidiDevice::_RtMidiCallback, this);
		_midiIn->openPort(selected.DeviceId, "Jamma MIDI In");
	}
	catch (const rt::midi::RtMidiError& err)
	{
		std::cout << "[MIDI] Failed to open input device #" << selected.DeviceId
			<< " (" << selected.Name << "): " << err.getMessage() << std::endl;
		_callback = MidiMessageCallback();
		_midiIn.reset();
		return false;
	}

	_deviceName = selected.Name;
	_deviceId = selected.DeviceId;
	_isOpen = true;

	std::cout << "[MIDI] Connected input device #" << _deviceId
	          << " (" << _deviceName << ")" << std::endl;
	return true;
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

void MidiDevice::_RtMidiCallback(double,
                                 std::vector<unsigned char>* message,
                                 void* userData)
{
	if ((nullptr == userData) || (nullptr == message))
		return;

	auto self = reinterpret_cast<MidiDevice*>(userData);
	self->_OnMidiData(*message);
}

void MidiDevice::_OnMidiData(const std::vector<unsigned char>& message) noexcept
{
	if (!_callback)
		return;
	if (message.empty())
		return;

	const auto status = static_cast<std::uint8_t>(message[0]);
	const auto data1 = static_cast<std::uint8_t>(message.size() > 1 ? message[1] : 0u);
	const auto data2 = static_cast<std::uint8_t>(message.size() > 2 ? message[2] : 0u);
	_callback(status, data1, data2);
}
