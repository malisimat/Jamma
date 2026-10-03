#pragma once

#include "Protocol.h"
#include <Windows.h>
#include <array>
#include <vector>

namespace console
{
	class UniqueHandle
	{
	public:
		UniqueHandle() = default;
		explicit UniqueHandle(HANDLE value) noexcept : _value(value) {}
		~UniqueHandle() { Reset(); }
		UniqueHandle(const UniqueHandle&) = delete;
		UniqueHandle& operator=(const UniqueHandle&) = delete;
		UniqueHandle(UniqueHandle&& other) noexcept : _value(other.Release()) {}
		UniqueHandle& operator=(UniqueHandle&& other) noexcept
		{
			if (this != &other) Reset(other.Release());
			return *this;
		}
		HANDLE Get() const noexcept { return _value; }
		bool Valid() const noexcept { return _value && _value != INVALID_HANDLE_VALUE; }
		HANDLE Release() noexcept { const auto value = _value; _value = nullptr; return value; }
		void Reset(HANDLE value = nullptr) noexcept
		{
			if (Valid()) CloseHandle(_value);
			_value = value;
		}
	private:
		HANDLE _value = nullptr;
	};

	// The caller owns the pipe and stop event. One worker performs all I/O.
	// Cancellation is completed before the stack OVERLAPPED or buffer is reused.
	inline bool Transfer(HANDLE pipe, HANDLE stop, void* data, std::size_t size,
		bool writing, DWORD timeoutMs = INFINITE)
	{
		auto* bytes = static_cast<std::uint8_t*>(data);
		while (size)
		{
			UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
			if (!event.Valid()) return false;
			OVERLAPPED operation{};
			operation.hEvent = event.Get();
			DWORD done = 0;
			const bool completed = writing
				? !!WriteFile(pipe, bytes, static_cast<DWORD>(size), &done, &operation)
				: !!ReadFile(pipe, bytes, static_cast<DWORD>(size), &done, &operation);
			if (!completed)
			{
				if (GetLastError() != ERROR_IO_PENDING) return false;
				const HANDLE waits[]{ stop, event.Get() };
				const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
				if (wait != WAIT_OBJECT_0 + 1)
				{
					CancelIoEx(pipe, &operation);
					GetOverlappedResult(pipe, &operation, &done, TRUE);
					return false;
				}
				if (!GetOverlappedResult(pipe, &operation, &done, FALSE)) return false;
			}
			if (!done) return false;
			bytes += done;
			size -= done;
		}
		return true;
	}

	inline bool WriteMessage(HANDLE pipe, HANDLE stop, const Message& message,
		DWORD timeoutMs = 2000)
	{
		auto frame = EncodeFrame(message);
		return frame && Transfer(pipe, stop, frame->data(), frame->size(), true, timeoutMs);
	}

	inline FrameResult ReadMessage(HANDLE pipe, HANDLE stop, DWORD timeoutMs = INFINITE)
	{
		std::array<std::uint8_t, 4> prefix{};
		if (!Transfer(pipe, stop, prefix.data(), prefix.size(), false, timeoutMs))
			return { FrameError::Incomplete, std::nullopt };
		std::uint32_t length = 0;
		for (unsigned i = 0; i < 4; ++i)
			length |= static_cast<std::uint32_t>(prefix[i]) << (i * 8);
		if (length > MaxFrameBytes) return { FrameError::TooLarge, std::nullopt };
		if (length < FrameHeaderBytes) return { FrameError::InvalidSize, std::nullopt };
		std::vector<std::uint8_t> frame(4 + length);
		for (unsigned i = 0; i < 4; ++i) frame[i] = prefix[i];
		if (!Transfer(pipe, stop, frame.data() + 4, length, false, timeoutMs))
			return { FrameError::Incomplete, std::nullopt };
		return DecodeFrame(frame);
	}
}
