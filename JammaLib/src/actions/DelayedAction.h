#pragma once

#include "Tickable.h"

namespace actions
{
	class DelayedAction : base::Tickable
	{
	public:
		class DelayedAction(unsigned int sampsDelay, double target);

	public:
		virtual void OnTick(Time curTime,
			unsigned int samps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params) override;

		unsigned int SampsLeft(unsigned int samp);
		double GetTarget() const;

	protected:
		unsigned int _sampsDelay;
		unsigned int _sampsDelayLeft;
		double _target;
	};
}
