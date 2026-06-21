///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>
#include <Rpc.h>
#include <xlocbuf>
#include <codecvt>

#pragma comment(lib,"Rpcrt4.lib")

namespace utils
{
	LPCWSTR CharsToUnicodeString(const char* str);
	std::wstring CharsToWideString(const char* str);

	std::string EncodeUtf8(const std::wstring& wStr);
	std::wstring DecodeUtf8(const std::string& str);

	std::string GetGuid();

	std::string Trim(std::string str);
	std::string CollapseWhitespace(std::string str);
	std::string ToLower(std::string str);
	bool ParseUnsigned(const std::string& token, unsigned int& value);
	std::string HtmlToText(const std::string& html, bool preserveLineBreaks = false);
	std::vector<std::string> SplitLines(const std::string& text);
	// Base64 encode a raw byte blob to a standard ASCII string.
	std::string Base64Encode(const std::vector<std::uint8_t>& data);
	// Decode a base64 string back to raw bytes. Returns empty on empty or malformed input.
	std::vector<std::uint8_t> Base64Decode(const std::string& str);

	bool StringReplace(std::string& str, const std::string& from, const std::string& to);
	bool StringReplace(std::wstring& str, const std::wstring& from, const std::wstring& to);
}
