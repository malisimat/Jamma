#pragma once

#include <optional>
#include "../engine/Timer.h"
#include "../io/UserConfig.h"
#include "../audio/AudioDevice.h"

namespace base
{
	class Tickable
	{
	public:
		virtual void OnTick(Time curTime,
			unsigned int samps,
			std::optional<io::UserConfig> cfg,
			std::optional<audio::AudioStreamParams> params) = 0;
	};
}
