#include "ResourcePaths.h"
#include "../utils/StringUtils.h"

#include <Windows.h>
#include <array>
#include <filesystem>
#include <system_error>

namespace resources
{
	std::string ResolveResourcePath(const std::string& relativePath)
	{
		// Resources are deployed beside the executable; shortcuts and file dialogs
		// must not redirect loading through the process working directory.
		std::array<wchar_t, 32768> executable{};
		const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
		if (!length || length >= executable.size())
			throw std::system_error(GetLastError(), std::system_category(), "Cannot locate executable resources");
		return utils::EncodeUtf8((std::filesystem::path(executable.data()).parent_path() /
			L"resources" / utils::DecodeUtf8(relativePath)).wstring());
	}

	std::string ResolveResourceListPath()
	{
		return ResolveResourcePath("ResourceList.txt");
	}

}
