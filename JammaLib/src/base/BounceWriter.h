#pragma once

#include <memory>

namespace base
{
	class MultiAudioSink;

	// The overdub target retains this writer through its tail for safe callback access.
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
