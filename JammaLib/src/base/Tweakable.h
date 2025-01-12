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
			TWEAKSTATE_SELECTED = 1,
			TWEAKSTATE_MUTED = 2
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

		bool IsSelected() const
		{
			return _tweakState & TWEAKSTATE_SELECTED;
		}

		bool IsMuted() const
		{
			return _tweakState & TWEAKSTATE_MUTED;
		}

		virtual bool Select()
		{
			bool isAlreadySet = IsSelected();
			_tweakState |= TWEAKSTATE_SELECTED;

			return !isAlreadySet;
		}
		virtual bool DeSelect()
		{
			bool isAlreadyUnset = !IsSelected();
			_tweakState &= ~TWEAKSTATE_SELECTED;

			return !isAlreadyUnset;
		}

		virtual bool Mute()
		{
			bool isAlreadySet = IsMuted();
			_tweakState |= TWEAKSTATE_SELECTED;

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
