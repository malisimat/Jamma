#include "InterpolatedValue.h"

using namespace audio;

InterpolatedValue::InterpolatedValue(InterpolatedValueParams faderParams) :
	_target(0.0)
{
}

double InterpolatedValue::Next()
{
	return _target;
}

double InterpolatedValue::Current() const
{
	return _target;
}

void InterpolatedValue::SetTarget(double target)
{
	_target = target;
}

InterpolatedValueLinear::InterpolatedValueLinear() :
	InterpolatedValue({}),
	_params({})
{
}

InterpolatedValueLinear::InterpolatedValueLinear(InterpolatedValueLinear::LinearParams linearParams) :
	InterpolatedValue(linearParams),
	_params(linearParams)
{
}

double InterpolatedValueLinear::Next()
{
	auto currentVal = _lastVal;

	if (_dVal > 0.0)
	{
		if (currentVal >= _target)
		{
			currentVal = _target;
			_dVal = 0.0;
		}
	}
	else if (_dVal < 0.0)
	{
		if (currentVal <= _target)
		{
			currentVal = _target;
			_dVal = 0.0;
		}
	}

	_lastVal = currentVal + _dVal;

	return _lastVal;
}

double InterpolatedValueLinear::Current() const
{
	return _lastVal;
}

void InterpolatedValueLinear::SetTarget(double target)
{
	if (_endVal != target)
	{
		_endVal = target;
		_dVal = (_endVal - _lastVal) * _params.Rate;
	}
	else
	{
		_dVal = 0.0;
	}
}

void InterpolatedValueLinear::Jump(double target)
{
	_lastVal = target;
	_endVal = target;
	_dVal = 0.0;
}

InterpolatedValueExp::InterpolatedValueExp() :
	InterpolatedValue({}),
	_lastVal(0.0),
	_params({})
{
}

InterpolatedValueExp::InterpolatedValueExp(InterpolatedValueExp::ExponentialParams expParams) :
	InterpolatedValue(expParams),
	_lastVal(0.0),
	_params(expParams)
{
}

double InterpolatedValueExp::Next()
{
	auto currentVal = _lastVal;
	_lastVal = currentVal + ((_target - currentVal) / _params.Damping);

	return _lastVal;
}

double InterpolatedValueExp::Current() const
{
	return _lastVal;
}

void InterpolatedValueExp::Jump(double target)
{
	_lastVal = target;
}
