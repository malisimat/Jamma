#pragma once

#include <Windows.h>
#include <climits>
#include <optional>
#include <string>
#include <string_view>

namespace console
{
	inline std::optional<std::wstring> Utf8ToUtf16(std::string_view text)
	{
		if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX))
			return std::nullopt;
		const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			text.data(), static_cast<int>(text.size()), nullptr, 0);
		if (length <= 0) return std::nullopt;
		std::wstring wide(static_cast<std::size_t>(length), L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
			static_cast<int>(text.size()), wide.data(), length) != length)
			return std::nullopt;
		return wide;
	}

	inline std::optional<std::wstring> ClipboardUtf8ToUtf16(std::string_view text)
	{
		if (text.find('\0') != std::string_view::npos) return std::nullopt;
		auto wide = Utf8ToUtf16(text);
		if (!wide) return std::nullopt;
		std::wstring result;
		result.reserve(wide->size());
		for (std::size_t at = 0; at < wide->size(); ++at)
		{
			if ((*wide)[at] == L'\r')
			{
				result += L"\r\n";
				if (at + 1 < wide->size() && (*wide)[at + 1] == L'\n') ++at;
			}
			else if ((*wide)[at] == L'\n') result += L"\r\n";
			else result += (*wide)[at];
		}
		return result;
	}
}
