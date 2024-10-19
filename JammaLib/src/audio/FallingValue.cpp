#include "FallingValue.h"

using namespace audio;

FallingValue::FallingValue(FallingValueParams fallingParams) :
	_target(0.0),
	_lastValue(0.0),
	_holdValue(0.0),
	_holdTicksRemaining(0u),
	_fallingParams(fallingParams)
{
}

double FallingValue::Next()
{
	auto nextValue = _lastValue - _fallingParams.FallRate;
	nextValue = nextValue < _target ? _target : nextValue;

	if (_holdTicksRemaining > 0)
		_holdTicksRemaining--;

	if (0 == _holdTicksRemaining)
	{
		_holdValue = nextValue;
		_holdTicksRemaining = _fallingParams.HoldSamps;
	}

	_lastValue = nextValue;
	return nextValue;
}

double FallingValue::Current() const
{
	return _lastValue;
}

double FallingValue::HoldValue() const
{
	return _holdValue;
}

void FallingValue::SetTarget(double target)
{
	_target = std::abs(target);

	if (_lastValue < _target)
	{
		_lastValue = _target;
		_holdValue = _target;
		_holdTicksRemaining = _fallingParams.HoldSamps;
	}
}
