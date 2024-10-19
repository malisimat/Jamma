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
	_lastValue = nextValue < _target ? _target : nextValue;

	if (_holdTicksRemaining > 0)
		_holdTicksRemaining--;

	if (0 == _holdTicksRemaining)
	{
		auto nextHoldValue = _holdValue - _fallingParams.HoldFallRate;
		_holdValue = nextHoldValue < _lastValue ? _lastValue : nextHoldValue;
	}

	return _lastValue;
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
	}

	if (_holdValue < _target)
	{
		_holdValue = _target;
		_holdTicksRemaining = _fallingParams.HoldSamps;
	}
}
