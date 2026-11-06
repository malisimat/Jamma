#include "PerformanceGroupPersistence.h"
#include "NativeMidiSidecar.h"
#include <set>
#include "../utils/StringUtils.h"

using io::PerformanceGroupPersistence;
using io::Json;

bool PerformanceGroupPersistence::ValidateIdentities(const JamFile& jam)
{
    std::set<std::string> stations, takes, audioLoops, midiLoops, plugins;
    const auto add = [](std::set<std::string>& ids, const std::string& id)
    {
        return id.empty() || (id.size() <= 256u && ids.insert(id).second);
    };
    for (const auto& station : jam.Stations)
    {
        if (!add(stations, station.Id)) return false;
        // Invalid/over-limit Station racks are preserved as inactive recovery
        // data. Validate hierarchy identity normally, but let Station perform
        // bounded rack admission rather than rejecting or erasing its raw IDs.
        bool inactiveRack = station.VstChain.size() > 32u;
        std::set<std::string> rackIds;
        for (const auto& plugin : station.VstChain)
            if (!plugin.Id.empty() && (!NativeMidiSidecar::ValidatePluginIdentity(plugin.Id) || !rackIds.insert(plugin.Id).second)) inactiveRack = true;
        if (!inactiveRack)
            for (const auto& plugin : station.VstChain)
                if (!plugin.Id.empty() && !plugins.insert(plugin.Id).second) return false;
        for (const auto& take : station.LoopTakes)
        {
            if (!add(takes, take.Id)) return false;
            for (const auto& plugin : take.VstChain) if (!add(plugins, plugin.Id)) return false;
            for (const auto& loop : take.Loops)
            {
                if (!add(audioLoops, loop.Id)) return false;
                for (const auto& plugin : loop.VstChain) if (!add(plugins, plugin.Id)) return false;
            }
            for (const auto& loop : take.MidiStreams) if (!add(midiLoops, loop.Id)) return false;
        }
    }
    return true;
}

const std::vector<Json::JsonPart>* PerformanceGroupPersistence::Records(const Json::JsonValue& value, bool& valid)
{
    valid = false;
    const auto array = std::get_if<Json::JsonArray>(&value);
    if (!array) return nullptr;
    if (const auto records = std::get_if<std::vector<Json::JsonPart>>(&array->Array))
    {
        valid = true;
        return records;
    }
    const auto empty = std::get_if<std::vector<bool>>(&array->Array);
    valid = empty && empty->empty();
    return nullptr;
}

bool PerformanceGroupPersistence::Boolean(const Json::JsonPart& value, const std::string& key, bool& out)
{
    const auto found = value.KeyValues.find(key);
    if (found == value.KeyValues.end()) return false;
    const auto boolean = std::get_if<bool>(&found->second);
    if (!boolean) return false;
    out = *boolean;
    return true;
}

bool PerformanceGroupPersistence::ReadTarget(const Json::JsonPart& value, engine::GroupTargetId& target)
{
    const auto kind = Json::GetUnsigned(value, "kind");
    const auto station = Json::GetString(value, "stationId");
    const auto take = Json::GetString(value, "takeId");
    const auto entity = Json::GetString(value, "entityId");
    if (!kind || *kind > 3u || !station || station->empty() || !take || !entity
        || station->size() > 256u || take->size() > 256u || entity->size() > 256u) return false;
    if ((*kind == 0u && (!take->empty() || !entity->empty()))
        || (*kind == 1u && (take->empty() || !entity->empty()))
        || (*kind >= 2u && (take->empty() || entity->empty()))) return false;
    target = { static_cast<engine::GroupTargetKind>(*kind), *station, *take, *entity };
    return true;
}

Json::JsonPart PerformanceGroupPersistence::WriteTarget(const engine::GroupTargetId& target)
{
    Json::JsonPart part;
    part.KeyValues["kind"] = static_cast<unsigned long>(target.Kind);
    part.KeyValues["stationId"] = target.StationId;
    part.KeyValues["takeId"] = target.TakeId;
    part.KeyValues["entityId"] = target.EntityId;
    return part;
}

std::string PerformanceGroupPersistence::TargetKey(const engine::GroupTargetId& target)
{
    // Length delimiters prevent IDs containing separators from colliding.
    return std::to_string(static_cast<unsigned int>(target.Kind)) + ":"
        + std::to_string(target.StationId.size()) + ":" + target.StationId
        + std::to_string(target.TakeId.size()) + ":" + target.TakeId
        + std::to_string(target.EntityId.size()) + ":" + target.EntityId;
}

bool PerformanceGroupPersistence::ValidBlob(const std::string& state, std::size_t& bytes)
{
    if (state.empty()) return true;
    if (state.size() % 4u != 0u || state.size() > (MaxBlobBytes - bytes) / 3u * 4u + 4u) return false;
    // Canonical re-encoding also rejects malformed padding/alphabet accepted
    // by permissive historical Base64Decode implementations.
    const auto decoded = utils::Base64Decode(state);
    if (decoded.empty() || decoded.size() > MaxBlobBytes - bytes || utils::Base64Encode(decoded) != state) return false;
    bytes += decoded.size();
    return true;
}

bool PerformanceGroupPersistence::Read(const Json::JsonPart& root, JamFile& jam)
{
    const auto found = root.KeyValues.find("performanceGroups");
    if (found == root.KeyValues.end()) return true;
    const auto groups = std::get_if<Json::JsonPart>(&found->second);
    if (!groups) return false;
    std::size_t targets = 0u, plugins = 0u, bytes = 0u;
    for (const auto mute : { true, false })
    {
        const std::string rowName = mute ? "mute" : "selection";
        const auto row = groups->KeyValues.find(rowName);
        if (row == groups->KeyValues.end()) continue;
        bool valid;
        const auto slots = Records(row->second, valid);
        if (!valid || (slots && slots->size() > 9u)) return false;
        std::set<unsigned int> seenSlots;
        if (slots) for (const auto& slot : *slots)
        {
            const auto number = Json::GetUnsigned(slot, "slot");
            const auto targetField = slot.KeyValues.find("targets");
            if (!number || *number < 1u || *number > 9u || !seenSlots.insert(*number).second
                || targetField == slot.KeyValues.end()) return false;
            const auto entries = Records(targetField->second, valid);
            if (!valid || (entries && entries->size() > MaxTargets - targets)) return false;
            engine::MuteGroupSnapshot muteSnapshot;
            engine::SelectionGroupSnapshot selectionSnapshot;
            if (!mute)
            {
                const auto depth = Json::GetUnsigned(slot, "depth");
                if (!depth || *depth > 3u) return false;
                selectionSnapshot.Depth = static_cast<engine::GroupTargetKind>(*depth);
            }
            std::set<std::string> seenTargets;
            if (entries) for (const auto& entry : *entries)
            {
                engine::GroupTargetId target;
                if (!ReadTarget(entry, target) || !seenTargets.insert(TargetKey(target)).second) return false;
                ++targets;
                if (mute)
                {
                    bool muted, effective;
                    if (!Boolean(entry, "muted", muted) || !Boolean(entry, "effectiveMuted", effective)) return false;
                    muteSnapshot.Targets.push_back({ std::move(target), muted, effective });
                }
                else
                {
                    bool selected;
                    if (!Boolean(entry, "selected", selected)) return false;
                    selectionSnapshot.Targets.push_back({ std::move(target), selected });
                }
            }
            if (mute)
            {
                const auto pluginField = slot.KeyValues.find("plugins");
                if (pluginField == slot.KeyValues.end()) return false;
                const auto pluginEntries = Records(pluginField->second, valid);
                if (!valid || (pluginEntries && pluginEntries->size() > MaxPlugins - plugins)) return false;
                std::set<std::string> seenPlugins;
                if (pluginEntries) for (const auto& entry : *pluginEntries)
                {
                    engine::GroupPluginState plugin;
                    const auto id = Json::GetString(entry, "pluginId");
                    const auto path = Json::GetString(entry, "path");
                    const auto state = Json::GetString(entry, "state");
                    if (!ReadTarget(entry, plugin.Owner) || static_cast<unsigned int>(plugin.Owner.Kind) > 1u
                        || !id || id->empty() || id->size() > 256u || !path || path->empty() || !state
                        || !Boolean(entry, "bypass", plugin.Bypass) || !ValidBlob(*state, bytes)
                        || !seenPlugins.insert(TargetKey(plugin.Owner) + ":" + *id).second) return false;
                    ++plugins;
                    plugin.PluginId = *id; plugin.Path = *path; plugin.State = *state;
                    muteSnapshot.Plugins.push_back(std::move(plugin));
                }
                jam.MuteGroups[*number - 1u] = std::move(muteSnapshot);
            }
            else jam.SelectionGroups[*number - 1u] = std::move(selectionSnapshot);
        }
        const auto active = groups->KeyValues.find(rowName + "Active");
        if (active != groups->KeyValues.end())
        {
            const auto number = Json::GetUnsigned(*groups, rowName + "Active");
            if (!number || !seenSlots.count(*number)) return false;
            (mute ? jam.MuteActiveGroup : jam.SelectionActiveGroup) = *number - 1u;
        }
    }
    return true;
}

bool PerformanceGroupPersistence::Write(const JamFile& jam, std::stringstream& stream)
{
    Json::JsonPart groups;
    for (const auto mute : { true, false })
    {
        std::vector<Json::JsonPart> slots;
        for (std::size_t i = 0u; i < 9u; ++i)
        {
            if (mute ? !jam.MuteGroups[i] : !jam.SelectionGroups[i]) continue;
            Json::JsonPart slot;
            slot.KeyValues["slot"] = static_cast<unsigned long>(i + 1u);
            std::vector<Json::JsonPart> targets;
            if (mute)
            {
                for (const auto& target : jam.MuteGroups[i]->Targets)
                {
                    auto value = WriteTarget(target.Target);
                    value.KeyValues["muted"] = target.Muted;
                    value.KeyValues["effectiveMuted"] = target.EffectiveMuted;
                    targets.push_back(std::move(value));
                }
                std::vector<Json::JsonPart> plugins;
                for (const auto& plugin : jam.MuteGroups[i]->Plugins)
                {
                    auto value = WriteTarget(plugin.Owner);
                    value.KeyValues["pluginId"] = plugin.PluginId;
                    value.KeyValues["path"] = plugin.Path;
                    value.KeyValues["state"] = plugin.State;
                    value.KeyValues["bypass"] = plugin.Bypass;
                    plugins.push_back(std::move(value));
                }
                slot.KeyValues["plugins"] = Json::JsonArray{ static_cast<unsigned int>(plugins.size()), std::move(plugins) };
            }
            else for (const auto& target : jam.SelectionGroups[i]->Targets)
            {
                auto value = WriteTarget(target.Target);
                value.KeyValues["selected"] = target.Selected;
                targets.push_back(std::move(value));
            }
            if (!mute) slot.KeyValues["depth"] = static_cast<unsigned long>(jam.SelectionGroups[i]->Depth);
            slot.KeyValues["targets"] = Json::JsonArray{ static_cast<unsigned int>(targets.size()), std::move(targets) };
            slots.push_back(std::move(slot));
        }
        const std::string name = mute ? "mute" : "selection";
        groups.KeyValues[name] = Json::JsonArray{ static_cast<unsigned int>(slots.size()), std::move(slots) };
        const auto active = mute ? jam.MuteActiveGroup : jam.SelectionActiveGroup;
        if (active)
        {
            if (*active >= 9u || (mute ? !jam.MuteGroups[*active] : !jam.SelectionGroups[*active])) return false;
            groups.KeyValues[name + "Active"] = static_cast<unsigned long>(*active + 1u);
        }
    }
    Json::JsonPart root;
    root.KeyValues["performanceGroups"] = groups;
    JamFile validated;
    if (!Read(root, validated)) return false;
    return Json::ToStream(groups, stream);
}
