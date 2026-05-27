#include "MidiQuantisation.h"

#include <array>
#include <cstring>

using namespace engine;

namespace
{
	static constexpr std::size_t TotalNoteSlots = 16u * 128u;

	constexpr std::size_t NoteSlot(std::uint8_t channel, std::uint8_t note) noexcept
	{
		return (static_cast<std::size_t>(channel & MidiEvent::ChannelMask) << 7) | (note & 0x7F);
	}
}

std::uint32_t engine::QuantiseSampleOffset(std::uint32_t offset,
                                            std::uint32_t step,
                                            std::uint32_t loopLength) noexcept
{
	if (0u == step || 0u == loopLength)
		return offset;

	// Round-to-nearest: snapped = ((offset + step/2) / step) * step
	const auto halfStep = step / 2u;
	const auto snapped = ((offset + halfStep) / step) * step;

	// Wrap into [0, loopLength) so notes near the loop end snap back to 0
	// rather than escaping the loop window.
	return snapped % loopLength;
}

void engine::QuantiseEvents(const MidiEvent* src,
                             std::size_t eventCount,
                             std::uint32_t loopLength,
                             std::uint32_t stepSamps,
                             MidiEvent* dst) noexcept
{
	if (nullptr == src || nullptr == dst || 0u == eventCount)
		return;

	if (0u == stepSamps || 0u == loopLength)
	{
		std::memcpy(dst, src, eventCount * sizeof(MidiEvent));
		return;
	}

	// Per (channel, note) slot remember the signed delta applied to the most
	// recent unmatched NoteOn so the matching NoteOff can ride the same shift.
	// Use a sentinel `hasPending` array to distinguish "no pending shift" from
	// "shift of 0 samples".
	std::array<std::int64_t, TotalNoteSlots> pendingDelta{};
	std::array<bool, TotalNoteSlots> hasPending{};

	for (std::size_t i = 0; i < eventCount; ++i)
	{
		MidiEvent ev = src[i];
		const auto slot = NoteSlot(ev.Channel(), ev.data1);

		if (ev.IsNoteOn())
		{
			if (ev.sampleOffset < loopLength)
			{
				const auto quantised = QuantiseSampleOffset(ev.sampleOffset, stepSamps, loopLength);
				pendingDelta[slot] = static_cast<std::int64_t>(quantised) -
					static_cast<std::int64_t>(ev.sampleOffset);
				hasPending[slot] = true;
				ev.sampleOffset = quantised;
			}
		}
		else if (ev.IsNoteOff())
		{
			if (hasPending[slot])
			{
				const auto delta = pendingDelta[slot];
				const auto shifted = static_cast<std::int64_t>(ev.sampleOffset) + delta;
				const auto loopEnd = static_cast<std::int64_t>(loopLength);

				if (shifted < 0)
				{
					ev.sampleOffset = 0u;
				}
				else if (shifted >= loopEnd)
				{
					// Clamp to last sample of the loop so the NoteOff still fires.
					ev.sampleOffset = static_cast<std::uint32_t>(loopEnd - 1);
				}
				else
				{
					ev.sampleOffset = static_cast<std::uint32_t>(shifted);
				}

				hasPending[slot] = false;
				pendingDelta[slot] = 0;
			}
		}

		dst[i] = ev;
	}
}
