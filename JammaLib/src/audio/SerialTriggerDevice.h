#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "SerialTriggerProtocol.h"

namespace audio
{
	class SerialTriggerDevice
	{
	public:
		using TriggerEventCallback = std::function<void(const SerialTriggerEvent& event)>;

		SerialTriggerDevice();
		~SerialTriggerDevice();

		SerialTriggerDevice(const SerialTriggerDevice&) = delete;
		SerialTriggerDevice& operator=(const SerialTriggerDevice&) = delete;

		bool Open(const std::string& portName,
			unsigned int baudRate,
			TriggerEventCallback callback);
		void Close();

		bool IsOpen() const noexcept { return _handle != nullptr; }
		const std::string& PortName() const noexcept { return _portName; }
		unsigned int BaudRate() const noexcept { return _baudRate; }

	private:
		void _ReadLoop();
		static std::string _NormalisePortName(const std::string& portName);

		std::atomic_bool _running;
		void* _handle;
		std::thread _reader;
		std::string _portName;
		unsigned int _baudRate;
		TriggerEventCallback _callback;
		SerialTriggerProtocol _protocol;
	};
}