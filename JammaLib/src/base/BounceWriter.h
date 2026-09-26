#pragma once

#include <memory>

namespace base
{
	class MultiAudioSink;

	// Narrow audio-thread interface used by an active overdub target to mix its
	// source take. Lifetime is retained by the target for the complete tail.
	class BounceWriter
	{
	public:
		virtual ~BounceWriter() = default;

		virtual void WriteBlock(const std::shared_ptr<MultiAudioSink> dest,
			const float* srcBuf,
			unsigned int numSamps,
			unsigned int destChannel) = 0;
	};
}
