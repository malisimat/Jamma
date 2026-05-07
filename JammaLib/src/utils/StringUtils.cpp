///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include <windows.h>
#include "StringUtils.h"

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