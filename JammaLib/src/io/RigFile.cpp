#include "RigFile.h"
#include "../utils/StringUtils.h"
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <unordered_set>
#include <type_traits>

using namespace io;

class RigFile::Reader
{
public:
	explicit Reader(Diagnostics* diagnostics) : Output(diagnostics) {}
	Diagnostics* Output;
	bool Valid = true;

	bool Error(const std::string& path, const std::string& message)
	{
		Valid = false;
		Add(Output, Severity::Error, path, message);
		return false;
	}
	static void Add(Diagnostics* output, Severity severity, const std::string& path, const std::string& message)
	{
		if (output) output->push_back({severity, path, message});
	}
	static bool ValidText(const std::string& value)
	{
		if (value.empty() || value.find('\0') != std::string::npos) return false;
		std::stringstream encoded;
		return Json::ToStream(value, encoded) && Json::FromStrictStream(std::move(encoded)).has_value();
	}
	bool Keys(const Json::JsonPart& part, const std::string& path, std::initializer_list<const char*> allowed)
	{
		for (const auto& [key, value] : part.KeyValues)
			if (std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return key == name; }))
				return Error(path + "." + key, "Unknown field");
		return true;
	}
	const Json::JsonValue* Field(const Json::JsonPart& part, const std::string& key, const std::string& path)
	{
		const auto found = part.KeyValues.find(key);
		if (found == part.KeyValues.end()) { Error(path + "." + key, "Required field missing"); return nullptr; }
		return &found->second;
	}
	const Json::JsonPart* Object(const Json::JsonValue* value, const std::string& path)
	{
		if (!value) return nullptr;
		const auto part = std::get_if<Json::JsonPart>(value);
		if (!part) Error(path, "Expected object");
		return part;
	}
	template<class T> bool Scalar(const Json::JsonPart& part, const char* key, const std::string& path, T& result)
	{
		const auto value = Field(part, key, path);
		if (!value) return false;
		if constexpr (std::is_same_v<T, unsigned int>)
		{
			const auto integer = std::get_if<unsigned long>(value);
			if (integer && *integer <= (std::numeric_limits<unsigned int>::max)()) { result = static_cast<unsigned int>(*integer); return true; }
			return Error(path + "." + key, "Expected unsigned integer");
		}
		else
		{
			const auto item = std::get_if<T>(value);
			if (!item) return Error(path + "." + key, std::is_same_v<T, bool> ? "Expected boolean" : "Expected string");
			result = *item;
			return true;
		}
	}
	template<class T> const std::vector<T>* Array(const Json::JsonValue* value, const std::string& path)
	{
		if (!value) return nullptr;
		const auto array = std::get_if<Json::JsonArray>(value);
		if (!array) { Error(path, "Expected array"); return nullptr; }
		static const std::vector<T> empty;
		if (array->Length == 0u) return &empty;
		const auto items = std::get_if<std::vector<T>>(&array->Array);
		if (!items) Error(path, "Unexpected array element type");
		return items;
	}
	bool Channel(const Json::JsonPart& part, const std::string& path, std::optional<unsigned int>& result)
	{
		const auto value = Field(part, "channel", path);
		if (!value) return false;
		if (std::holds_alternative<std::monostate>(*value)) { result.reset(); return true; }
		unsigned int channel;
		if (!Scalar(part, "channel", path, channel)) return false;
		if (channel < 1u || channel > 16u) return Error(path + ".channel", "Expected MIDI channel 1..16 or null");
		result = channel - 1u;
		return true;
	}
	bool Edge(const Json::JsonPart& part, BindingSource source, const std::string& path, EdgeMatcher& edge)
	{
		if (source != BindingSource::Midi)
			return Keys(part, path, {"code", "state"}) && Scalar(part, "code", path, edge.Code) && Scalar(part, "state", path, edge.State);
		std::string kind;
		if (!Keys(part, path, {"kind", "id", "state", "channel"}) || !Scalar(part, "kind", path, kind) ||
			!Scalar(part, "id", path, edge.Code) || !Scalar(part, "state", path, edge.State) || !Channel(part, path, edge.Channel)) return false;
		if (kind != "note" && kind != "cc") return Error(path + ".kind", "Expected note or cc");
		edge.Kind = kind == "note" ? NOTE : CC;
		return true;
	}
	bool ReadBinding(const Json::JsonPart& part, const std::string& path, Binding& binding)
	{
		std::string source;
		if (!Keys(part, path, {"source", "device", "button", "press", "release"}) ||
			!Scalar(part, "source", path, source) || !Scalar(part, "device", path, binding.Device)) return false;
		if (source == "keyboard") binding.Source = BindingSource::Keyboard;
		else if (source == "serial") binding.Source = BindingSource::Serial;
		else if (source == "midi") binding.Source = BindingSource::Midi;
		else return Error(path + ".source", "Expected keyboard, serial, or midi");
		const bool button = part.KeyValues.contains("button");
		const bool expanded = part.KeyValues.contains("press") || part.KeyValues.contains("release");
		if (button == expanded) return Error(path, "Specify either button or both press and release");
		if (expanded)
		{
			const auto press = Object(Field(part, "press", path), path + ".press");
			const auto release = Object(Field(part, "release", path), path + ".release");
			return press && release && Edge(*press, binding.Source, path + ".press", binding.Press) && Edge(*release, binding.Source, path + ".release", binding.Release);
		}
		binding.Press.State = 1u;
		if (binding.Source != BindingSource::Midi)
		{
			if (!Scalar(part, "button", path, binding.Press.Code)) return false;
		}
		else
		{
			const auto spec = Object(Field(part, "button", path), path + ".button");
			std::string kind;
			if (!spec || !Keys(*spec, path + ".button", {"kind", "id", "channel"}) ||
				!Scalar(*spec, "kind", path + ".button", kind) || !Scalar(*spec, "id", path + ".button", binding.Press.Code) ||
				!Channel(*spec, path + ".button", binding.Press.Channel)) return false;
			if (kind != "note" && kind != "noteoff" && kind != "cc") return Error(path + ".button.kind", "Expected note, noteoff, or cc");
			binding.Press.Kind = kind == "cc" ? CC : NOTE;
			binding.Press.State = kind == "noteoff" ? 0u : 1u;
		}
		binding.Release = binding.Press;
		binding.Release.State = 1u - binding.Press.State;
		return true;
	}
	bool ReadTrigger(const Json::JsonPart& part, const std::string& path, Trigger& trigger)
	{
		if (!Keys(part, path, {"id", "name", "target", "controls", "capture"}) ||
			!Scalar(part, "id", path, trigger.Id) || !Scalar(part, "name", path, trigger.Name)) return false;
		const auto target = Field(part, "target", path);
		if (!target) return false;
		if (const auto name = std::get_if<std::string>(target)) trigger.StationTarget = *name;
		else if (!std::holds_alternative<std::monostate>(*target)) return Error(path + ".target", "Expected station name or null");
		const auto controls = Object(Field(part, "controls", path), path + ".controls");
		const auto capture = Object(Field(part, "capture", path), path + ".capture");
		if (!controls || !capture || !Keys(*controls, path + ".controls", {"activate", "ditch"}) || !Keys(*capture, path + ".capture", {"audio", "midi"})) return false;
		if (!controls->KeyOrder.empty() && controls->KeyOrder.front() == "ditch") trigger.ControlBindings.First = Control::Ditch;
		for (const auto group : {Control::Activate, Control::Ditch})
		{
			const auto key = group == Control::Activate ? "activate" : "ditch";
			const auto bindings = Array<Json::JsonPart>(Field(*controls, key, path + ".controls"), path + ".controls." + key);
			if (!bindings) return false;
			auto& list = group == Control::Activate ? trigger.ControlBindings.Activate : trigger.ControlBindings.Ditch;
			for (size_t i = 0u; i < bindings->size(); ++i)
			{
				Binding binding;
				if (!ReadBinding((*bindings)[i], path + ".controls." + key + "[" + std::to_string(i) + "]", binding)) return false;
				list.push_back(std::move(binding));
			}
		}
		const auto audio = Array<unsigned long>(Field(*capture, "audio", path + ".capture"), path + ".capture.audio");
		const auto midi = Array<std::string>(Field(*capture, "midi", path + ".capture"), path + ".capture.midi");
		if (!audio || !midi) return false;
		for (size_t i = 0u; i < audio->size(); ++i)
		{
			if ((*audio)[i] > (std::numeric_limits<unsigned int>::max)()) return Error(path + ".capture.audio[" + std::to_string(i) + "]", "Audio channel out of range");
			trigger.CaptureInputs.Audio.push_back(static_cast<unsigned int>((*audio)[i]));
		}
		trigger.CaptureInputs.Midi = *midi;
		return true;
	}
	std::optional<RigFile> Read(const Json::JsonValue& value)
	{
		const auto root = Object(&value, "$" );
		if (!root || !Keys(*root, "$", {"version", "name", "devices", "settings", "triggers"})) return std::nullopt;
		RigFile rig{};
		if (!Scalar(*root, "version", "$", rig.Version) || !Scalar(*root, "name", "$", rig.Name)) return std::nullopt;
		if (rig.Version != CurrentVersion) { Error("$.version", "Unsupported rig version"); return std::nullopt; }
		const auto devices = Object(Field(*root, "devices", "$"), "$.devices");
		const auto settings = Object(Field(*root, "settings", "$"), "$.settings");
		if (!devices || !settings || !Keys(*devices, "$.devices", {"audio", "midi", "serial"}) || !Keys(*settings, "$.settings", {"loop", "trigger", "midi"})) return std::nullopt;
		const auto audio = Object(Field(*devices, "audio", "$.devices"), "$.devices.audio");
		const auto loop = Object(Field(*settings, "loop", "$.settings"), "$.settings.loop");
		const auto trigger = Object(Field(*settings, "trigger", "$.settings"), "$.settings.trigger");
		const auto midiSettings = Object(Field(*settings, "midi", "$.settings"), "$.settings.midi");
		if (!audio || !loop || !trigger || !midiSettings) return std::nullopt;
		if (!Keys(*audio, "$.devices.audio", {"name", "samplerate", "bufsize", "inlatency", "outlatency", "numbuffers", "numchannelsin", "numchannelsout"})) return std::nullopt;
		auto& a = rig.User.Audio;
		if (!Scalar(*audio, "name", "$.devices.audio", a.Name) || !Scalar(*audio, "samplerate", "$.devices.audio", a.SampleRate) ||
			!Scalar(*audio, "bufsize", "$.devices.audio", a.BufSize) || !Scalar(*audio, "inlatency", "$.devices.audio", a.LatencyIn) ||
			!Scalar(*audio, "outlatency", "$.devices.audio", a.LatencyOut) || !Scalar(*audio, "numbuffers", "$.devices.audio", a.NumBuffers) ||
			!Scalar(*audio, "numchannelsin", "$.devices.audio", a.NumChannelsIn) || !Scalar(*audio, "numchannelsout", "$.devices.audio", a.NumChannelsOut)) return std::nullopt;
		if (!Keys(*loop, "$.settings.loop", {"fadeSamps", "seedGrainMinMs", "seedGrainTargetMaxMs", "seedBpmMin", "seedQuantisation"})) return std::nullopt;
		auto& l = rig.User.Loop;
		std::string quantisation;
		if (!Scalar(*loop, "fadeSamps", "$.settings.loop", l.FadeSamps) || !Scalar(*loop, "seedGrainMinMs", "$.settings.loop", l.SeedGrainMinMs) ||
			!Scalar(*loop, "seedGrainTargetMaxMs", "$.settings.loop", l.SeedGrainTargetMaxMs) || !Scalar(*loop, "seedBpmMin", "$.settings.loop", l.SeedBpmMin) ||
			!Scalar(*loop, "seedQuantisation", "$.settings.loop", quantisation)) return std::nullopt;
		if (quantisation != "power" && quantisation != "multiple") { Error("$.settings.loop.seedQuantisation", "Expected power or multiple"); return std::nullopt; }
		l.SeedUsesPowers = quantisation == "power";
		if (!Keys(*trigger, "$.settings.trigger", {"preDelaySamps", "debounceMs"}) || !Scalar(*trigger, "preDelaySamps", "$.settings.trigger", rig.User.Trigger.PreDelaySamps) ||
			!Scalar(*trigger, "debounceMs", "$.settings.trigger", rig.User.Trigger.DebounceMs)) return std::nullopt;
		if (!Keys(*midiSettings, "$.settings.midi", {"channelOverrideTriggers", "channelOverrideLive"}) ||
			!Scalar(*midiSettings, "channelOverrideTriggers", "$.settings.midi", rig.User.Midi.ChannelOverrideTriggers) ||
			!Scalar(*midiSettings, "channelOverrideLive", "$.settings.midi", rig.User.Midi.ChannelOverrideLive)) return std::nullopt;
		const auto midi = Array<Json::JsonPart>(Field(*devices, "midi", "$.devices"), "$.devices.midi");
		const auto serial = Array<Json::JsonPart>(Field(*devices, "serial", "$.devices"), "$.devices.serial");
		const auto triggers = Array<Json::JsonPart>(Field(*root, "triggers", "$"), "$.triggers");
		if (!midi || !serial || !triggers) return std::nullopt;
		for (size_t i = 0u; i < midi->size(); ++i)
		{
			const auto path = "$.devices.midi[" + std::to_string(i) + "]";
			UserConfig::MidiSettings device;
			if (!Keys((*midi)[i], path, {"id", "name", "enabled"}) || !Scalar((*midi)[i], "id", path, device.Id) ||
				!Scalar((*midi)[i], "name", path, device.Name) || !Scalar((*midi)[i], "enabled", path, device.Enabled)) return std::nullopt;
			rig.User.Midi.Devices.push_back(std::move(device));
		}
		for (size_t i = 0u; i < serial->size(); ++i)
		{
			const auto path = "$.devices.serial[" + std::to_string(i) + "]";
			UserConfig::SerialSettings device;
			if (!Keys((*serial)[i], path, {"id", "port", "baudrate", "enabled"}) || !Scalar((*serial)[i], "id", path, device.Name) ||
				!Scalar((*serial)[i], "port", path, device.Port) || !Scalar((*serial)[i], "baudrate", path, device.BaudRate) ||
				!Scalar((*serial)[i], "enabled", path, device.Enabled)) return std::nullopt;
			rig.User.Serial.Devices.push_back(std::move(device));
		}
		for (size_t i = 0u; i < triggers->size(); ++i)
		{
			Trigger entry;
			if (!ReadTrigger((*triggers)[i], "$.triggers[" + std::to_string(i) + "]", entry)) return std::nullopt;
			rig.Triggers.push_back(std::move(entry));
		}
		return Normalize(rig, Output);
	}
};

std::optional<RigFile> RigFile::FromStream(std::stringstream ss, Diagnostics* diagnostics)
{
	const auto value = Json::FromStrictStream(std::move(ss));
	if (!value) { Reader::Add(diagnostics, Severity::Error, "$", "Malformed or unsupported JSON structure"); return std::nullopt; }
	return Reader(diagnostics).Read(*value);
}

bool RigFile::EdgesOverlap(BindingSource source, const EdgeMatcher& lhs, const EdgeMatcher& rhs) noexcept
{
	return lhs.Code == rhs.Code && lhs.State == rhs.State &&
		(source != BindingSource::Midi || (lhs.Kind == rhs.Kind && (!lhs.Channel || !rhs.Channel || lhs.Channel == rhs.Channel)));
}

bool RigFile::BindingsOverlap(const Binding& lhs, const Binding& rhs) noexcept
{
	return lhs.Source == rhs.Source && lhs.Device == rhs.Device &&
		(EdgesOverlap(lhs.Source, lhs.Press, rhs.Press) || EdgesOverlap(lhs.Source, lhs.Press, rhs.Release) ||
		 EdgesOverlap(lhs.Source, lhs.Release, rhs.Press) || EdgesOverlap(lhs.Source, lhs.Release, rhs.Release));
}

std::optional<RigFile> RigFile::Normalize(const RigFile& rig, Diagnostics* diagnostics)
{
	Reader check(diagnostics);
	auto require = [&](bool condition, const std::string& path, const std::string& message) { return condition || check.Error(path, message); };
	if (!require(rig.Version == CurrentVersion, "$.version", "Unsupported rig version") ||
		!require(Reader::ValidText(rig.Name), "$.name", "Name must be nonempty UTF-8 without NUL") ||
		!require(rig.Triggers.size() <= MaxTriggers, "$.triggers", "Trigger capacity exceeded")) return std::nullopt;
	const auto& audio = rig.User.Audio;
	if (!require(Reader::ValidText(audio.Name), "$.devices.audio.name", "Hardware selector must be nonempty UTF-8 without NUL") ||
		!require(audio.SampleRate > 0u, "$.devices.audio.samplerate", "Sample rate must be positive") ||
		!require(audio.BufSize > 0u, "$.devices.audio.bufsize", "Buffer size must be positive") ||
		!require(audio.NumBuffers > 0u, "$.devices.audio.numbuffers", "Buffer count must be positive") ||
		!require(audio.NumChannelsIn > 0u || audio.NumChannelsOut > 0u, "$.devices.audio", "At least one input or output channel is required") ||
		!require(audio.NumChannelsIn <= JamFile::MaxAudioRouteChannels, "$.devices.audio.numchannelsin", "Input channel capacity exceeded") ||
		!require(audio.NumChannelsOut <= JamFile::MaxAudioRouteChannels, "$.devices.audio.numchannelsout", "Output channel capacity exceeded")) return std::nullopt;
	const auto& loop = rig.User.Loop;
	if (!require(loop.SeedGrainMinMs > 0u, "$.settings.loop.seedGrainMinMs", "Minimum grain must be positive") ||
		!require(loop.SeedGrainTargetMaxMs >= loop.SeedGrainMinMs, "$.settings.loop.seedGrainTargetMaxMs", "Maximum grain must be at least minimum grain") ||
		!require(loop.SeedBpmMin > 0u, "$.settings.loop.seedBpmMin", "Minimum BPM must be positive") ||
		!require(rig.User.Trigger.PreDelaySamps <= (std::numeric_limits<unsigned int>::max)() - constants::MaxLoopFadeSamps,
			"$.settings.trigger.preDelaySamps", "Alignment sample count overflows")) return std::nullopt;
	std::unordered_set<std::string> registry{KeyboardDevice};
	std::unordered_set<std::string> midiIds;
	std::unordered_set<std::string> serialIds;
	for (size_t i = 0u; i < rig.User.Midi.Devices.size(); ++i)
	{
		const auto& device = rig.User.Midi.Devices[i];
		const auto path = "$.devices.midi[" + std::to_string(i) + "]";
		if (!require(Reader::ValidText(device.Id), path + ".id", "Registry ID must be nonempty UTF-8 without NUL") ||
			!require(registry.insert(device.Id).second, path + ".id", "Registry ID must be unique") ||
			!require(Reader::ValidText(device.Name), path + ".name", "Hardware selector must be nonempty UTF-8 without NUL")) return std::nullopt;
		midiIds.insert(device.Id);
	}
	for (size_t i = 0u; i < rig.User.Serial.Devices.size(); ++i)
	{
		const auto& device = rig.User.Serial.Devices[i];
		const auto path = "$.devices.serial[" + std::to_string(i) + "]";
		if (!require(Reader::ValidText(device.Name), path + ".id", "Registry ID must be nonempty UTF-8 without NUL") ||
			!require(registry.insert(device.Name).second, path + ".id", "Registry ID must be unique") ||
			!require(Reader::ValidText(device.Port), path + ".port", "Port must be nonempty UTF-8 without NUL") ||
			!require(device.BaudRate > 0u, path + ".baudrate", "Baud rate must be positive")) return std::nullopt;
		serialIds.insert(device.Name);
	}
	RigFile candidate = rig;
	std::unordered_set<std::string> triggerIds;
	std::unordered_set<std::string> triggerNames;
	for (size_t i = 0u; i < candidate.Triggers.size(); ++i)
	{
		auto& trigger = candidate.Triggers[i];
		const auto path = "$.triggers[" + std::to_string(i) + "]";
		if (!require(Reader::ValidText(trigger.Id), path + ".id", "Trigger ID must be nonempty UTF-8 without NUL") ||
			!require(triggerIds.insert(trigger.Id).second, path + ".id", "Trigger ID must be unique") ||
			!require(Reader::ValidText(trigger.Name), path + ".name", "Trigger name must be nonempty UTF-8 without NUL") ||
			!require(triggerNames.insert(trigger.Name).second, path + ".name", "Trigger name must be unique") ||
			!require(!trigger.StationTarget || Reader::ValidText(*trigger.StationTarget), path + ".target", "Use a nonempty UTF-8 station name without NUL, or null") ||
			!require(trigger.ControlBindings.First == Control::Activate || trigger.ControlBindings.First == Control::Ditch,
				path + ".controls", "Invalid first control group")) return std::nullopt;
		std::vector<Binding> prior;
		const auto first = trigger.ControlBindings.First;
		for (const auto group : {first, first == Control::Activate ? Control::Ditch : Control::Activate})
		{
			auto& bindings = group == Control::Activate ? trigger.ControlBindings.Activate : trigger.ControlBindings.Ditch;
			const auto groupPath = path + (group == Control::Activate ? ".controls.activate" : ".controls.ditch");
			std::vector<Binding> retained;
			size_t compiledCount = 0u;
			for (size_t b = 0u; b < bindings.size(); ++b)
			{
				const auto& binding = bindings[b];
				const auto bindingPath = groupPath + "[" + std::to_string(b) + "]";
				bool reference = false;
				switch (binding.Source)
				{
				case BindingSource::Keyboard: reference = binding.Device == KeyboardDevice; break;
				case BindingSource::Midi: reference = midiIds.contains(binding.Device); break;
				case BindingSource::Serial: reference = serialIds.contains(binding.Device); break;
				default: check.Error(bindingPath + ".source", "Invalid source kind"); return std::nullopt;
				}
				if (!require(reference, bindingPath + ".device", "Device reference missing or has the wrong source kind")) return std::nullopt;
				for (const auto press : {true, false})
				{
					const auto& edge = press ? binding.Press : binding.Release;
					const auto edgePath = bindingPath + (press ? ".press" : ".release");
					if (!require(edge.State <= 1u, edgePath + ".state", "State must be 0 or 1")) return std::nullopt;
					if (binding.Source == BindingSource::Midi)
					{
						if (!require(edge.Code <= 127u, edgePath + ".id", "MIDI ID must be 0..127") ||
							!require(edge.Kind == NOTE || edge.Kind == CC, edgePath + ".kind", "Invalid MIDI kind") ||
							!require(!edge.Channel || *edge.Channel < 16u, edgePath + ".channel", "MIDI channel out of range")) return std::nullopt;
					}
					else if (!require(!edge.Channel && edge.Kind == NOTE, edgePath, "Non-MIDI edge cannot select a MIDI kind or channel")) return std::nullopt;
				}
				if (!require(binding.Source != BindingSource::Midi || binding.Press.Channel == binding.Release.Channel,
					bindingPath + ".release.channel", "Press and release channel selections must match") ||
					!require(!EdgesOverlap(binding.Source, binding.Press, binding.Release), bindingPath + ".release", "Press and release must match distinct events")) return std::nullopt;
				const auto conflict = std::find_if(prior.begin(), prior.end(), [&](const Binding& existing) { return BindingsOverlap(existing, binding); });
				if (conflict != prior.end())
				{
					Reader::Add(diagnostics, Severity::Warning, bindingPath,
						*conflict == binding ? "Duplicate binding removed; earliest definition wins" : "Conflicting binding removed; earliest definition wins");
					continue;
				}
				compiledCount += binding.Source == BindingSource::Midi && !binding.Press.Channel ? 16u : 1u;
				if (!require(compiledCount <= MaxControlBindings, groupPath, "Compiled control binding capacity exceeded")) return std::nullopt;
				retained.push_back(binding);
				prior.push_back(binding);
			}
			bindings = std::move(retained);
		}
		std::vector<unsigned int> audioChannels;
		for (size_t c = 0u; c < trigger.CaptureInputs.Audio.size(); ++c)
		{
			const auto channel = trigger.CaptureInputs.Audio[c];
			const auto itemPath = path + ".capture.audio[" + std::to_string(c) + "]";
			if (!require(channel < JamFile::MaxAudioRouteChannels, itemPath, "Audio channel capacity exceeded")) return std::nullopt;
			if (std::find(audioChannels.begin(), audioChannels.end(), channel) != audioChannels.end())
				Reader::Add(diagnostics, Severity::Warning, itemPath, "Duplicate capture channel removed");
			else audioChannels.push_back(channel);
		}
		if (!require(audioChannels.size() <= JamFile::MaxAudioRouteChannels, path + ".capture.audio", "Capture channel capacity exceeded")) return std::nullopt;
		trigger.CaptureInputs.Audio = std::move(audioChannels);
		std::vector<std::string> midiDevices;
		for (size_t c = 0u; c < trigger.CaptureInputs.Midi.size(); ++c)
		{
			const auto& id = trigger.CaptureInputs.Midi[c];
			const auto itemPath = path + ".capture.midi[" + std::to_string(c) + "]";
			if (!require(midiIds.contains(id), itemPath, "MIDI registry reference missing or has the wrong source kind")) return std::nullopt;
			if (std::find(midiDevices.begin(), midiDevices.end(), id) != midiDevices.end())
				Reader::Add(diagnostics, Severity::Warning, itemPath, "Duplicate capture device removed");
			else midiDevices.push_back(id);
		}
		trigger.CaptureInputs.Midi = std::move(midiDevices);
	}
	return candidate;
}

bool RigFile::Validate(const RigFile& rig, Diagnostics* diagnostics)
{
	return Normalize(rig, diagnostics).has_value();
}

bool RigFile::ToJsonStream(const RigFile& rig, std::stringstream& ss, Diagnostics* diagnostics)
{
	const auto normalized = Normalize(rig, diagnostics);
	if (!normalized) return false;
	const auto& value = *normalized;
	const auto& user = value.User;
	using Part = Json::JsonPart;
	auto array = [](auto values) { return Json::JsonArray{static_cast<unsigned int>(values.size()), std::move(values)}; };
	auto integer = [](unsigned int number) { return static_cast<unsigned long>(number); };
	auto edge = [&](const EdgeMatcher& matcher, BindingSource source)
	{
		Part part;
		part.KeyValues["state"] = integer(matcher.State);
		if (source == BindingSource::Midi)
		{
			part.KeyValues["kind"] = std::string(matcher.Kind == NOTE ? "note" : "cc");
			part.KeyValues["id"] = integer(matcher.Code);
			part.KeyValues["channel"] = matcher.Channel ? Json::JsonValue(integer(*matcher.Channel + 1u)) : Json::JsonValue(std::monostate{});
		}
		else part.KeyValues["code"] = integer(matcher.Code);
		return part;
	};
	auto bindings = [&](const std::vector<Binding>& source)
	{
		std::vector<Part> parts;
		for (const auto& binding : source)
			parts.push_back(Part{{{"source", std::string(binding.Source == BindingSource::Keyboard ? "keyboard" : binding.Source == BindingSource::Serial ? "serial" : "midi")},
				{"device", binding.Device}, {"press", edge(binding.Press, binding.Source)}, {"release", edge(binding.Release, binding.Source)}}});
		return array(std::move(parts));
	};
	std::vector<Part> midi;
	for (const auto& device : user.Midi.Devices) midi.push_back(Part{{{"id", device.Id}, {"name", device.Name}, {"enabled", device.Enabled}}});
	std::vector<Part> serial;
	for (const auto& device : user.Serial.Devices) serial.push_back(Part{{{"id", device.Name}, {"port", device.Port}, {"baudrate", integer(device.BaudRate)}, {"enabled", device.Enabled}}});
	const auto& a = user.Audio;
	Part audio{{{"name", a.Name}, {"samplerate", integer(a.SampleRate)}, {"bufsize", integer(a.BufSize)}, {"inlatency", integer(a.LatencyIn)},
		{"outlatency", integer(a.LatencyOut)}, {"numbuffers", integer(a.NumBuffers)}, {"numchannelsin", integer(a.NumChannelsIn)}, {"numchannelsout", integer(a.NumChannelsOut)}}};
	const auto& l = user.Loop;
	Part loop{{{"fadeSamps", integer(l.FadeSamps)}, {"seedGrainMinMs", integer(l.SeedGrainMinMs)}, {"seedGrainTargetMaxMs", integer(l.SeedGrainTargetMaxMs)},
		{"seedBpmMin", integer(l.SeedBpmMin)}, {"seedQuantisation", std::string(l.SeedUsesPowers ? "power" : "multiple")}}};
	Part triggerSettings{{{"preDelaySamps", integer(user.Trigger.PreDelaySamps)}, {"debounceMs", integer(user.Trigger.DebounceMs)}}};
	Part midiSettings{{{"channelOverrideTriggers", user.Midi.ChannelOverrideTriggers}, {"channelOverrideLive", user.Midi.ChannelOverrideLive}}};
	Part devices{{{"audio", audio}, {"midi", array(std::move(midi))}, {"serial", array(std::move(serial))}}};
	Part settings{{{"loop", loop}, {"trigger", triggerSettings}, {"midi", midiSettings}}};
	std::vector<Part> triggers;
	for (const auto& trigger : value.Triggers)
	{
		Part controls{{{"activate", bindings(trigger.ControlBindings.Activate)}, {"ditch", bindings(trigger.ControlBindings.Ditch)}}};
		controls.KeyOrder = trigger.ControlBindings.First == Control::Activate ? std::vector<std::string>{"activate", "ditch"} : std::vector<std::string>{"ditch", "activate"};
		std::vector<unsigned long> channels(trigger.CaptureInputs.Audio.begin(), trigger.CaptureInputs.Audio.end());
		Part capture{{{"audio", array(std::move(channels))}, {"midi", array(trigger.CaptureInputs.Midi)}}};
		triggers.push_back(Part{{{"id", trigger.Id}, {"name", trigger.Name},
			{"target", trigger.StationTarget ? Json::JsonValue(*trigger.StationTarget) : Json::JsonValue(std::monostate{})}, {"controls", controls}, {"capture", capture}}});
	}
	Part root{{{"version", value.Version}, {"name", value.Name}, {"devices", devices}, {"settings", settings}, {"triggers", array(std::move(triggers))}}};
	return Json::ToStream(root, ss) && ss.good();
}

bool RigFile::ToStream(RigFile rig, std::stringstream& ss)
{
	ss << "Version: " << rig.Version << '\n' << "Name: " << rig.Name << '\n';
	ss << "Audio: " << rig.User.Audio.Name << " SampleRate: " << rig.User.Audio.SampleRate << " BufferSize: " << rig.User.Audio.BufSize << '\n';
	ss << "MIDI devices: " << rig.User.Midi.Devices.size() << " Serial devices: " << rig.User.Serial.Devices.size() << '\n';
	for (const auto& trigger : rig.Triggers)
		ss << "Trigger: " << trigger.Name << " ID: " << trigger.Id << " Target: " << trigger.StationTarget.value_or("unbound")
			<< " Activate bindings: " << trigger.ControlBindings.Activate.size() << " Ditch bindings: " << trigger.ControlBindings.Ditch.size()
			<< " Audio capture: " << trigger.CaptureInputs.Audio.size() << " MIDI capture: " << trigger.CaptureInputs.Midi.size() << '\n';
	return ss.good();
}

RigFileRouting::Resolution RigFileRouting::Resolve(const RigFile& rig,
	const std::vector<JamFile::Station>& stations, unsigned int availableAdcChannels,
	const std::vector<std::string>& availableMidiDevices,
	const std::vector<std::string>* availableSerialDevices)
{
	Resolution result{rig};
	const auto normalized = RigFile::Normalize(rig, &result.Diagnostics);
	if (!normalized) { result.IsValid = false; return result; }
	result.CandidateRig = *normalized;
	result.RequiresSave = result.CandidateRig.Triggers != rig.Triggers;
	auto opened = [&](const std::string& id) { return std::find(availableMidiDevices.begin(), availableMidiDevices.end(), id) != availableMidiDevices.end(); };
	auto warning = [&](const std::string& path, const std::string& message) { result.Diagnostics.push_back({RigFile::Severity::Warning, path, message}); };
	for (size_t i = 0u; i < result.CandidateRig.Triggers.size(); ++i)
	{
		const auto& trigger = result.CandidateRig.Triggers[i];
		const auto path = "$.triggers[" + std::to_string(i) + "]";
		TriggerResolution resolution;
		resolution.TriggerIndex = i;
		resolution.TriggerName = trigger.Name;
		resolution.TargetName = trigger.StationTarget;
		if (trigger.StationTarget)
		{
			size_t matches = 0u;
			for (size_t station = 0u; station < stations.size(); ++station)
				if (stations[station].Name == *trigger.StationTarget) { ++matches; resolution.StationIndex = station; }
			if (matches != 1u)
			{
				resolution.StationIndex.reset();
				resolution.Reason = matches == 0u ? Warning::TargetMissing : Warning::TargetAmbiguous;
				warning(path + ".target", matches == 0u ? "Target station is unavailable; connection retained" : "Target station name is ambiguous; connection retained");
			}
		}
		for (size_t channel = 0u; channel < trigger.CaptureInputs.Audio.size(); ++channel)
		{
			const auto index = trigger.CaptureInputs.Audio[channel];
			const bool available = index < availableAdcChannels;
			resolution.Sources.push_back({SourceKind::Adc, index, {}, available});
			if (!available) warning(path + ".capture.audio[" + std::to_string(channel) + "]", "Audio input is unavailable; selection retained");
		}
		for (size_t device = 0u; device < trigger.CaptureInputs.Midi.size(); ++device)
		{
			const auto& id = trigger.CaptureInputs.Midi[device];
			const bool available = opened(id);
			resolution.Sources.push_back({SourceKind::Midi, 0u, id, available});
			if (!available) warning(path + ".capture.midi[" + std::to_string(device) + "]", "MIDI input is unavailable; selection retained");
		}
		for (const auto group : {RigFile::Control::Activate, RigFile::Control::Ditch})
		{
			const auto& bindings = group == RigFile::Control::Activate ? trigger.ControlBindings.Activate : trigger.ControlBindings.Ditch;
			for (size_t bindingIndex = 0u; bindingIndex < bindings.size(); ++bindingIndex)
			{
				const auto& binding = bindings[bindingIndex];
				const auto found = std::find_if(resolution.ControlSources.begin(), resolution.ControlSources.end(), [&](const ControlSource& source) {
					return source.Kind == binding.Source && source.Device == binding.Device;
				});
				if (found != resolution.ControlSources.end()) continue;
				ControlSource source{binding.Source, binding.Device};
				if (binding.Source == RigFile::BindingSource::Keyboard) source.Available = true;
				else if (binding.Source == RigFile::BindingSource::Midi) source.Available = opened(binding.Device);
				else if (availableSerialDevices)
					source.Available = std::find(availableSerialDevices->begin(), availableSerialDevices->end(), binding.Device) != availableSerialDevices->end();
				// A null inventory means unknown, never inferred from configured/enumerated ports.
				if (source.Available && !*source.Available)
					warning(path + (group == RigFile::Control::Activate ? ".controls.activate[" : ".controls.ditch[") + std::to_string(bindingIndex) + "].device",
						"Control device is unavailable; binding retained");
				resolution.ControlSources.push_back(std::move(source));
			}
		}
		result.Triggers.push_back(std::move(resolution));
	}
	return result;
}

std::string RigFileRouting::NextTriggerName(const RigFile& rig)
{
	for (unsigned int suffix = 1u;; ++suffix)
	{
		const auto name = "Trigger-" + std::to_string(suffix);
		if (std::none_of(rig.Triggers.begin(), rig.Triggers.end(), [&](const RigFile::Trigger& trigger) { return trigger.Name == name; })) return name;
	}
}

RigFile RigFileRouting::WithUnboundTrigger(const RigFile& rig, RigFile::Diagnostics* diagnostics)
{
	if (rig.Triggers.size() >= RigFile::MaxTriggers)
	{
		if (diagnostics) diagnostics->push_back({RigFile::Severity::Error, "$.triggers", "Trigger capacity exceeded"});
		return rig;
	}
	auto candidate = rig;
	RigFile::Trigger trigger;
	for (unsigned int attempt = 0u; attempt < 4u; ++attempt)
	{
		trigger.Id = utils::GetGuid();
		if (!trigger.Id.empty() && std::none_of(rig.Triggers.begin(), rig.Triggers.end(),
			[&](const RigFile::Trigger& existing) { return existing.Id == trigger.Id; })) break;
		trigger.Id.clear();
	}
	if (trigger.Id.empty())
	{
		if (diagnostics) diagnostics->push_back({RigFile::Severity::Error, "$.triggers", "Could not allocate a stable trigger ID"});
		return rig;
	}
	trigger.Name = NextTriggerName(rig);
	candidate.Triggers.push_back(std::move(trigger));
	return RigFile::Normalize(candidate, diagnostics).value_or(rig);
}

std::optional<RigFile> RigFileRouting::WithoutTrigger(const RigFile& rig, size_t triggerIndex)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	candidate.Triggers.erase(candidate.Triggers.begin() + triggerIndex);
	return RigFile::Normalize(candidate);
}

std::optional<RigFile> RigFileRouting::WithStationTarget(const RigFile& rig, size_t triggerIndex, std::optional<std::string> target)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	candidate.Triggers[triggerIndex].StationTarget = std::move(target);
	return RigFile::Normalize(candidate);
}

std::optional<RigFile> RigFileRouting::WithAdcInput(const RigFile& rig, size_t triggerIndex, unsigned int channel)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	auto& channels = candidate.Triggers[triggerIndex].CaptureInputs.Audio;
	if (std::find(channels.begin(), channels.end(), channel) != channels.end()) return std::nullopt;
	channels.push_back(channel);
	return RigFile::Normalize(candidate);
}

std::optional<RigFile> RigFileRouting::WithoutAdcInput(const RigFile& rig, size_t triggerIndex, unsigned int channel)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	auto& channels = candidate.Triggers[triggerIndex].CaptureInputs.Audio;
	const auto found = std::find(channels.begin(), channels.end(), channel);
	if (found == channels.end()) return std::nullopt;
	channels.erase(found);
	return RigFile::Normalize(candidate);
}

std::optional<RigFile> RigFileRouting::WithMidiInput(const RigFile& rig, size_t triggerIndex, std::string device)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	auto& devices = candidate.Triggers[triggerIndex].CaptureInputs.Midi;
	if (std::find(devices.begin(), devices.end(), device) != devices.end()) return std::nullopt;
	devices.push_back(std::move(device));
	return RigFile::Normalize(candidate);
}

std::optional<RigFile> RigFileRouting::WithoutMidiInput(const RigFile& rig, size_t triggerIndex, const std::string& device)
{
	if (triggerIndex >= rig.Triggers.size()) return std::nullopt;
	auto candidate = rig;
	auto& devices = candidate.Triggers[triggerIndex].CaptureInputs.Midi;
	const auto found = std::find(devices.begin(), devices.end(), device);
	if (found == devices.end()) return std::nullopt;
	devices.erase(found);
	return RigFile::Normalize(candidate);
}

const std::string RigFile::DefaultJson = R"json({
  "version":"2.0",
  "name":"default",
  "devices":{
    "audio":{"name":"default","samplerate":44100,"bufsize":512,"inlatency":4600,"outlatency":6000,"numbuffers":4,"numchannelsin":2,"numchannelsout":2},
    "midi":[{"id":"midi-1","name":"default","enabled":true}],
    "serial":[]
  },
  "settings":{
    "loop":{"fadeSamps":800,"seedGrainMinMs":400,"seedGrainTargetMaxMs":3000,"seedBpmMin":80,"seedQuantisation":"power"},
    "trigger":{"preDelaySamps":400,"debounceMs":280},
    "midi":{"channelOverrideTriggers":false,"channelOverrideLive":true}
  },
  "triggers":[{
    "id":"default-trigger-1","name":"Trig1","target":"Station1",
    "controls":{
      "activate":[{"source":"keyboard","device":"keyboard","button":49}],
      "ditch":[{"source":"keyboard","device":"keyboard","button":50}]
    },
    "capture":{"audio":[0,1],"midi":[]}
  }]
})json";
