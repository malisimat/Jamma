#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace console
{
	// All sizes include bytes, not Unicode code points. A frame is length-prefixed
	// in little-endian order; its body is version, type, request ID, then UTF-8.
	inline constexpr std::uint8_t ProtocolVersion = 1;
	inline constexpr std::size_t MaxFrameBytes = 64 * 1024;
	inline constexpr std::size_t MaxInputBytes = 4 * 1024;
	inline constexpr std::size_t MaxEventBytes = 16 * 1024;
	inline constexpr std::size_t FrameHeaderBytes = 10;

	enum class MessageType : std::uint8_t
	{
		Hello = 1,
		Event,
		StatusSnapshot,
		CommandRequest,
		CommandResult,
		Shutdown
	};

	struct Message
	{
		MessageType Type;
		std::uint64_t RequestId = 0;
		std::string Text;
	};

	enum class FrameError
	{
		None,
		Incomplete,
		TooLarge,
		InvalidVersion,
		InvalidType,
		InvalidUtf8,
		InvalidSize,
		InvalidMessage
	};

	struct FrameResult
	{
		FrameError Error = FrameError::None;
		std::optional<Message> Value;
	};

	inline bool ValidUtf8(std::string_view text) noexcept
	{
		for (std::size_t i = 0; i < text.size();)
		{
			const auto first = static_cast<std::uint8_t>(text[i]);
			if (first < 0x80) { ++i; continue; }
			const std::size_t count = first >= 0xF0 && first <= 0xF4 ? 4
				: first >= 0xE0 && first <= 0xEF ? 3
				: first >= 0xC2 && first <= 0xDF ? 2 : 0;
			if (!count || i + count > text.size()) return false;
			for (std::size_t j = 1; j < count; ++j)
				if ((static_cast<std::uint8_t>(text[i + j]) & 0xC0) != 0x80) return false;
			const auto second = static_cast<std::uint8_t>(text[i + 1]);
			if ((first == 0xE0 && second < 0xA0)
				|| (first == 0xED && second >= 0xA0)
				|| (first == 0xF0 && second < 0x90)
				|| (first == 0xF4 && second >= 0x90)) return false;
			i += count;
		}
		return true;
	}

	inline bool ValidType(std::uint8_t type) noexcept
	{
		return type >= static_cast<std::uint8_t>(MessageType::Hello)
			&& type <= static_cast<std::uint8_t>(MessageType::Shutdown);
	}

	inline std::size_t MessageLimit(MessageType type) noexcept
	{
		if (type == MessageType::Shutdown) return 0;
		if (type == MessageType::Hello) return 128;
		if (type == MessageType::CommandRequest || type == MessageType::StatusSnapshot)
			return MaxInputBytes;
		return MaxEventBytes;
	}

	inline bool ValidMessage(MessageType type, std::uint64_t id, std::string_view text) noexcept
	{
		if (text.size() > MessageLimit(type)) return false;
		if (type == MessageType::Hello) return id == 0 && !text.empty();
		if (type == MessageType::Shutdown) return id == 0 && text.empty();
		if (type == MessageType::CommandRequest || type == MessageType::CommandResult)
			return id != 0;
		return id == 0;
	}

	inline std::optional<std::vector<std::uint8_t>> EncodeFrame(const Message& message)
	{
		if (!ValidType(static_cast<std::uint8_t>(message.Type))
			|| !ValidMessage(message.Type, message.RequestId, message.Text)
			|| message.Text.size() + FrameHeaderBytes > MaxFrameBytes
			|| !ValidUtf8(message.Text)) return std::nullopt;
		const auto size = static_cast<std::uint32_t>(message.Text.size() + FrameHeaderBytes);
		std::vector<std::uint8_t> bytes(4 + size);
		for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(size >> (i * 8));
		bytes[4] = ProtocolVersion;
		bytes[5] = static_cast<std::uint8_t>(message.Type);
		for (unsigned i = 0; i < 8; ++i)
			bytes[6 + i] = static_cast<std::uint8_t>(message.RequestId >> (i * 8));
		for (std::size_t i = 0; i < message.Text.size(); ++i)
			bytes[14 + i] = static_cast<std::uint8_t>(message.Text[i]);
		return bytes;
	}

	inline FrameResult DecodeFrame(std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() < 4) return { FrameError::Incomplete, std::nullopt };
		std::uint32_t size = 0;
		for (unsigned i = 0; i < 4; ++i) size |= static_cast<std::uint32_t>(bytes[i]) << (i * 8);
		if (size > MaxFrameBytes) return { FrameError::TooLarge, std::nullopt };
		if (size < FrameHeaderBytes) return { FrameError::InvalidSize, std::nullopt };
		if (bytes.size() < 4ull + size) return { FrameError::Incomplete, std::nullopt };
		if (bytes.size() != 4ull + size) return { FrameError::InvalidSize, std::nullopt };
		if (bytes[4] != ProtocolVersion) return { FrameError::InvalidVersion, std::nullopt };
		if (!ValidType(bytes[5])) return { FrameError::InvalidType, std::nullopt };
		const auto type = static_cast<MessageType>(bytes[5]);
		const auto textSize = size - FrameHeaderBytes;
		if (textSize > MessageLimit(type)) return { FrameError::TooLarge, std::nullopt };
		std::uint64_t id = 0;
		for (unsigned i = 0; i < 8; ++i) id |= static_cast<std::uint64_t>(bytes[6 + i]) << (i * 8);
		const std::string text(reinterpret_cast<const char*>(bytes.data() + 14), textSize);
		if (!ValidUtf8(text)) return { FrameError::InvalidUtf8, std::nullopt };
		if (!ValidMessage(type, id, text)) return { FrameError::InvalidMessage, std::nullopt };
		return { FrameError::None, Message{ type, id, text } };
	}
}
