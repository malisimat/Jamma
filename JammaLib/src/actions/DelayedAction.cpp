#include "DelayedAction.h"

using namespace actions;

DelayedAction::DelayedAction(unsigned int sampsDelay,
	double target) :
	_sampsDelay(sampsDelay),
	_sampsDelayLeft(sampsDelay),
	_target(target)
{
}

unsigned int DelayedAction::SampsLeft(unsigned int samps)
{
	if (samps >= _sampsDelayLeft)
		return 0u;

	return _sampsDelayLeft - samps;
}

double DelayedAction::GetTarget() const
{
	return _target;
}

void DelayedAction::OnTick(Time curTime,
	unsigned int samps,
	std::optional<io::UserConfig> cfg,
	std::optional<audio::AudioStreamParams> params)
{
	if (samps >= _sampsDelayLeft)
		_sampsDelayLeft = 0u;
	else
		_sampsDelayLeft -= samps;
}