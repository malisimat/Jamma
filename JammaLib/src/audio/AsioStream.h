#pragma once

#include "rtaudio/RtAudio.h"
#include <atomic>
#include <memory>
#include <thread>

namespace audio
{
	class AsioStreamTestAccess;

	// Jamma's lifecycle policy around the unmodified RtAudio ASIO backend.
	// Own this exact type: upstream RtAudio has a non-virtual destructor.
	class AsioStream : public RtAudio
	{
	public:
		class Backend : public RtApiAsio
		{
		public:
			using DriverStop = long (*)();
			explicit Backend(DriverStop stop = &StopDriver) : _stopDriver(stop), _owner(std::this_thread::get_id()) {}
			// Upstream's driver notifications call these virtual entry points.
			// They may arrive on the owner thread too, while a driver call is active.
			void stopStream() override { _resetRequested.store(true, std::memory_order_release); }
			void abortStream() override { stopStream(); }
			void StopFromOwner();
			void StartFromOwner();
			void closeStream() override;
			bool ResetRequested() const noexcept { return _resetRequested.load(std::memory_order_acquire); }
			bool IsActive() const noexcept { return stream_.state != STREAM_CLOSED && stream_.state != STREAM_STOPPED; }
		private:
			static long StopDriver();
			DriverStop _stopDriver;
			const std::thread::id _owner;
			std::atomic<bool> _resetRequested{false};
			bool _firstStart = true; // Lifecycle owner only.
			static_assert(std::atomic<bool>::is_always_lock_free);
		};

		AsioStream();
		void startStream() { static_cast<Backend*>(rtapi_)->StartFromOwner(); }
		void stopStream() { static_cast<Backend*>(rtapi_)->StopFromOwner(); }
		void abortStream() { stopStream(); }
		bool ResetRequested() const noexcept { return static_cast<const Backend*>(rtapi_)->ResetRequested(); }
		bool IsActive() const noexcept { return static_cast<const Backend*>(rtapi_)->IsActive(); }
	private:
		friend class AsioStreamTestAccess;
		explicit AsioStream(std::unique_ptr<Backend> backend);
	};
}
