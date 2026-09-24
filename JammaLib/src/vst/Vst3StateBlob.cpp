///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Vst3StateBlob.h"
#include <limits>

namespace vst::Vst3StateBlob
{
	static void AppendLe32(std::vector<std::uint8_t>& out, std::uint32_t value)
	{
		out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
		out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
		out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
		out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
	}

	static std::uint32_t ReadLe32(const std::uint8_t* bytes) noexcept
	{
		return static_cast<std::uint32_t>(bytes[0])
			| (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	}

	std::vector<std::uint8_t> Frame(const std::vector<std::uint8_t>& componentState,
		const std::vector<std::uint8_t>& controllerState)
	{
		if (componentState.size() > MaxSubBlobSize || controllerState.size() > MaxSubBlobSize)
			return {};

		const std::uint64_t n = componentState.size();
		const std::uint64_t m = controllerState.size();
		const std::uint64_t nm = n + m; // both bounded by MaxSubBlobSize, cannot overflow uint64

		constexpr std::uint64_t maxUint32 = (std::numeric_limits<std::uint32_t>::max)();
		if (nm > MaxSubBlobSize || (nm + 8u) > maxUint32)
			return {};

		const auto payloadSize = static_cast<std::uint32_t>(nm + 8u);

		std::vector<std::uint8_t> blob;
		blob.reserve(HeaderSize + static_cast<std::size_t>(nm));
		blob.push_back(FormatVersion);
		blob.push_back(StateType);
		AppendLe32(blob, payloadSize);
		AppendLe32(blob, static_cast<std::uint32_t>(n));
		AppendLe32(blob, static_cast<std::uint32_t>(m));
		blob.insert(blob.end(), componentState.begin(), componentState.end());
		blob.insert(blob.end(), controllerState.begin(), controllerState.end());
		return blob;
	}

	bool TryParse(const std::vector<std::uint8_t>& blob, ParsedState& out)
	{
		if (blob.size() < HeaderSize)
			return false;

		const auto version = blob[0];
		const auto stateType = blob[1];
		if (version != FormatVersion || stateType != StateType)
			return false;

		const auto payloadSize = ReadLe32(blob.data() + 2);
		const auto n = ReadLe32(blob.data() + 6);
		const auto m = ReadLe32(blob.data() + 10);

		if (n > MaxSubBlobSize || m > MaxSubBlobSize)
			return false;

		const std::uint64_t nm64 = static_cast<std::uint64_t>(n) + static_cast<std::uint64_t>(m);
		if (nm64 > MaxSubBlobSize)
			return false;

		constexpr std::uint64_t maxUint32 = (std::numeric_limits<std::uint32_t>::max)();
		const std::uint64_t expectedPayload = nm64 + 8u;
		if (expectedPayload > maxUint32 || payloadSize != static_cast<std::uint32_t>(expectedPayload))
			return false;

		const std::uint64_t totalSize64 = static_cast<std::uint64_t>(HeaderSize) + nm64;
		if (totalSize64 != static_cast<std::uint64_t>(blob.size()))
			return false; // truncated header/payload, or trailing bytes

		const auto componentBegin = blob.begin() + static_cast<std::ptrdiff_t>(HeaderSize);
		const auto componentEnd = componentBegin + static_cast<std::ptrdiff_t>(n);
		const auto controllerEnd = componentEnd + static_cast<std::ptrdiff_t>(m);

		ParsedState parsed;
		parsed.ComponentState.assign(componentBegin, componentEnd);
		parsed.ControllerState.assign(componentEnd, controllerEnd);
		out = std::move(parsed);
		return true;
	}
}
