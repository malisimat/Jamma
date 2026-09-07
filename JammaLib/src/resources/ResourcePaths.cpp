#include "ResourcePaths.h"

#include <vector>
#include <string>
#include <fstream>

namespace resources
{
	static bool FileExists(const std::string& path)
	{
		std::ifstream file(path.c_str(), std::ios::binary);
		return file.good();
	}

	static std::vector<std::string> BuildSearchRoots()
	{
		return {
			"resources/",
			"Jamma/resources/"
		};
	}

	std::string ResolveResourcePath(const std::string& relativePath)
	{
		if (FileExists(relativePath))
			return relativePath;

		const auto roots = BuildSearchRoots();
		for (const auto& root : roots)
		{
			const std::string candidate = root + relativePath;
			if (FileExists(candidate))
				return candidate;
		}

		return relativePath;
	}

	std::string ResolveResourceListPath()
	{
		return ResolveResourcePath("ResourceList.txt");
	}

}
