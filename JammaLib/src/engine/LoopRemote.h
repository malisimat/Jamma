#pragma once

#include <atomic>
#include "Loop.h"

namespace engine
{
	// A loop that receives audio streamed in from a remote ninjam user.
	// Audio is written into the loop's BufferBank so it can be played back
	// and visualised through the standard Loop playback path.
	class LoopRemote :
		public Loop
	{
	public:
		LoopRemote(LoopParams params,
			audio::AudioMixerParams mixerParams);

		virtual std::string ClassName() const override { return "LoopRemote"; }

		// Called from the job thread. Resizes buffer to match the ninjam interval.
		void SetMeasureLength(unsigned int measureLengthSamps);
		// Called from the job thread. Adjusts the read cursor within the interval.
		void SetMeasurePosition(unsigned int positionSamps);
		// Called from the audio callback. Writes decoded samples into the ring buffer.
		void IngestSamples(const float* samples, unsigned int numSamps);
		void SetVisualLength(unsigned int visualLengthSamps);

		unsigned int MeasureLength() const { return _measureLengthSamps.load(); }
		unsigned int MeasurePosition() const { return _measurePositionSamps.load(); }
		unsigned int VisualLength() const { return _visualLengthSamps.load(); }

	protected:
		virtual unsigned long _ModelDisplayLength(bool isRecording, unsigned long actualLoopLength) const override;

	private:
		std::atomic<unsigned int> _measureLengthSamps;
		std::atomic<unsigned int> _measurePositionSamps;
		std::atomic<unsigned int> _visualLengthSamps;
	};
}
