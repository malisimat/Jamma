#pragma once

#include <string>

namespace resources
{
	std::string ResolveResourcePath(const std::string& relativePath);
	std::string ResolveResourceListPath();

}