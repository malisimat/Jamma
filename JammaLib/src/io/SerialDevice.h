#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "SerialTriggerProtocol.h"

namespace io
{
	class SerialDevice
	{
	public:
		using TriggerEventCallback = std::function<void(const SerialTriggerEvent& event)>;

		SerialDevice();
		~SerialDevice();

		SerialDevice(const SerialDevice&) = delete;
		SerialDevice& operator=(const SerialDevice&) = delete;

		static std::vector<std::string> EnumeratePorts();

		bool Open(const std::string& deviceName,
			const std::string& portName,
			unsigned int baudRate,
			TriggerEventCallback callback);
		void Close();

		bool IsOpen() const noexcept { return _handle != nullptr; }
		const std::string& DeviceName() const noexcept { return _deviceName; }
		const std::string& PortName() const noexcept { return _portName; }
		unsigned int BaudRate() const noexcept { return _baudRate; }

	private:
		void _ReadLoop();
		static std::string _NormalisePortName(const std::string& portName);

		std::atomic_bool _running;
		void* _handle;
		std::thread _reader;
		std::string _deviceName;
		std::string _portName;
		unsigned int _baudRate;
		TriggerEventCallback _callback;
		SerialTriggerProtocol _protocol;
	};
}