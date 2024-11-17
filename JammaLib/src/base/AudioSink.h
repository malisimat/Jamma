#pragma once

#include <vector>
#include <memory>
#include "Audible.h"

namespace base
{
	class AudioSink :
		public virtual Audible
	{
	public:
		AudioSink() : _writeIndex(0) {}

	public:
		virtual AudioDirection AudibleDirection() const override { return AUDIO_SINK; }
		virtual void Zero(unsigned int numSamps)
		{
			auto offsetInput = 0;
			auto offsetMonitor = 0;

			for (auto i = 0u; i < numSamps; i++)
			{
				offsetInput = OnMixWrite(0.0f, 0.0f, 1.0f, offsetInput, AUDIOSOURCE_INPUT);
				offsetMonitor = OnMixWrite(0.0f, 0.0f, 1.0f, offsetMonitor, AUDIOSOURCE_MONITOR);
			}
		}
		inline virtual int OnMixWrite(float samp,
			float fadeCurrent,
			float fadeNew,
			int indexOffset,
			AudioSourceType source) { return indexOffset; };
		virtual void EndWrite(unsigned int numSamps) { return EndWrite(numSamps, false); }
		virtual void EndWrite(unsigned int numSamps,
			bool updateIndex) = 0;

		std::shared_ptr<AudioSink> shared_from_this()
		{
			return std::dynamic_pointer_cast<AudioSink>(
				Audible::shared_from_this());
		}

	protected:
		unsigned long _writeIndex;
	};
}
