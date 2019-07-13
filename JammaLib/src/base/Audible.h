#pragma once

#include "Sharable.h"

namespace base
{
	class Audible :
		public virtual Sharable
	{
	public:
		enum AudioDirection
		{
			AUDIO_NONE,
			AUDIO_SOURCE,
			AUDIO_SINK,
			AUDIO_BOTH
		};

	public:
		Audible() {};
		~Audible() {};

	public:
		virtual AudioDirection AudibleDirection() const { return AUDIO_NONE; }
	};
}
