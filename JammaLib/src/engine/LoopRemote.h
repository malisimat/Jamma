#pragma once

#include "Loop.h"

namespace engine
{
	class LoopRemote :
		public Loop
	{
	public:
		LoopRemote(LoopParams params,
			audio::AudioMixerParams mixerParams);

		virtual std::string ClassName() const override { return "LoopRemote"; }

		void SetMeasureLength(unsigned int measureLengthSamps);
		void SetMeasurePosition(unsigned int positionSamps);
		void IngestSamples(const float* samples, unsigned int numSamps);

		unsigned int MeasureLength() const { return _measureLengthSamps; }
		unsigned int MeasurePosition() const { return _measurePositionSamps; }

	private:
		unsigned int _measureLengthSamps;
		unsigned int _measurePositionSamps;
	};
}
