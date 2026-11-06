#include "LocalMetronomeTiming.h"
#include <cmath>
#include <limits>

using namespace audio;

ninjam::NinjamMetronomeTimingResult LocalMetronomeTiming::Compute(const PlaybackRateBlock& block,
    double masterPhase, std::uint64_t masterLength, unsigned int beatsPerInterval,
    double masterRecordingRate, ninjam::NinjamMetronomeTimingState& state) noexcept
{
    ninjam::NinjamMetronomeTimingResult result;
    if (masterLength == 0u || masterLength > (std::numeric_limits<unsigned int>::max)()
        || beatsPerInterval == 0u || beatsPerInterval > masterLength || block.Frames == 0u
        || !std::isfinite(masterPhase) || !std::isfinite(masterRecordingRate) || masterRecordingRate <= 0.0)
    {
        state.Primed = false;
        return result;
    }
    result.Valid = true;
    result.GenerationReset = !state.Primed || state.IntervalLengthSamps != masterLength
        || state.Bpi != beatsPerInterval;
    state.Primed = true;
    state.IntervalLengthSamps = static_cast<unsigned int>(masterLength);
    state.Bpi = beatsPerInterval;

    auto position = std::fmod(masterPhase, static_cast<double>(masterLength));
    if (position < 0.0) position += masterLength;
    auto beat = static_cast<std::uint64_t>(position * beatsPerInterval / masterLength);
    const auto boundary = [masterLength, beatsPerInterval](std::uint64_t index) noexcept {
        return (index * masterLength + beatsPerInterval / 2u) / beatsPerInterval;
    };
    while (static_cast<double>(boundary(beat)) < position) ++beat;
    auto cursor = block.Cursor();
    for (unsigned int frame = 0u; frame < block.Frames; ++frame)
    {
        const auto end = position + cursor.Next() / masterRecordingRate;
        // Each physical frame owns [source edge, next source edge). This prevents
        // repeating slow-rate beats and retains beats crossed by a fast frame.
        while (static_cast<double>(boundary(beat)) < end)
        {
            if (result.OnsetCount == result.MaxOnsets) return result;
            result.Onsets[result.OnsetCount++] = {frame, false};
            if (beat % beatsPerInterval == 0u)
            {
                if (result.OnsetCount == result.MaxOnsets) return result;
                result.Onsets[result.OnsetCount++] = {frame, true};
            }
            ++beat;
        }
        position = end;
    }
    return result;
}
