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
		unsigned int intervalPositionSamps = 0u;
		unsigned int intervalLengthSamps = 0u;
		float bpm = 0.0f;
		unsigned int bpi = 0u;
		unsigned int deviceSampleRate = 0u;
		unsigned int outputLatencySamps = 0u;
		unsigned int numFrames = 0u;
	};

	struct NinjamMetronomeOnset
	{
		unsigned int offset = 0u;
		bool accent = false;
	};

	struct NinjamMetronomeTimingState
	{
		bool primed = false;
		unsigned int intervalLengthSamps = 0u;
		float bpm = 0.0f;
		unsigned int bpi = 0u;
		unsigned int deviceSampleRate = 0u;
	};

	struct NinjamMetronomeTimingResult
	{
		static constexpr unsigned int MaxOnsets = 128u;

		std::array<NinjamMetronomeOnset, MaxOnsets> onsets{};
		unsigned int onsetCount = 0u;
		bool valid = false;
		bool generationReset = false;
	};

	class NinjamMetronomeTiming
	{
	public:
		static NinjamMetronomeTimingResult Compute(const NinjamMetronomeTimingInput& input,
			NinjamMetronomeTimingState& state) noexcept;
	};
}