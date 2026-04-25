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
			_tweakState(TWEAKSTATE_NONE) {}
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
			return _tweakState.load(std::memory_order_relaxed);
		}

		bool IsMuted() const
		{
			return GetTweakState() & TWEAKSTATE_MUTED;
		}

		virtual bool Mute()
		{
			bool isAlreadySet = IsMuted();
			auto tweakState = GetTweakState();
			tweakState |= TWEAKSTATE_MUTED;
			_tweakState.store(tweakState, std::memory_order_relaxed);

			return !isAlreadySet;
		}

		virtual bool UnMute()
		{
			bool isAlreadyUnset = !IsMuted();
			auto tweakState = GetTweakState();
			tweakState &= ~TWEAKSTATE_MUTED;
			_tweakState.store(tweakState, std::memory_order_relaxed);

			return !isAlreadyUnset;
		}

	protected:
		std::atomic<TweakState> _tweakState;

	};
}
