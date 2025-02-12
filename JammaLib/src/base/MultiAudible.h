#pragma once

#include <memory>
#include "Audible.h"
#include "Sharable.h"

namespace base
{
	class MultiAudible :
		public virtual Sharable
	{
	public:
		enum MultiAudioPlugType
		{
			MULTIAUDIOPLUG_NONE,
			MULTIAUDIOPLUG_SOURCE,
			MULTIAUDIOPLUG_SINK,
			MULTIAUDIOPLUG_BOTH
		};
	public:
		MultiAudible() {};
		~MultiAudible() {};

	public:
		virtual MultiAudioPlugType MultiAudioPlug() const { return MULTIAUDIOPLUG_NONE; }
	};
}
