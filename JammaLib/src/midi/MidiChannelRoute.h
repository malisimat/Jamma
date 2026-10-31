#pragma once

#include <string>
#include <vector>

namespace midi
{
	struct MidiChannelRoute
	{
		std::vector<std::string> PluginIds;
		bool operator==(const MidiChannelRoute&) const = default;
	};
}
