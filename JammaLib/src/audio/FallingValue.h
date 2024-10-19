#pragma once

#include <vector>
#include <memory>

namespace audio
{
	class FallingValue
	{
	public:
		class FallingValueParams
		{
		public:
			double FallRate;
			double HoldFallRate;
			unsigned int HoldSamps;
		};

	public:
		FallingValue(FallingValueParams fallingParams);

	public:
		virtual double Next();
		virtual double Current() const;
		virtual double HoldValue() const;
		virtual void SetTarget(double target);

	protected:
		double _target;
		double _lastValue;
		double _holdValue;
		unsigned int _holdTicksRemaining;
		FallingValueParams _fallingParams;
	};
}
