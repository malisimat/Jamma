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
		virtual AudioPlugType AudioPlug() const override { return AUDIOPLUG_SINK; }
		virtual void Zero(unsigned int numSamps)
		{
			auto offsetInput = 0;
			auto offsetMonitor = 0;

			for (auto i = 0u; i < numSamps; i++)
			{
				offsetInput = OnMixWrite(0.0f, 0.0f, 1.0f, offsetInput, AUDIOSOURCE_ADC);
				offsetMonitor = OnMixWrite(0.0f, 0.0f, 1.0f, offsetMonitor, AUDIOSOURCE_MONITOR);
			}
		}
		inline virtual int OnMixWrite(float samp,
			float fadeCurrent,
			float fadeNew,
			int indexOffset,
			AudioSourceType source) { return indexOffset; };
		// Block-level write: writes numSamps from contiguous data array.
		// Reduces per-sample virtual call overhead for pure-routing paths.
		// Default falls back to per-sample OnMixWrite.
		virtual void OnBlockWrite(const float* data,
			unsigned int numSamps,
			int indexOffset,
			float fadeCurrent,
			float fadeNew,
			AudioSourceType source)
		{
			for (auto i = 0u; i < numSamps; i++)
			{
				OnMixWrite(data[i], fadeCurrent, fadeNew, indexOffset + (int)i, source);
			}
		}
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
