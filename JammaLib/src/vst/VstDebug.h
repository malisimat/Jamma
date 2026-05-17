///////////////////////////////////////////////////////////
//
// Author 2026 GitHub Copilot
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <string>

namespace vst
{
	struct DebugOptions
	{
		bool AutoOpenEditor = false;
		bool LogToFile = false;
		std::wstring LogPath;
		unsigned int StationIndex = 0;
		unsigned int PluginIndex = 0;

		bool IsEnabled() const noexcept
		{
			return AutoOpenEditor || LogToFile;
		}
	};
}