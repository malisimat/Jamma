#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

namespace audio
{
	// Prepared with a take and retained by its mixers and replacement captures.
	// Claims are separate from performer mute/level settings. Both job-owned
	// replacement captures and audio-owned punch sessions can hold one claim.
	class CaptureSourceMuteControl
	{
	public:
		void Acquire() noexcept { _claims.fetch_add(1u, std::memory_order_acq_rel); }
		void Release() noexcept
		{
			const auto previous = _claims.fetch_sub(1u, std::memory_order_acq_rel);
			assert(previous > 0u);
		}
		void AcquireReplacement() noexcept
		{
			_replacementRevision.fetch_add(1u, std::memory_order_acq_rel);
			_replacementMuted.store(true, std::memory_order_release);
			_replacementClaims.fetch_add(1u, std::memory_order_acq_rel);
		}
		void ReleaseReplacement() noexcept
		{
			const auto previous = _replacementClaims.fetch_sub(1u, std::memory_order_acq_rel);
			assert(previous > 0u);
		}
		// Explicit performer UnMute auditions completed source material. Claim
		// ownership remains intact; the next replacement re-enables its mute.
		bool AllowReplacementPlayback() noexcept
		{
			const bool wasMuted = _replacementMuted.exchange(false, std::memory_order_acq_rel);
			return wasMuted && _replacementClaims.load(std::memory_order_acquire) != 0u;
		}
		bool IsMuted() const noexcept
		{
			return IsTransientMuted() || IsReplacementMuted();
		}
		bool IsTransientMuted() const noexcept { return _claims.load(std::memory_order_acquire) != 0u; }
		bool IsReplacementMuted() const noexcept {
			return _replacementClaims.load(std::memory_order_acquire) != 0u &&
				_replacementMuted.load(std::memory_order_acquire);
		}
		std::uint64_t ReplacementRevision() const noexcept {
			return _replacementRevision.load(std::memory_order_acquire);
		}
	private:
		static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
		static_assert(std::atomic<bool>::is_always_lock_free);
		std::atomic<std::uint32_t> _claims{0u};
		std::atomic<std::uint32_t> _replacementClaims{0u};
		std::atomic<bool> _replacementMuted{true};
		std::atomic<std::uint64_t> _replacementRevision{0u};
	};
}
