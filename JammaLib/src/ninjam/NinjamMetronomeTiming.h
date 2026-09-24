///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2026 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <array>

namespace ninjam
{
	struct NinjamMetronomeTimingInput
	{
		unsigned int IntervalPositionSamps = 0u;
		unsigned int IntervalLengthSamps = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
		unsigned int DeviceSampleRate = 0u;
		unsigned int OutputLatencySamps = 0u;
		unsigned int NumFrames = 0u;
	};

	struct NinjamMetronomeOnset
	{
		unsigned int Offset = 0u;
		bool Accent = false;
	};

	struct NinjamMetronomeTimingState
	{
		bool Primed = false;
		unsigned int IntervalLengthSamps = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
		unsigned int DeviceSampleRate = 0u;
	};

	struct NinjamMetronomeTimingResult
	{
		static constexpr unsigned int MaxOnsets = 128u;

		std::array<NinjamMetronomeOnset, MaxOnsets> Onsets{};
		unsigned int OnsetCount = 0u;
		bool Valid = false;
		bool GenerationReset = false;
	};

	class NinjamMetronomeTiming
	{
	public:
		static NinjamMetronomeTimingResult Compute(const NinjamMetronomeTimingInput& input,
			NinjamMetronomeTimingState& state) noexcept;
	};
}
