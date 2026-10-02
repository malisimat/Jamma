#pragma once

#include "../../console/LineRing.h"
#include <array>
#include <iostream>
#include <streambuf>

namespace console
{
	// App-side capture of opted-in non-audio C++ output. A callback path that
	// has not opted in performs no allocation, lock, or I/O.
	class ConsoleCapture
	{
	public:
		explicit ConsoleCapture(LineRing& lines) : _lines(lines), _cout(this), _cerr(this)
		{
			_originalCout = std::cout.rdbuf(&_cout);
			_originalCerr = std::cerr.rdbuf(&_cerr);
		}
		~ConsoleCapture() { Stop(); }
		ConsoleCapture(const ConsoleCapture&) = delete;
		ConsoleCapture& operator=(const ConsoleCapture&) = delete;
		static void EnableForCurrentThread(bool enabled) noexcept { _enabled = enabled; }
		void Stop() noexcept
		{
			if (!_originalCout) return;
			std::cout.rdbuf(_originalCout);
			std::cerr.rdbuf(_originalCerr);
			_originalCout = nullptr;
			_originalCerr = nullptr;
		}
	private:
		class Buffer final : public std::streambuf
		{
		public:
			explicit Buffer(ConsoleCapture* owner) : _owner(owner) {}
		protected:
			int_type overflow(int_type value) override
			{
				if (value != traits_type::eof()) _owner->Put(static_cast<char>(value));
				return traits_type::not_eof(value);
			}
			std::streamsize xsputn(const char* text, std::streamsize size) override
			{
				for (std::streamsize at = 0; at < size; ++at) _owner->Put(text[at]);
				return size;
			}
			int sync() override { return 0; }
		private:
			ConsoleCapture* _owner;
		};
		void Put(char value) noexcept
		{
			if (!_enabled) return;
			if (value == '\r') return;
			if (value == '\n')
			{
				_lines.Publish(std::string_view(_pending.data(), _length));
				_length = 0;
				_overflow = false;
				return;
			}
			if (_length < _pending.size()) _pending[_length++] = value;
			else _overflow = true;
			if (_overflow) _length = _pending.size(); // Oversize is counted at newline.
		}
		LineRing& _lines;
		Buffer _cout;
		Buffer _cerr;
		std::streambuf* _originalCout = nullptr;
		std::streambuf* _originalCerr = nullptr;
		static inline thread_local bool _enabled = false;
		static inline thread_local std::array<char, LineRing::MaxLineBytes + 1> _pending{};
		static inline thread_local std::size_t _length = 0;
		static inline thread_local bool _overflow = false;
	};
}
