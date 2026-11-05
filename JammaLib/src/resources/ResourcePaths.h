#pragma once

#include <string>

namespace resources
{
	// Absolute UTF-8 paths anchored at the running executable's resources folder.
	std::string ResolveResourcePath(const std::string& relativePath);
	std::string ResolveResourceListPath();

}
