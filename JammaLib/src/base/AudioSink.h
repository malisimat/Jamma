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
				offsetInput = OnOverwrite(0.0f, offsetInput, AUDIOSOURCE_INPUT);
				offsetMonitor = OnOverwrite(0.0f, offsetMonitor, AUDIOSOURCE_MONITOR);
			}
		}
		inline virtual int OnWrite(float samp,
			int indexOffset,
			AudioSourceType source) { return indexOffset; };
		inline virtual int OnOverwrite(float samp,
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
