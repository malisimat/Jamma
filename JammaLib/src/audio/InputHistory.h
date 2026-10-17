#pragma once
#include <cstdint>
#include "../midi/MidiBlockTiming.h"

namespace audio
{
    // Borrowed only during one audio callback. The owner must not ingest/resize
    // until the render returns. No cursor mutation or sample ownership transfer.
    struct InputHistoryChannel
    {
        const float* Samples = nullptr;
        unsigned int Capacity = 0, WriteHead = 0;
    };

    struct InputHistoryView
    {
        const InputHistoryChannel* Channels = nullptr;
        unsigned int ChannelCount = 0, ValidCount = 0, InputLatency = 0;
        std::uint64_t IngressHead = 0, EpochStart = 0;
        unsigned int* ReadCount = nullptr; // optional callback-owned diagnostic, never retained

        bool ContainsOrigin(std::uint32_t trigger) const noexcept
        {
            const auto elapsed = midi::MidiSampleDelta(static_cast<std::uint32_t>(IngressHead), trigger);
            return Channels && elapsed >= 0 && static_cast<std::uint64_t>(elapsed) <= IngressHead - EpochStart;
        }
        bool Read(unsigned int channel, std::uint32_t physical, float& sample) const noexcept
        {
            if (ReadCount) ++*ReadCount;
            if (!Channels || channel >= ChannelCount) return false;
            const auto age = static_cast<std::int64_t>(midi::MidiSampleDelta(static_cast<std::uint32_t>(IngressHead), physical)) - InputLatency;
            const auto& ring = Channels[channel];
            if (age < 1 || age > ValidCount || age > ring.Capacity || !ring.Samples) return false;
            const auto index = (static_cast<std::uint64_t>(ring.WriteHead) + ring.Capacity - age) % ring.Capacity;
            sample = ring.Samples[index];
            return true;
        }
    };

    // Explicit DAC capability. Never pass to bounce, source capture or export.
    struct DacRenderContext
    {
        InputHistoryView History;
    };
}
