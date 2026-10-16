#pragma once
#include <atomic>
#include <cstddef>
#include <array>
#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
namespace console
{
	struct BackendDiagnostic
	{
		int Type = 0;
		bool Startup = false;
		std::size_t Length = 0;
		std::array<char, 512> Text{};
	};
	// Compatibility streams never enter the off-callback capture from these scopes.
	class DiagnosticThread
	{
	public:
		class RealtimeScope
		{
		public:
			RealtimeScope() noexcept : _previous(_realtime) { _realtime = true; }
			~RealtimeScope() { _realtime = _previous; }
		private:
			bool _previous;
		};
		enum class StartupOrigin { Unspecified, Startup, Runtime };
		class StartupScope
		{
		public:
			explicit StartupScope(bool startup = true) noexcept : _previous(_startupThread) { _startupThread = startup ? StartupOrigin::Startup : StartupOrigin::Runtime; }
			~StartupScope() { _startupThread = _previous; }
		private:
			StartupOrigin _previous;
		};
		static bool StartupOwned() noexcept { return _startupThread == StartupOrigin::Startup; }
		static bool StartupAttributed(bool general) noexcept
		{
			return _startupThread == StartupOrigin::Unspecified ? general : StartupOwned();
		}
		static void StartupWindow(bool active) noexcept { _startupWindow.store(active, std::memory_order_relaxed); }
		// One bounded mailbox: producers never wait for the consumer or each other.
		static void BackendError(int type, std::string_view text) noexcept
		{
			unsigned empty = 0;
			if (!_backendState.compare_exchange_strong(empty, 1, std::memory_order_acquire))
			{ _backendDropped.fetch_add(1, std::memory_order_relaxed); return; }
			_backend.Type = type;
			_backend.Startup = StartupAttributed(_startupWindow.load(std::memory_order_relaxed));
			_backend.Length = (std::min)(text.size(), _backend.Text.size());
			if (_backend.Length < text.size())
				while (_backend.Length && (static_cast<unsigned char>(text[_backend.Length]) & 0xc0) == 0x80) --_backend.Length;
			if (_backend.Length) std::memcpy(_backend.Text.data(), text.data(), _backend.Length);
			if (_backend.Length != text.size()) _backendDropped.fetch_add(1, std::memory_order_relaxed);
			_backendState.store(2, std::memory_order_release);
		}
		static std::optional<BackendDiagnostic> TakeBackendError() noexcept
		{
			unsigned ready = 2;
			if (!_backendState.compare_exchange_strong(ready, 3, std::memory_order_acquire)) return {};
			const auto result = _backend;
			_backendState.store(0, std::memory_order_release);
			return result;
		}
		static std::size_t ConsumeBackendDropped() noexcept { return _backendDropped.exchange(0); }
		static void AudioStatus(unsigned status) noexcept
		{
			if (!status) return;
			auto& target = StartupAttributed(_startupWindow.load(std::memory_order_relaxed)) ? _startupAudioStatus : _audioStatus;
			target.fetch_or(status, std::memory_order_relaxed);
		}
		static unsigned ConsumeAudioStatus() noexcept { return _audioStatus.exchange(0); }
		static unsigned ConsumeStartupAudioStatus() noexcept { return _startupAudioStatus.exchange(0); }
		static bool Realtime() noexcept { return _realtime; }
		static void Suppressed() noexcept { _suppressed.fetch_add(1, std::memory_order_relaxed); }
		static std::size_t ConsumeSuppressed() noexcept { return _suppressed.exchange(0); }
	private:
		static_assert(std::atomic<bool>::is_always_lock_free);
		static_assert(std::atomic<unsigned>::is_always_lock_free);
		static_assert(std::atomic<std::size_t>::is_always_lock_free);
		static inline BackendDiagnostic _backend;
		static inline std::atomic<unsigned> _backendState{0}, _audioStatus{0}, _startupAudioStatus{0};
		static inline std::atomic<std::size_t> _backendDropped{0};
		static inline thread_local bool _realtime = false;
		static inline thread_local StartupOrigin _startupThread = StartupOrigin::Unspecified;
		static inline std::atomic<bool> _startupWindow{false};
		static inline std::atomic<std::size_t> _suppressed{0};
	};
}
