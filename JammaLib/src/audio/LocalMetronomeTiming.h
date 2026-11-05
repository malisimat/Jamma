#pragma once

#include "PlaybackRate.h"
#include "../ninjam/NinjamMetronomeTiming.h"

namespace audio
{
    // Local beats use source geometry; returned offsets always address device frames.
    class LocalMetronomeTiming
    {
    public:
        static ninjam::NinjamMetronomeTimingResult Compute(const PlaybackRateBlock& block,
            double masterPhase, std::uint64_t masterLength, unsigned int beatsPerInterval,
            double masterRecordingRate, ninjam::NinjamMetronomeTimingState& state) noexcept;
    };
}
