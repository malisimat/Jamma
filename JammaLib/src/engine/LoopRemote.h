#pragma once

#include <atomic>
#include <chrono>
#include "Loop.h"

namespace engine
{
	// A loop that receives audio streamed in from a remote ninjam user.
	// Audio is written into the loop's BufferBank so it can be played back
	// and visualised through the standard Loop playback path.
	//
	// Visual model updates are throttled to a maximum rate to avoid excessive
	// CPU cost when many remote users are active. The waveform is rendered via
	// vertex-shader displacement from a 1D texture, so the per-update cost is
	// limited to decimation + texture upload (no geometry rebuild).
	class LoopRemote :
		public Loop
	{
	public:
		LoopRemote(LoopParams params,
			audio::AudioMixerParams mixerParams);

		virtual std::string ClassName() const override { return "LoopRemote"; }
		virtual void Update() override;

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
		virtual double _DrawRadiusScale() const noexcept override { return 0.5; }

		std::atomic<bool> _modelDirty;
		std::atomic<unsigned int> _measureLengthSamps;
		std::atomic<unsigned int> _measurePositionSamps;
		std::atomic<unsigned int> _visualLengthSamps;

		// Throttle: visual updates are limited to _MaxVisualUpdateHz.
		static constexpr double _MaxVisualUpdateHz = 15.0;
		std::chrono::steady_clock::time_point _lastVisualUpdate;
	};
}
