#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rt { namespace midi { class RtMidiIn; } }

namespace audio
{
	struct MidiInputDeviceInfo
	{
		unsigned int DeviceId = 0u;
		std::string Name;
	};

	class MidiDevice
	{
	public:
		using MidiMessageCallback = std::function<void(std::uint8_t status,
		                                              std::uint8_t data1,
		                                              std::uint8_t data2)>;

		MidiDevice();
		~MidiDevice();

		MidiDevice(const MidiDevice&) = delete;
		MidiDevice& operator=(const MidiDevice&) = delete;

		static std::vector<MidiInputDeviceInfo> EnumerateInputDevices();

		// Opens and starts a MIDI input stream. If preferredDeviceName is empty, or
		// no exact/substring match is found, the first discovered input is used.
		bool Open(const std::string& preferredDeviceName,
		          MidiMessageCallback callback);
		void Close();

		bool IsOpen() const noexcept { return _isOpen; }
		const std::string& DeviceName() const noexcept { return _deviceName; }
		unsigned int DeviceId() const noexcept { return _deviceId; }

	private:
		static void _RtMidiCallback(double deltatime,
		                           std::vector<unsigned char>* message,
		                           void* userData);
		void _OnMidiData(const std::vector<unsigned char>& message) noexcept;

		std::unique_ptr<rt::midi::RtMidiIn> _midiIn;
		bool _isOpen;
		std::string _deviceName;
		unsigned int _deviceId;
		MidiMessageCallback _callback;
	};
}
