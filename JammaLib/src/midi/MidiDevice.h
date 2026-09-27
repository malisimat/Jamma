#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
#include "MidiQueue.h"

namespace rt { namespace midi { class RtMidiIn; } }

namespace midi
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
			std::uint8_t data2,
			double deltaSeconds,
			std::int64_t callbackArrivalMicros)>;

		MidiDevice();
		~MidiDevice();

		MidiDevice(const MidiDevice&) = delete;
		MidiDevice& operator=(const MidiDevice&) = delete;

		static std::vector<MidiInputDeviceInfo> EnumerateInputDevices();

		// Opens and starts a MIDI input stream. If preferredDeviceName is empty, or
		// no exact/substring match is found, the first discovered input is used.
		// loggingVerbose: capture bounded packet details for the MIDI pump to print.
		bool Open(const std::string& preferredDeviceName,
			MidiMessageCallback callback,
			bool loggingVerbose = false);
		void Close();
		// Single consumer: call from the MIDI pump, never from the device callback.
		void DrainVerbosePackets(const std::string& configuredName);

		bool IsOpen() const noexcept { return _isOpen; }
		const std::string& DeviceName() const noexcept { return _deviceName; }
		unsigned int DeviceId() const noexcept { return _deviceId; }

	private:
		static std::string _ToLower(std::string str);
		static void _LogMidiMessageDetail(std::ostream& out,
			const unsigned char* message, std::size_t size);
		static void _RtMidiCallback(double deltatime,
			std::vector<unsigned char>* message,
			void* userData);
		void _OnMidiData(const std::vector<unsigned char>& message,
			double deltaSeconds,
			std::int64_t callbackArrivalMicros) noexcept;

		std::unique_ptr<rt::midi::RtMidiIn> _midiIn;
		bool _isOpen;
		std::string _deviceName;
		unsigned int _deviceId;
		bool _loggingVerbose;
		MidiMessageCallback _callback;
		struct VerbosePacket
		{
			std::array<unsigned char, 256> Bytes{};
			std::size_t Size = 0u;
			bool Truncated = false;
		};
		// The WinMM callback is the sole producer; PumpMidi is the sole consumer.
		MidiQueue<64, VerbosePacket> _verbosePackets;
		std::uint64_t _lastVerboseDroppedCount = 0u;
	};
}
