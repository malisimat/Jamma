#include "SerialTriggerDevice.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <iostream>

using namespace audio;

namespace
{
	HANDLE AsHandle(void* handle)
	{
		return static_cast<HANDLE>(handle);
	}
}

SerialTriggerDevice::SerialTriggerDevice()
	: _running(false),
	  _handle(nullptr),
	  _reader(),
	  _portName(),
	  _baudRate(0u),
	  _callback(),
	  _protocol()
{
}

SerialTriggerDevice::~SerialTriggerDevice()
{
	Close();
}

bool SerialTriggerDevice::Open(const std::string& portName,
	unsigned int baudRate,
	TriggerEventCallback callback)
{
	Close();

	const auto normalisedPort = _NormalisePortName(portName);
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
		std::cout << "[Serial] Failed to open port " << portName << std::endl;
		return false;
	}

	DCB dcb{};
	dcb.DCBlength = sizeof(DCB);
	if (!GetCommState(handle, &dcb))
	{
		std::cout << "[Serial] Failed to read port state for " << portName << std::endl;
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
		std::cout << "[Serial] Failed to configure port " << portName << std::endl;
		CloseHandle(handle);
		return false;
	}

	COMMTIMEOUTS timeouts{};
	timeouts.ReadIntervalTimeout = 10;
	timeouts.ReadTotalTimeoutConstant = 10;
	timeouts.ReadTotalTimeoutMultiplier = 0;
	SetCommTimeouts(handle, &timeouts);
	PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

	_protocol.Reset();
	_callback = std::move(callback);
	_portName = portName;
	_baudRate = baudRate;
	_handle = handle;
	_running.store(true, std::memory_order_release);
	_reader = std::thread([this]() { _ReadLoop(); });

	std::cout << "[Serial] Connected port " << _portName << " @ " << _baudRate << std::endl;
	return true;
}

void SerialTriggerDevice::Close()
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
		std::cout << "[Serial] Disconnected port " << _portName << std::endl;
	}

	_handle = nullptr;
	_portName.clear();
	_baudRate = 0u;
	_callback = TriggerEventCallback();
	_protocol.Reset();
}

void SerialTriggerDevice::_ReadLoop()
{
	auto handle = AsHandle(_handle);
	while (_running.load(std::memory_order_acquire) && (handle != nullptr))
	{
		unsigned char byte = 0u;
		DWORD bytesRead = 0u;
		if (!ReadFile(handle, &byte, 1u, &bytesRead, nullptr))
		{
			if (_running.load(std::memory_order_acquire))
				std::cout << "[Serial] Read error on " << _portName << std::endl;
			break;
		}

		if (bytesRead != 1u)
			continue;

		SerialTriggerEvent event{};
		if (_callback && _protocol.PushByte(byte, event))
			_callback(event);
	}
}

std::string SerialTriggerDevice::_NormalisePortName(const std::string& portName)
{
	if (portName.rfind("\\\\.\\", 0) == 0)
		return portName;

	return "\\\\.\\" + portName;
}