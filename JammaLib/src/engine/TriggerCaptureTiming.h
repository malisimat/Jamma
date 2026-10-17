#pragma once
#include <cstdint>
#include <optional>

namespace engine
{
	// Frozen by the structural capture owner before publication. Device samples
	// wrap at uint32_t; use MidiSampleDelta when comparing them.
	struct TriggerCaptureTiming
	{
		std::optional<std::uint32_t> TriggerSample;
		std::int64_t DelaySamps = 0;
		std::uint32_t CapacitySamps = 0u;
		std::uint32_t SourceAgeSamps = 0u;
		std::uint32_t SeamSamps = 0u;
		std::optional<std::int64_t> ContentTransportOrigin;
	};
}
