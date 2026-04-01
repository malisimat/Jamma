#pragma once

#include <vector>
#include <memory>
#include "Audible.h"

namespace base
{
	// Destination-centric block write request.
	// Bundles all context for a block-level write, so the destination
	// manages index arithmetic, ring-buffer wrap, and routing.
	struct AudioWriteRequest
	{
		const float* samples;            // Source sample buffer
		unsigned int numSamps;           // Number of samples to write
		unsigned int stride;             // 1 = contiguous, N = interleaved
		float fadeCurrent;               // Fade factor for existing content
		float fadeNew;                   // Fade factor for new content
		Audible::AudioSourceType source; // Audio source type

		AudioWriteRequest() noexcept :
			samples(nullptr),
			numSamps(0),
			stride(1),
			fadeCurrent(0.0f),
			fadeNew(1.0f),
			source(Audible::AUDIOSOURCE_ADC)
		{}
	};

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

		// Block-level write: writes a contiguous or strided block of samples.
		// Default falls back to per-sample OnMixWrite for backward compatibility.
		virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset)
		{
			auto offset = writeOffset;
			for (unsigned int i = 0; i < request.numSamps; i++)
			{
				offset = OnMixWrite(
					request.samples[i * request.stride],
					request.fadeCurrent,
					request.fadeNew,
					offset,
					request.source
				);
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
