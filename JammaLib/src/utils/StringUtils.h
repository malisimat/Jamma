///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <string>
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

	bool StringReplace(std::string& str, const std::string& from, const std::string& to);
	bool StringReplace(std::wstring& str, const std::wstring& from, const std::wstring& to);
}
