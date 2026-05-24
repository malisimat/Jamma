#include "SerialDevice.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <iostream>

using namespace io;

namespace
{
	HANDLE AsHandle(void* handle)
	{
		return static_cast<HANDLE>(handle);
	}
}

SerialDevice::SerialDevice()
	: _running(false),
	  _handle(nullptr),
	  _reader(),
	  _deviceName(),
	  _portName(),
	  _baudRate(0u),
	  _callback(),
	  _protocol()
{
}

SerialDevice::~SerialDevice()
{
	Close();
}

std::vector<std::string> SerialDevice::EnumeratePorts()
{
	std::vector<std::string> ports;
	char deviceTarget[512]{};

	for (unsigned int index = 1u; index <= 256u; ++index)
	{
		auto portName = std::string("COM") + std::to_string(index);
		if (0u != QueryDosDeviceA(portName.c_str(), deviceTarget, static_cast<DWORD>(sizeof(deviceTarget))))
			ports.push_back(portName);
	}

	return ports;
}

bool SerialDevice::Open(const std::string& deviceName,
	const std::string& portName,
	unsigned int baudRate,
	TriggerEventCallback callback)
{
	Close();

	const auto normalisedPort = _NormalisePortName(portName);
	std::cout << "[Serial] Opening device \"" << deviceName << "\" on " << portName
		<< " @ " << baudRate << std::endl;

	auto handle = CreateFileA(
		normalisedPort.c_str(),
		GENERIC_READ,
		0,
		nullptr,
		OPEN_EXISTING,
		0,
		nullptr);

	if (handle == INVALID_HANDLE_VALUE)
	{
		std::cout << "[Serial] Failed to open device \"" << deviceName << "\" on "
			<< portName << " (error " << GetLastError() << ")" << std::endl;
		return false;
	}

	DCB dcb{};
	dcb.DCBlength = sizeof(DCB);
	if (!GetCommState(handle, &dcb))
	{
		std::cout << "[Serial] Failed to read port state for device \"" << deviceName
			<< "\" on " << portName << " (error " << GetLastError() << ")" << std::endl;
		CloseHandle(handle);
		return false;
	}

	dcb.BaudRate = baudRate;
	dcb.ByteSize = 8;
	dcb.StopBits = ONESTOPBIT;
	dcb.Parity = NOPARITY;
	dcb.fBinary = TRUE;
	if (!SetCommState(handle, &dcb))
	{
		std::cout << "[Serial] Failed to configure device \"" << deviceName << "\" on "
			<< portName << " (error " << GetLastError() << ")" << std::endl;
		CloseHandle(handle);
		return false;
	}

	COMMTIMEOUTS timeouts{};
	timeouts.ReadIntervalTimeout = 10;
	timeouts.ReadTotalTimeoutConstant = 10;
	timeouts.ReadTotalTimeoutMultiplier = 0;
	if (!SetCommTimeouts(handle, &timeouts))
	{
		std::cout << "[Serial] Failed to set timeouts for device \"" << deviceName << "\" on "
			<< portName << " (error " << GetLastError() << ")" << std::endl;
		CloseHandle(handle);
		return false;
	}

	PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

	_protocol.Reset();
	_callback = std::move(callback);
	_deviceName = deviceName;
	_portName = portName;
	_baudRate = baudRate;
	_handle = handle;
	_running.store(true, std::memory_order_release);
	_reader = std::thread([this]() { _ReadLoop(); });

	std::cout << "[Serial] Connected device \"" << _deviceName << "\" on " << _portName
		<< " @ " << _baudRate << std::endl;
	return true;
}

void SerialDevice::Close()
{
	_running.store(false, std::memory_order_release);

	auto handle = AsHandle(_handle);
	if (handle != nullptr)
		CancelIoEx(handle, nullptr);

	if (_reader.joinable())
		_reader.join();

	if (handle != nullptr)
	{
		CloseHandle(handle);
		std::cout << "[Serial] Disconnected device \"" << _deviceName << "\" from "
			<< _portName << std::endl;
	}

	_handle = nullptr;
	_deviceName.clear();
	_portName.clear();
	_baudRate = 0u;
	_callback = TriggerEventCallback();
	_protocol.Reset();
}

void SerialDevice::_ReadLoop()
{
	auto handle = AsHandle(_handle);
	while (_running.load(std::memory_order_acquire) && (handle != nullptr))
	{
		unsigned char byte = 0u;
		DWORD bytesRead = 0u;
		if (!ReadFile(handle, &byte, 1u, &bytesRead, nullptr))
		{
			if (_running.load(std::memory_order_acquire))
			{
				std::cout << "[Serial] Read error on device \"" << _deviceName << "\" ("
					<< _portName << ", error " << GetLastError() << ")" << std::endl;
			}
			break;
		}

		if (bytesRead != 1u)
			continue;

		SerialTriggerEvent event{};
		if (_callback && _protocol.PushByte(byte, event))
		{
			event.Device = _deviceName;
			_callback(event);
		}
	}
}

std::string SerialDevice::_NormalisePortName(const std::string& portName)
{
	if (portName.rfind("\\\\.\\", 0) == 0)
		return portName;

	return "\\\\.\\" + portName;
}