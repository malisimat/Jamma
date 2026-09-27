#pragma once

#include <cstdint>

namespace midi
{
	enum class MidiTimestampSource : std::uint8_t
	{
		InitialArrival,
		DriverDelta,
		InvalidDeltaFallback,
		DiscontinuityFallback
	};

	struct MidiDriverTimestamp
	{
		std::int64_t EventMicros = 0;
		MidiTimestampSource Source = MidiTimestampSource::InitialArrival;
	};

	// Callback-thread-owned state. RtMidi's first delta has no known epoch, so
	// the first callback arrival seeds the timeline; later valid deltas preserve
	// the driver's inter-event spacing, including equal-time events.
	class MidiDriverTimestampMapper
	{
	public:
		MidiDriverTimestamp Map(double deltaSeconds, std::int64_t callbackArrivalMicros) noexcept;
		void Reset() noexcept;

	private:
		bool _initialized = false;
		std::int64_t _lastEventMicros = 0;
	};

	std::uint64_t MapMidiTimestampToAudioSample(unsigned int sampleRate,
		std::uint64_t anchorSample,
		std::int64_t anchorMicros,
		std::int64_t eventMicros) noexcept;
}
