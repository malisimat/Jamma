#pragma once

#include "RigFile.h"
#include "../midi/MidiEvent.h"
#include "../midi/MidiDevice.h"
#include <cstdint>
#include <memory>
#include <algorithm>

namespace io
{
    // Complete off-callback values; Scene's existing serialization joins UI and input owners.
    class TriggerBindingLearn
    {
    public:
        struct Device { RigFile::BindingSource Source; std::string Id, Display; bool Available = false; };
        static std::vector<Device> Devices(const UserConfig& config, const midi::MidiConnectionResult& midi,
            const std::vector<std::string>& serial)
        {
            std::vector<Device> result;
            for (const auto& device : config.Midi.Devices)
            {
                auto display = device.Name;
                for (const auto& attempt : midi.Attempts)
                    if (attempt.RegistryId == device.Id && !attempt.ConnectedName.empty()) display = attempt.ConnectedName;
                result.push_back({RigFile::BindingSource::Midi,device.Id,std::move(display),
                    std::find(midi.ConnectedRegistryIds.begin(),midi.ConnectedRegistryIds.end(),device.Id) != midi.ConnectedRegistryIds.end()});
            }
            for (const auto& device : config.Serial.Devices)
                result.push_back({RigFile::BindingSource::Serial,device.Name,device.Port,
                    std::find(serial.begin(),serial.end(),device.Name) != serial.end()});
            return result;
        }
        struct Request
        {
            std::uint64_t Session = 0u, Attempt = 0u, Revision = 0u;
            bool operator==(const Request&) const = default;
        };
        struct Observation
        {
            Request Identity;
            bool Armed = false;
            std::optional<RigFile::Binding> Binding;
            std::optional<unsigned int> PhysicalChannel;
            std::string Error;
        };
        static std::optional<RigFile::Binding> Midi(const std::string& device, const midi::MidiEvent& event)
        {
            const auto type = event.MessageType();
            if (device.empty() || event.data1 > 127u || event.data2 > 127u ||
                (type != midi::MidiEvent::NoteOn && type != midi::MidiEvent::NoteOff && type != 0xb0u)) return {};
            RigFile::Binding binding;
            binding.Source = RigFile::BindingSource::Midi; binding.Device = device;
            binding.Press.Code = event.data1; binding.Press.Channel = event.Channel();
            binding.Press.Kind = type == 0xb0u ? RigFile::CC : RigFile::NOTE;
            binding.Press.State = type == 0xb0u ? (event.data2 != 0u) : event.IsNoteOn();
            binding.Release = binding.Press; binding.Release.State ^= 1u;
            return binding;
        }
        static RigFile::Binding Serial(const std::string& device, unsigned int button, bool down)
        {
            RigFile::Binding binding; binding.Source = RigFile::BindingSource::Serial; binding.Device = device;
            binding.Press.Code = button; binding.Press.State = down;
            binding.Release = binding.Press; binding.Release.State ^= 1u; return binding;
        }
        static bool KeyboardAllowed(unsigned int key)
        {
            // Both edges must reach ordinary trigger matching in normal performance.
            // Reserve keys owned by camera/editor/channel/group/automation shortcuts too.
            if (key == 0u || key >= 256u || (key >= 112u && key <= 120u) ||
                (key >= 160u && key <= 165u)) return false;
            switch (key)
            {
            case 8: case 9: case 13: case 16: case 17: case 18: case 27: case 32:
            case 33: case 34: case 45: case 69: case 76: case 82: case 83: case 86:
            case 87: case 88: case 90: case 91: case 92: case 93: case 96: case 192:
            case 219: case 221: return false;
            default: return true;
            }
        }
        static RigFile::Binding Keyboard(unsigned int key)
        {
            RigFile::Binding binding; binding.Press.Code = key; binding.Press.State = 1u;
            binding.Release = binding.Press; binding.Release.State = 0u; return binding;
        }
        static std::optional<RigFile> ReplacePedal(const RigFile& original, const std::string& id,
            RigFile::Control control, const std::vector<RigFile::Binding>& bindings, std::string& error)
        {
            auto candidate = original;
            const auto target = std::find_if(candidate.Triggers.begin(), candidate.Triggers.end(),
                [&](const auto& trigger) { return trigger.Id == id; });
            if (target == candidate.Triggers.end()) { error = "Trigger no longer exists"; return {}; }
            (control == RigFile::Control::Activate ? target->ControlBindings.Activate : target->ControlBindings.Ditch) = bindings;
            const auto& activate = target->ControlBindings.Activate;
            const auto& ditch = target->ControlBindings.Ditch;
            for (const auto pedal : {RigFile::Control::Activate, RigFile::Control::Ditch})
            {
                const auto& rows = pedal == RigFile::Control::Activate ? activate : ditch;
                for (std::size_t i = 0u; i < rows.size(); ++i) for (std::size_t j = i + 1u; j < rows.size(); ++j)
                    if (RigFile::BindingsOverlap(rows[i], rows[j]))
                    { error = std::string(pedal == RigFile::Control::Activate ? "Activate" : "Ditch") + " rows " + std::to_string(i + 1u) + " and " + std::to_string(j + 1u) + " overlap"; return {}; }
            }
            for (std::size_t i = 0u; i < activate.size(); ++i) for (std::size_t j = 0u; j < ditch.size(); ++j)
                if (RigFile::BindingsOverlap(activate[i], ditch[j]))
                { error = "Activate row " + std::to_string(i + 1u) + " overlaps Ditch row " + std::to_string(j + 1u); return {}; }
            RigFile::Diagnostics diagnostics;
            const auto normalized = RigFile::Normalize(candidate, &diagnostics);
            if (!normalized) { error = diagnostics.empty() ? "Invalid bindings or compiled capacity exceeded" : diagnostics.front().Message; return {}; }
            // Normalize resolves overlaps by dropping whole rows. Editing must never silently lose one.
            for (std::size_t i = 0u; i < candidate.Triggers.size(); ++i)
                if (candidate.Triggers[i].ControlBindings != normalized->Triggers[i].ControlBindings)
                { error = "Bindings overlap: relearn or delete the conflicting row (check both pedals)"; return {}; }
            error.clear(); return candidate;
        }
    };
}
