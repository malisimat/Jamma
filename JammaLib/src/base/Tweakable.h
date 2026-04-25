#pragma once

#include <atomic>

namespace base
{
	class TweakableParams
	{
	};

	class Tweakable
	{
	public:
		Tweakable(TweakableParams params) :
			_tweakState(static_cast<int>(TWEAKSTATE_NONE)) {}
		~Tweakable() {}

		enum TweakState
		{
			TWEAKSTATE_NONE = 0,
			TWEAKSTATE_MUTED = 1
		};

		// Bitwise operators for setting flags easily
		friend TweakState operator|(TweakState a, TweakState b) {
			return static_cast<TweakState>(static_cast<int>(a) | static_cast<int>(b));
		}

		friend TweakState operator&(TweakState a, TweakState b) {
			return static_cast<TweakState>(static_cast<int>(a) & static_cast<int>(b));
		}

		friend TweakState operator~(TweakState a) {
			return static_cast<TweakState>(~static_cast<int>(a));
		}

		friend TweakState& operator|=(TweakState& a, TweakState b) {
			a = a | b;
			return a;
		}

		friend TweakState& operator&=(TweakState& a, TweakState b) {
			a = a & b;
			return a;
		}

	public:
		TweakState GetTweakState() const {
			return static_cast<TweakState>(_tweakState.load(std::memory_order_relaxed));
		}

		bool IsMuted() const
		{
			return GetTweakState() & TWEAKSTATE_MUTED;
		}

		virtual bool Mute()
		{
			auto prev = _tweakState.fetch_or(static_cast<int>(TWEAKSTATE_MUTED), std::memory_order_relaxed);
			return 0 == (prev & static_cast<int>(TWEAKSTATE_MUTED));
		}

		virtual bool UnMute()
		{
			auto prev = _tweakState.fetch_and(static_cast<int>(~TWEAKSTATE_MUTED), std::memory_order_relaxed);
			return 0 != (prev & static_cast<int>(TWEAKSTATE_MUTED));
		}

	protected:
		std::atomic<int> _tweakState;

	};
}
