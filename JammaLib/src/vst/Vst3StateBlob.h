///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vst
{
	// Self-describing container for a VST3 IComponent + IEditController state
	// pair, carried inside the outer per-plugin blob format shared with
	// Vst2Plugin (byte 0 = format version, byte 1 = state-type flag). This is
	// state type 2 ("VST3 component/controller state").
	//
	// Layout (all multi-byte integers little-endian):
	//   offset  size  field
	//   0       1     format version = 1
	//   1       1     state type = 2 (VST3 component/controller state)
	//   2       4     payload byte count (= 8 + N + M)
	//   6       4     component-state byte count (N)
	//   10      4     controller-state byte count (M)
	//   14      N     component-state bytes
	//   14+N    M     controller-state bytes
	//
	// Pure byte-level logic; does not depend on the VST3 SDK or any plugin
	// object, so it can be unit-tested without loading a real plugin.
	namespace Vst3StateBlob
	{
		constexpr std::uint8_t FormatVersion = 1u;
		constexpr std::uint8_t StateType = 2u;
		constexpr std::size_t HeaderSize = 14u;

		// IBStream::read/write byte counts are signed int32; reject any
		// sub-blob that could not be represented as such a count.
		constexpr std::size_t MaxSubBlobSize = 0x7FFFFFFFu;

		// Frame componentState and controllerState into a self-describing
		// blob. Returns an empty vector if either input exceeds
		// MaxSubBlobSize, or if the combined size would overflow size_t or
		// the blob's uint32 payload-size field.
		std::vector<std::uint8_t> Frame(const std::vector<std::uint8_t>& componentState,
			const std::vector<std::uint8_t>& controllerState);

		struct ParsedState
		{
			std::vector<std::uint8_t> ComponentState;
			std::vector<std::uint8_t> ControllerState;
		};

		// Validate and split a blob produced by Frame(). Returns false for
		// any malformed input: unknown version/type, a header shorter than
		// HeaderSize, an inconsistent declared payload size, an oversized
		// sub-blob, or trailing/truncated bytes. `out` is left untouched on
		// failure.
		bool TryParse(const std::vector<std::uint8_t>& blob, ParsedState& out);
	}
}
