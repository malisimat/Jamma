#pragma once

namespace base
{
	class TweakableParams
	{
	};

	class Tweakable
	{
	public:
		Tweakable(TweakableParams params) :
			_tweakState(TWEAKSTATE_DEFAULT) {}
		~Tweakable() {}

		enum TweakState
		{
			TWEAKSTATE_DEFAULT = 0,
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
			return _tweakState;
		}

		bool IsMuted() const
		{
			return _tweakState & TWEAKSTATE_MUTED;
		}

		virtual bool Mute()
		{
			bool isAlreadySet = IsMuted();
			_tweakState |= TWEAKSTATE_MUTED;

			return !isAlreadySet;
		}

		virtual bool UnMute()
		{
			bool isAlreadyUnset = !IsMuted();
			_tweakState &= ~TWEAKSTATE_MUTED;

			return !isAlreadyUnset;
		}


	protected:
		TweakState _tweakState;

	};
}
