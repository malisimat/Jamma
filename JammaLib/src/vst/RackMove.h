#pragma once

#include "VstChain.h"
#include "../io/JamFile.h"
#include <algorithm>

namespace vst
{
	// Target is the final zero-based slot ordinal, after removing Source.
	// Owner identity includes its hierarchy scope; revisions belong to that owner.
	struct RackMove
	{
		enum class Scope { Station, Take, Loop };
		Scope OwnerScope = Scope::Station;
		std::string OwnerId;
		std::string PluginId;
		std::size_t Source = 0u, Target = 0u;
		std::uint64_t Revision = 0u;

		bool Valid(Scope scope, const std::string& owner, std::uint64_t revision,
			const std::vector<io::JamFile::VstEntry>& entries) const
		{
			return OwnerScope == scope && !OwnerId.empty() && OwnerId == owner
				&& Revision == revision && Source < entries.size() && Target < entries.size()
				&& Source != Target && !PluginId.empty() && entries[Source].Id == PluginId
				&& std::count_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.Id == PluginId; }) == 1;
		}

		// Caller serializes staging and metadata; audio topology is never modified.
		std::shared_ptr<VstChain> Stage(const std::shared_ptr<VstChain>& chain,
			std::vector<io::JamFile::VstEntry>& entries, std::vector<std::wstring>& paths) const
		{
			if (Source >= entries.size() || Target >= entries.size() || Source == Target || paths.size() != entries.size() || (chain && chain->NumPlugins() != entries.size())) return {};
			auto replacement = std::make_shared<VstChain>();
			for (std::size_t i = 0u; i < entries.size(); ++i)
			{
				auto original = i;
				if (i == Target) original = Source;
				else if (Source < Target && i >= Source && i < Target) original = i + 1u;
				else if (Target < Source && i > Target && i <= Source) original = i - 1u;
				replacement->AddSlot(chain ? chain->GetPlugin(original) : nullptr);
			}
			if (Source < Target)
			{
				std::rotate(entries.begin() + Source, entries.begin() + Source + 1u, entries.begin() + Target + 1u);
				std::rotate(paths.begin() + Source, paths.begin() + Source + 1u, paths.begin() + Target + 1u);
			}
			else
			{
				std::rotate(entries.begin() + Target, entries.begin() + Source, entries.begin() + Source + 1u);
				std::rotate(paths.begin() + Target, paths.begin() + Source, paths.begin() + Source + 1u);
			}
			return replacement;
		}
	};
}
