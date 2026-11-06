#pragma once
#include "ConsoleCapture.h"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include <thread>
namespace console
{
	// Process descriptor setup/teardown belongs to the app owner. Library workers
	// must stop before Stop. The reader only enters the off-callback capture sink.
	class CrtCapture
	{
		struct Stream
		{
			FILE* File = nullptr;
			DWORD Id = 0;
			int Saved = -1;
			HANDLE Read = nullptr, Previous = nullptr;
			bool SharedHandle = false, Redirected = false;
		};
	public:
		explicit CrtCapture(ConsoleCapture& capture) : _capture(capture)
		{
			_streams[0].File = stdout; _streams[0].Id = STD_OUTPUT_HANDLE;
			_streams[1].File = stderr; _streams[1].Id = STD_ERROR_HANDLE;
			_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!_stop) { Fail("stop-event"); return; }
			if (!Redirect(_streams[0]) || !Redirect(_streams[1])) { Stop(); return; }
			try { _reader = std::thread([this] { Drain(); }); }
			catch (...) { Fail("reader-start"); Stop(); }
		}
		~CrtCapture() { Stop(); }
		CrtCapture(const CrtCapture&) = delete;
		CrtCapture& operator=(const CrtCapture&) = delete;
		bool Available() const noexcept { return _reader.joinable(); }
		void Stop() noexcept
		{
			// Flush while the reader is still running, then restore writer handles.
			// Reader shutdown uses an event rather than waiting for pipe EOF: a
			// third-party duplicate of a writer must not hold application exit open.
			for (auto& stream : _streams)
			{
				if (stream.Redirected)
				{
					std::fflush(stream.File);
					const auto descriptor = _fileno(stream.File);
					if (_dup2(stream.Saved, descriptor) != 0) Fail("restore-descriptor");
					SetStdHandle(stream.Id, stream.SharedHandle ? reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)) : stream.Previous);
					stream.Redirected = false;
				}
				if (stream.Saved != -1) { _close(stream.Saved); stream.Saved = -1; }
			}
			if (_stop) SetEvent(_stop);
			if (_reader.joinable()) _reader.join();
			for (auto& stream : _streams)
				if (stream.Read) { CloseHandle(stream.Read); stream.Read = nullptr; }
			if (_stop) { CloseHandle(_stop); _stop = nullptr; }
		}
	private:
		bool Redirect(Stream& stream)
		{
			stream.Previous = GetStdHandle(stream.Id);
			// A GUI subsystem launch can have no CRT descriptor at all. Give that
			// FILE a valid NUL descriptor before replacing it with our pipe.
			if (_fileno(stream.File) < 0)
			{
				FILE* opened = nullptr;
				if (_wfreopen_s(&opened, L"NUL", L"w", stream.File)) { Fail("initialize-descriptor"); return false; }
			}
			const auto descriptor = _fileno(stream.File);
			stream.SharedHandle = stream.Previous == reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
			std::fflush(stream.File);
			stream.Saved = _dup(descriptor);
			if (stream.Saved == -1) { Fail("save-descriptor"); return false; }
			HANDLE writer = nullptr;
			if (!CreatePipe(&stream.Read, &writer, nullptr, 64 * 1024)) { Fail("create-pipe"); return false; }
			const auto pipeDescriptor = _open_osfhandle(reinterpret_cast<intptr_t>(writer), _O_WRONLY | _O_BINARY | _O_NOINHERIT);
			if (pipeDescriptor == -1) { CloseHandle(writer); Fail("pipe-descriptor"); return false; }
			const bool redirected = _dup2(pipeDescriptor, descriptor) == 0;
			_close(pipeDescriptor);
			if (!redirected) { Fail("redirect-descriptor"); return false; }
			stream.Redirected = true;
			if (!SetStdHandle(stream.Id, reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)))) { Fail("redirect-handle"); return false; }
			return true;
		}
		void Drain() noexcept
		{
			try
			{
				std::array<char, 4096> bytes{};
				std::array<DWORD, 2> remaining{};
				std::array<bool, 2> failed{};
				bool stopping = false;
				for (;;)
				{
					if (!stopping && WaitForSingleObject(_stop, 0) == WAIT_OBJECT_0)
					{
						stopping = true;
						for (unsigned i = 0; i < _streams.size(); ++i)
							PeekNamedPipe(_streams[i].Read, nullptr, 0, nullptr, &remaining[i], nullptr);
					}
					bool readAny = false;
					for (unsigned index = 0; index < _streams.size(); ++index)
					{
						if (failed[index] || (stopping && !remaining[index])) continue;
						DWORD available = 0;
						const auto pipe = _streams[index].Read;
						if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
						{
							if (GetLastError() != ERROR_BROKEN_PIPE) Fail("pipe-peek");
							failed[index] = true;
							continue;
						}
						if (stopping) available = (std::min)(available, remaining[index]);
						if (!available) continue;
						DWORD count = 0;
						if (!ReadFile(pipe, bytes.data(), (std::min)(available, static_cast<DWORD>(bytes.size())), &count, nullptr))
						{ Fail("pipe-read"); failed[index] = true; continue; }
						if (count) _capture.WriteExternal(index, std::string_view(bytes.data(), count));
						readAny |= count != 0;
						if (stopping) remaining[index] -= count;
					}
					if (readAny) continue;
					if (stopping) break;
					// Flush raw partial diagnostics after an idle interval, independently
					// for stdout/stderr. CRT buffering still requires fflush at its producer.
					_capture.WriteExternal(0, {}, true);
					_capture.WriteExternal(1, {}, true);
					WaitForSingleObject(_stop, 10);
				}
				// Pending CRT fragments belong to this reader, so its TLS destructor
				// flushes them before Stop returns and before capture is detached.
			}
			catch (...) { Fail("reader-exception"); }
		}
		void Fail(const char* stage) noexcept
		{
			std::array<char, 192> text{};
			std::snprintf(text.data(), text.size(), "[ERROR][diagnostics] CRT capture stage=%s windows-error=%lu crt-error=%d\n", stage, GetLastError(), errno);
			OutputDebugStringA(text.data());
			try { _capture.WriteExternal(1, text.data(), true); } catch (...) {}
		}
		ConsoleCapture& _capture;
		std::array<Stream, 2> _streams;
		HANDLE _stop = nullptr;
		std::thread _reader;
	};
}
