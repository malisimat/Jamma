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
			static const float zeros[4096] = {};
			auto sampsToZero = (numSamps <= 4096u) ? numSamps : 4096u;

			AudioWriteRequest reqAdc;
			reqAdc.samples = zeros;
			reqAdc.numSamps = sampsToZero;
			reqAdc.stride = 1;
			reqAdc.fadeCurrent = 0.0f;
			reqAdc.fadeNew = 1.0f;
			reqAdc.source = AUDIOSOURCE_ADC;
			OnBlockWrite(reqAdc, 0);

			AudioWriteRequest reqMon;
			reqMon.samples = zeros;
			reqMon.numSamps = sampsToZero;
			reqMon.stride = 1;
			reqMon.fadeCurrent = 0.0f;
			reqMon.fadeNew = 1.0f;
			reqMon.source = AUDIOSOURCE_MONITOR;
			OnBlockWrite(reqMon, 0);
		}

		// Block-level write: writes a contiguous or strided block of samples.
		virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) {}

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
