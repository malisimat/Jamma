#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "JamFile.h"

namespace io
{
	// Jamma's lossless sample-domain MIDI asset. This intentionally is not SMF:
	// SMF tick timing cannot represent arbitrary sample offsets or automation
	// origins without rounding.
	class NativeMidiSidecar
	{
	public:
		static constexpr std::uint16_t CurrentMajor = 0u;
		static constexpr std::uint16_t CurrentMinor = 2u;
		static constexpr std::uint16_t CurrentPatch = 0u;
		static constexpr std::size_t MaxEvents = 4096u;
		static constexpr std::size_t MaxLanes = 8u;
		static constexpr std::size_t MaxPointsPerLane = 512u;
		static constexpr std::size_t MaxAssetBytes = 4u * 1024u * 1024u;

		struct Event
		{
			std::uint32_t SampleOffset = 0;
			std::uint8_t Status = 0;
			std::uint8_t Data1 = 0;
			std::uint8_t Data2 = 0;
		};

		struct Stream
		{
			std::uint32_t LogicalLength = 0;
			std::uint64_t AutomationGlobalSampleOrigin = 0;
			std::vector<Event> Events;
			std::vector<JamFile::AutomationLane> Lanes;
		};

		static bool ToStream(const Stream& stream, std::ostream& out, std::string* error = nullptr);
		static std::optional<Stream> FromStream(std::istream& in, std::string* error = nullptr);

	private:
		static bool Validate(const Stream& stream, std::string* error) noexcept;
		static bool WriteBytes(std::ostream& out, const void* data, std::size_t size) noexcept;
		static bool ReadBytes(std::istream& in, void* data, std::size_t size, std::size_t& consumed) noexcept;
		static void SetError(std::string* error, const char* text) noexcept;
	};
}
