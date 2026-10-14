#pragma once
#include "JamFile.h"

namespace io
{
    // Non-RT manifest codec. Bounded records prevent group blobs from amplifying
    // memory use beyond the ordinary JSON document allocation.
    class PerformanceGroupPersistence
    {
    public:
        static bool Read(const Json::JsonPart& root, JamFile& jam);
        static bool Write(const JamFile& jam, std::stringstream& stream);
        static bool ValidateIdentities(const JamFile& jam);
    private:
        static constexpr std::size_t MaxTargets = 32768u;
        static constexpr std::size_t MaxPlugins = 4096u;
        static constexpr std::size_t MaxBlobBytes = 64u * 1024u * 1024u;
        static bool ReadTarget(const Json::JsonPart& value, engine::GroupTargetId& target);
        static Json::JsonPart WriteTarget(const engine::GroupTargetId& target);
        static const std::vector<Json::JsonPart>* Records(const Json::JsonValue& value, bool& valid);
        static bool Boolean(const Json::JsonPart& value, const std::string& key, bool& out);
        static bool ValidBlob(const std::string& state, std::size_t& bytes);
        static std::string TargetKey(const engine::GroupTargetId& target);
    };
}
