///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include "StringUtils.h"

namespace utils
{
	std::string DecodeHtmlEntity(const std::string& entity)
	{
		if (entity == "amp") return "&";
		if (entity == "lt") return "<";
		if (entity == "gt") return ">";
		if (entity == "quot") return "\"";
		if (entity == "nbsp") return " ";
		if (entity == "#39") return "'";
		return "&" + entity + ";";
	}

	bool ParseUnsigned(const std::string& token, unsigned int& value)
	{
		if (token.empty())
			return false;

		for (auto c : token)
		{
			if (!std::isdigit(static_cast<unsigned char>(c)))
				return false;
		}

		try
		{
			size_t consumed = 0;
			const auto parsed = std::stoull(token, &consumed, 10);
			if (consumed != token.size())
				return false;
			if (parsed > std::numeric_limits<unsigned int>::max())
				return false;

			value = static_cast<unsigned int>(parsed);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
}

LPCWSTR utils::CharsToUnicodeString(const char* str)
{
	size_t size = strlen(str) + 1;
	wchar_t* wideStr = new wchar_t[size];

	size_t outSize;
	mbstowcs_s(&outSize, wideStr, size, str, size - 1);

	return wideStr;
}

std::wstring utils::CharsToWideString(const char* str)
{
	size_t size = strlen(str) + 1;
	wchar_t* wideStr = new wchar_t[size];

	size_t outSize;
	mbstowcs_s(&outSize, wideStr, size, str, size - 1);

	return std::wstring(wideStr);
}

std::string utils::EncodeUtf8(const std::wstring& wStr)
{
	if (wStr.empty())
		return std::string();

	int numChars = WideCharToMultiByte(CP_UTF8,
		0, &wStr[0],
		(int)wStr.size(),
		NULL, 0,
		NULL, NULL);

	std::string str(numChars, 0);
	WideCharToMultiByte(CP_UTF8,
		0, &wStr[0],
		(int)wStr.size(),
		&str[0], numChars,
		NULL, NULL);

	return str;
}

std::wstring utils::DecodeUtf8(const std::string& str)
{
	if (str.empty()) return std::wstring();
	int numChars = MultiByteToWideChar(CP_UTF8,
		0, &str[0],
		(int)str.size(),
		NULL, 0);

	std::wstring wStr(numChars, 0);
	MultiByteToWideChar(CP_UTF8,
		0, &str[0],
		(int)str.size(),
		&wStr[0], numChars);

	return wStr;
}

std::string utils::GetGuid()
{
	std::string str = "";

	UUID uuid;
	RPC_STATUS ret_val = UuidCreate(&uuid);

	if (RPC_S_OK == ret_val)
	{
		char* buf = nullptr;
		UuidToStringA(&uuid, (RPC_CSTR*)&buf);

		if (buf)
		{
			str = std::string(buf);
			RpcStringFreeA((RPC_CSTR*)&buf);
		}
	}

	return str;
}

std::string utils::Trim(std::string str)
{
	auto isSpace = [](unsigned char c) { return 0 != std::isspace(c); };
	while (!str.empty() && isSpace(static_cast<unsigned char>(str.front())))
		str.erase(str.begin());
	while (!str.empty() && isSpace(static_cast<unsigned char>(str.back())))
		str.pop_back();
	return str;
}

std::string utils::CollapseWhitespace(std::string str)
{
	std::string out;
	out.reserve(str.size());
	bool lastWasSpace = false;
	for (const auto ch : str)
	{
		if (0 != std::isspace(static_cast<unsigned char>(ch)))
		{
			if (!lastWasSpace)
				out.push_back(' ');
			lastWasSpace = true;
		}
		else
		{
			out.push_back(ch);
			lastWasSpace = false;
		}
	}
	return Trim(std::move(out));
}

std::string utils::ToLower(std::string str)
{
	std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return str;
}

std::string utils::HtmlToText(const std::string& html, bool preserveLineBreaks)
{
	std::string out;
	out.reserve(html.size());

	for (std::size_t i = 0; i < html.size(); ++i)
	{
		if (html[i] == '<')
		{
			const auto close = html.find('>', i + 1);
			if (close == std::string::npos)
				break;

			auto tag = ToLower(html.substr(i + 1, close - i - 1));
			tag = Trim(std::move(tag));
			const auto spacePos = tag.find(' ');
			if (spacePos != std::string::npos)
				tag.erase(spacePos);

			if (tag == "br" || tag == "br/" || tag == "ul" || tag == "/ul"
				|| tag == "li" || tag == "/li" || tag == "p" || tag == "/p")
			{
				out.push_back(preserveLineBreaks ? '\n' : ' ');
			}

			i = close;
			continue;
		}

		if (html[i] == '&')
		{
			const auto semi = html.find(';', i + 1);
			if (semi != std::string::npos)
			{
				out += DecodeHtmlEntity(html.substr(i + 1, semi - i - 1));
				i = semi;
				continue;
			}
		}

		out.push_back(html[i]);
	}

	return out;
}

std::vector<std::string> utils::SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	std::stringstream ss(text);
	std::string line;
	while (std::getline(ss, line))
		lines.push_back(line);
	return lines;
}

std::string utils::Base64Encode(const std::vector<std::uint8_t>& data)
{
	static const char kBase64Chars[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	if (data.empty())
		return {};

	std::string out;
	out.reserve(((data.size() + 2) / 3) * 4);

	const std::uint8_t* p = data.data();
	size_t remaining = data.size();

	while (remaining >= 3)
	{
		const std::uint32_t v = (static_cast<std::uint32_t>(p[0]) << 16) |
									(static_cast<std::uint32_t>(p[1]) <<  8) |
									 static_cast<std::uint32_t>(p[2]);
		out += kBase64Chars[(v >> 18) & 0x3F];
		out += kBase64Chars[(v >> 12) & 0x3F];
		out += kBase64Chars[(v >>  6) & 0x3F];
		out += kBase64Chars[ v        & 0x3F];
		p         += 3;
		remaining -= 3;
	}

	if (remaining == 2)
	{
		const std::uint32_t v = (static_cast<std::uint32_t>(p[0]) << 16) |
									(static_cast<std::uint32_t>(p[1]) <<  8);
		out += kBase64Chars[(v >> 18) & 0x3F];
		out += kBase64Chars[(v >> 12) & 0x3F];
		out += kBase64Chars[(v >>  6) & 0x3F];
		out += '=';
	}
	else if (remaining == 1)
	{
		const std::uint32_t v = static_cast<std::uint32_t>(p[0]) << 16;
		out += kBase64Chars[(v >> 18) & 0x3F];
		out += kBase64Chars[(v >> 12) & 0x3F];
		out += '=';
		out += '=';
	}

	return out;
}

std::vector<std::uint8_t> utils::Base64Decode(const std::string& str)
{
	static const char kBase64Chars[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	if (str.empty())
		return {};
	if (str.size() % 4 != 0)
		return {};

	static const int kInvalid = -1;
	int revTable[256];
	std::fill(std::begin(revTable), std::end(revTable), kInvalid);
	for (int i = 0; i < 64; ++i)
		revTable[static_cast<unsigned char>(kBase64Chars[i])] = i;

	std::vector<std::uint8_t> out;
	out.reserve((str.size() / 4) * 3);

	size_t i = 0;
	while (i + 3 < str.size())
	{
		const int b0 = revTable[static_cast<unsigned char>(str[i])];
		const int b1 = revTable[static_cast<unsigned char>(str[i + 1])];
		const int b2 = revTable[static_cast<unsigned char>(str[i + 2])];
		const int b3 = revTable[static_cast<unsigned char>(str[i + 3])];
		const bool pad2 = str[i + 2] == '=';
		const bool pad3 = str[i + 3] == '=';

		if (b0 < 0 || b1 < 0)
			return {};
		if (pad2 && !pad3)
			return {};
		if ((pad2 || pad3) && i + 4 != str.size())
			return {};

		out.push_back(static_cast<std::uint8_t>((b0 << 2) | (b1 >> 4)));

		if (!pad2)
		{
			if (b2 < 0)
				return {};
			out.push_back(static_cast<std::uint8_t>(((b1 & 0x0F) << 4) | (b2 >> 2)));
		}

		if (!pad3)
		{
			if (b3 < 0)
				return {};
			out.push_back(static_cast<std::uint8_t>(((b2 & 0x03) << 6) | b3));
		}

		i += 4;
	}

	return out;
}

bool utils::StringReplace(std::string& str, const std::string& from, const std::string& to)
{
	size_t start_pos = str.find(from);
	if (start_pos == std::string::npos)
		return false;
	str.replace(start_pos, from.length(), to);
	return true;
}

bool utils::StringReplace(std::wstring& str, const std::wstring& from, const std::wstring& to)
{
	size_t start_pos = str.find(from);
	if (start_pos == std::wstring::npos)
		return false;
	str.replace(start_pos, from.length(), to);
	return true;
}