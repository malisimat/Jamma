#include "AsioStream.h"
#include "rtaudio/include/asio.h"
#include <utility>

namespace audio
{
	AsioStream::AsioStream() : RtAudio(RtAudio::WINDOWS_ASIO)
	{
		// Both instances are unopened. Construct before replacing so allocation or
		// COM setup failure leaves base-class destruction with a valid backend.
		auto backend = std::make_unique<Backend>();
		delete std::exchange(rtapi_, backend.release());
	}

	AsioStream::AsioStream(std::unique_ptr<Backend> backend) : RtAudio(RtAudio::WINDOWS_ASIO)
	{
		delete std::exchange(rtapi_, backend.release());
	}

	long AsioStream::Backend::StopDriver() { return ASIOStop(); }

	void AsioStream::Backend::StartFromOwner()
	{
		if (std::this_thread::get_id() != _owner)
		{
			_resetRequested.store(true, std::memory_order_release);
			return;
		}
		// Buffer creation can report the prepared rate before the first start.
		// Preserve notifications raised during start itself and every later resume.
		if (_firstStart) { _resetRequested.store(false, std::memory_order_release); _firstStart = false; }
		startStream();
	}

	void AsioStream::Backend::StopFromOwner()
	{
		if (std::this_thread::get_id() != _owner)
		{
			_resetRequested.store(true, std::memory_order_release);
			return;
		}
		if (!IsActive()) return;
		// A vanished device cannot drain more output callbacks. ASIOStop itself
		// is the lifetime barrier; its success, not silence/heartbeat, allows close.
		const auto result = _stopDriver();
		if (result == ASE_OK) { stream_.state = STREAM_STOPPED; return; }
		// Failure leaves the previous active/stopping state untouched: callbacks
		// may still be reading it, and no lifetime barrier has been established.
		const std::string message = "ASIO driver stop failed (" + std::to_string(result) + "); backend retained";
		// RtApi::error automatically aborts on a non-warning: that would recurse
		// into stopStream and could obscure the original failed lifetime barrier.
		const auto callback = reinterpret_cast<RtAudioErrorCallback>(stream_.callbackInfo.errorCallback);
		if (callback) callback(RtAudioError::SYSTEM_ERROR, message);
		else throw RtAudioError(message, RtAudioError::SYSTEM_ERROR);
	}

	void AsioStream::Backend::closeStream()
	{
		if (std::this_thread::get_id() != _owner)
		{
			_resetRequested.store(true, std::memory_order_release);
			return;
		}
		if (IsActive()) StopFromOwner();
		if (IsActive()) return;
		if (!isStreamOpen()) return;
		RtApiAsio::closeStream();
	}
}
