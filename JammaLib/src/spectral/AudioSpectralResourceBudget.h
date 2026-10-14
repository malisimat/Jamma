#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include "AudioSpectralConfig.h"

namespace spectral
{
	// One explicit owner per session/editor. Admissions and final token destruction
	// are off audio; immutable sources pin their reservation through jobs/history.
	class AudioSpectralResourceBudget : public std::enable_shared_from_this<AudioSpectralResourceBudget>
	{
	public:
		class Reservation
		{
		public:
			Reservation(std::shared_ptr<AudioSpectralResourceBudget> owner, std::uint64_t bytes)
				: _owner(std::move(owner)), _bytes(bytes) {}
			~Reservation() { _owner->_used.fetch_sub(_bytes, std::memory_order_relaxed); }
			Reservation(const Reservation&) = delete;
			Reservation& operator=(const Reservation&) = delete;
			std::uint64_t Bytes() const noexcept { return _bytes; }
		private:
			const std::shared_ptr<AudioSpectralResourceBudget> _owner;
			const std::uint64_t _bytes;
		};

		explicit AudioSpectralResourceBudget(std::uint64_t limit = AudioSpectralConfig::MaxWorkingBytes)
			: _limit(limit) {}
		std::uint64_t UsedBytes() const noexcept { return _used.load(std::memory_order_relaxed); }
		std::shared_ptr<const Reservation> Reserve(std::uint64_t bytes, std::string& reason)
		{
			reason.clear();
			auto used = _used.load(std::memory_order_relaxed);
			for (;;)
			{
				if (bytes > _limit || used > _limit - bytes)
				{
					reason = "Session spectral memory budget is full; existing edits are preserved";
					return nullptr;
				}
				if (_used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed)) break;
			}
			try { return std::make_shared<Reservation>(shared_from_this(), bytes); }
			catch (const std::bad_alloc&)
			{
				_used.fetch_sub(bytes, std::memory_order_relaxed);
				reason = "Not enough memory for spectral resource admission";
				return nullptr;
			}
		}
	private:
		const std::uint64_t _limit;
		std::atomic<std::uint64_t> _used{ 0u };
	};
}
