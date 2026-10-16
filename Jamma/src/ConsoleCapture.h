#pragma once
#include "../../console/LineRing.h"
#include "../../console/DiagnosticThread.h"
#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <streambuf>
namespace console
{
	// App producers stop before Stop restores process stream buffers. Shared State
	// survives worker TLS destruction; Stop detaches sinks before their owners die.
	class ConsoleCapture
	{
		struct Thread;
		struct Pending
		{
			std::array<char, LineRing::MaxLineBytes> Bytes{};
			std::size_t Length = 0;
			bool Startup = false;
		};
		struct State
		{
			std::mutex Mutex;
			LineRing* Lines = nullptr;
			std::function<void(std::string_view)> Startup;
			Thread* Threads = nullptr;
			bool Active = true;
			bool GeneralStartup = true;
			void Publish(Pending& pending)
			{
				const std::string_view line(pending.Bytes.data(), pending.Length);
				Lines->Publish(line);
				if (Startup && pending.Startup) Startup(line);
				pending.Length = 0;
			}
		};
		struct Thread
		{
			std::shared_ptr<State> Owner;
			Thread* Next = nullptr;
			std::array<Pending, 3> Streams;
			void Detach()
			{
				if (!Owner) return;
				auto state = std::move(Owner);
				std::scoped_lock lock(state->Mutex);
				if (!state->Active) return;
				for (auto& pending : Streams) if (pending.Length) state->Publish(pending);
				auto** node = &state->Threads;
				while (*node && *node != this) node = &(*node)->Next;
				if (*node) *node = Next;
			}
			~Thread() { Detach(); }
		};
	public:
		explicit ConsoleCapture(LineRing& lines, std::function<void(std::string_view)> startup = {})
			: _state(std::make_shared<State>()), _cout(this, 0), _cerr(this, 1), _clog(this, 2)
		{
			_state->Lines = &lines;
			_state->Startup = std::move(startup);
			DiagnosticThread::StartupWindow(true);
			// cerr's unitbuf would turn every insertion in a diagnostic into a
			// separate entry. Newline and explicit flush remain publication points.
			_cerrUnitbuf = (std::cerr.flags() & std::ios_base::unitbuf) != 0;
			std::cerr.unsetf(std::ios_base::unitbuf);
			_originalCout = std::cout.rdbuf(&_cout);
			_originalCerr = std::cerr.rdbuf(&_cerr);
			_originalClog = std::clog.rdbuf(&_clog);
		}
		~ConsoleCapture() { Stop(); }
		ConsoleCapture(const ConsoleCapture&) = delete;
		ConsoleCapture& operator=(const ConsoleCapture&) = delete;
		static void EnableForCurrentThread(bool enabled) noexcept { _enabled = enabled; }
		void CompleteStartup(std::string_view completion = {})
		{
			std::scoped_lock lock(_state->Mutex);
			if (!_state->GeneralStartup) return;
			FlushAll();
			if (!completion.empty())
			{
				_state->Lines->Publish(completion);
				if (_state->Startup) _state->Startup(completion);
			}
			_state->GeneralStartup = false;
			DiagnosticThread::StartupWindow(false);
		}
		void Stop() noexcept
		{
			if (!_originalCout) return;
			{
				std::scoped_lock lock(_state->Mutex);
				FlushAll();
				_state->Active = false;
				DiagnosticThread::StartupWindow(false);
				_state->Threads = nullptr;
				_state->Startup = {};
			}
			std::cout.rdbuf(_originalCout);
			std::cerr.rdbuf(_originalCerr);
			std::clog.rdbuf(_originalClog);
			if (_cerrUnitbuf) std::cerr.setf(std::ios_base::unitbuf);
			_originalCout = nullptr;
		}
	private:
		class Buffer final : public std::streambuf
		{
		public:
			Buffer(ConsoleCapture* owner, unsigned stream) : _owner(owner), _stream(stream) {}
		protected:
			int_type overflow(int_type value) override
			{
				if (value != traits_type::eof())
				{
					const char ch = static_cast<char>(value);
					_owner->Write(_stream, &ch, 1, false);
				}
				return traits_type::not_eof(value);
			}
			std::streamsize xsputn(const char* text, std::streamsize size) override
			{
				_owner->Write(_stream, text, size, false);
				return size;
			}
			int sync() override { _owner->Write(_stream, nullptr, 0, true); return 0; }
		private:
			ConsoleCapture* _owner; unsigned _stream;
		};
		void FlushAll()
		{
			for (auto* thread = _state->Threads; thread; thread = thread->Next)
				for (auto& pending : thread->Streams) if (pending.Length) _state->Publish(pending);
		}
		void Write(unsigned stream, const char* text, std::streamsize size, bool flush)
		{
			if (!_enabled) return;
			if (DiagnosticThread::Realtime()) { if (size) DiagnosticThread::Suppressed(); return; }
			if (_thread.Owner != _state)
			{
				_thread.Detach();
				std::scoped_lock lock(_state->Mutex);
				_thread.Owner = _state;
				_thread.Streams = {};
				_thread.Next = _state->Threads;
				_state->Threads = &_thread;
			}
			std::scoped_lock lock(_state->Mutex);
			if (!_state->Active) return;
			auto& pending = _thread.Streams[stream];
			const bool startup = DiagnosticThread::StartupAttributed(_state->GeneralStartup);
			if (pending.Length && pending.Startup != startup) _state->Publish(pending);
			for (std::streamsize at = 0; at < size; ++at)
			{
				if (text[at] == '\r') continue;
				if (!pending.Length) pending.Startup = startup;
				if (text[at] == '\n') { _state->Publish(pending); continue; }
				// Split oversize lines at UTF-8 boundaries, retaining the tail.
				if (pending.Length == pending.Bytes.size())
				{
					auto split = pending.Length;
					while (split > pending.Length - 4 && (static_cast<unsigned char>(pending.Bytes[split - 1]) & 0xc0) == 0x80) --split;
					if (split < pending.Length || static_cast<unsigned char>(pending.Bytes[split - 1]) >= 0xc0) --split;
					const auto tail = pending.Length - split;
					pending.Length = split;
					_state->Publish(pending);
					std::memmove(pending.Bytes.data(), pending.Bytes.data() + split, tail);
					pending.Length = tail;
				}
				pending.Bytes[pending.Length++] = text[at];
			}
			if (flush && pending.Length) _state->Publish(pending);
		}
		bool _cerrUnitbuf = false;
		std::shared_ptr<State> _state;
		Buffer _cout, _cerr, _clog;
		std::streambuf* _originalCout = nullptr;
		std::streambuf* _originalCerr = nullptr;
		std::streambuf* _originalClog = nullptr;
		static inline thread_local bool _enabled = true;
		static thread_local Thread _thread;
	};
	inline thread_local ConsoleCapture::Thread ConsoleCapture::_thread;
}
