#pragma once

#include "Sharable.h"

namespace base
{
	class Audible :
		public virtual Sharable
	{
	public:
		enum AudioPlugType
		{
			AUDIOPLUG_NONE,
			AUDIOPLUG_SOURCE,
			AUDIOPLUG_SINK,
			AUDIOPLUG_BOTH
		};

		enum AudioSourceType
		{
			AUDIOSOURCE_ADC,
			AUDIOSOURCE_MONITOR,
			AUDIOSOURCE_BOUNCE,
			AUDIOSOURCE_LOOPS
		};

	public:
		Audible() {};
		~Audible() {};

	public:
		virtual AudioPlugType AudioPlug() const { return AUDIOPLUG_NONE; }
	};
}
