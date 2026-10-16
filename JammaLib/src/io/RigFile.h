#pragma once

#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "Json.h"
#include "UserConfig.h"
#include "JamFile.h"

namespace io
{
	struct RigFile
	{
		static constexpr const char* CurrentVersion = "2.0";
		static constexpr const char* KeyboardDevice = "keyboard";
		static constexpr std::size_t MaxControlBindings = 64u;
		static constexpr std::size_t MaxTriggers = 256u;

		enum MidiTriggerEvent { NOTE, CC };
		enum class BindingSource { Keyboard, Serial, Midi };
		enum class Control { Activate, Ditch };

		struct EdgeMatcher
		{
			// For MIDI, Code is a note/controller ID and State is normalised 0/1.
			unsigned int Code = 0u;
			unsigned int State = 0u;
			MidiTriggerEvent Kind = NOTE;
			// MIDI channels are zero-based internally; null means any channel.
			std::optional<unsigned int> Channel;
			bool operator==(const EdgeMatcher&) const = default;
		};

		struct Binding
		{
			BindingSource Source = BindingSource::Keyboard;
			std::string Device = KeyboardDevice;
			EdgeMatcher Press;
			EdgeMatcher Release;
			bool operator==(const Binding&) const = default;
		};

		struct Controls
		{
			std::vector<Binding> Activate;
			std::vector<Binding> Ditch;
			// Preserve textual control-section order without a second editable
			// priority setting. Each list retains its own binding order.
			Control First = Control::Activate;
			bool operator==(const Controls&) const = default;
		};

		struct Capture
		{
			std::vector<unsigned int> Audio;
			std::vector<std::string> Midi;
			bool operator==(const Capture&) const = default;
		};

		struct Trigger
		{
			std::string Id;
			std::string Name;
			std::optional<std::string> StationTarget;
			Controls ControlBindings;
			Capture CaptureInputs;
			bool operator==(const Trigger&) const = default;
		};

		enum class Severity { Warning, Error };
		struct Diagnostic
		{
			Severity Level = Severity::Error;
			std::string Path;
			std::string Message;
		};
		using Diagnostics = std::vector<Diagnostic>;

		// Canonical-only reader. Diagnostics describe fields; absent hardware and
		// stations are resolved separately and are not configuration errors.
		static std::optional<RigFile> FromStream(std::stringstream ss, Diagnostics* diagnostics = nullptr);
		static bool ToStream(RigFile rig, std::stringstream& ss);
		static bool ToJsonStream(const RigFile& rig, std::stringstream& ss, Diagnostics* diagnostics = nullptr);
		static bool Validate(const RigFile& rig, Diagnostics* diagnostics = nullptr);
		// Off-thread normalisation removes whole later conflicting bindings so
		// no retained binding can lose one of its press/release edges.
		static std::optional<RigFile> Normalize(const RigFile& rig, Diagnostics* diagnostics = nullptr);
		static bool BindingsOverlap(const Binding& lhs, const Binding& rhs) noexcept;
		static const std::string DefaultJson;

		std::string Version = CurrentVersion;
		std::string Name;
		// One settings aggregate: serialises as devices + settings, not user.
		// Endpoint configuration remains solely in Audio/Midi/Serial.
		UserConfig User;
		std::vector<Trigger> Triggers;
		bool operator==(const RigFile&) const = default;

	private:
		class Reader;
		static bool EdgesOverlap(BindingSource source, const EdgeMatcher& lhs,
			const EdgeMatcher& rhs) noexcept;
	};

	struct RigFileRouting
	{
		enum class SourceKind { Adc, Midi };
		enum class Warning { None, TargetMissing, TargetAmbiguous };

		struct Source
		{
			SourceKind Kind;
			unsigned int AdcChannel = 0u;
			std::string MidiDevice;
			bool Available = false;
		};
		struct ControlSource
		{
			RigFile::BindingSource Kind;
			std::string Device;
			std::optional<bool> Available;
		};
		struct TriggerResolution
		{
			size_t TriggerIndex = 0u;
			std::string TriggerName;
			std::optional<std::string> TargetName;
			std::optional<size_t> StationIndex;
			std::vector<Source> Sources;
			std::vector<ControlSource> ControlSources;
			Warning Reason = Warning::None;
		};
		struct Resolution
		{
			RigFile CandidateRig;
			std::vector<TriggerResolution> Triggers;
			RigFile::Diagnostics Diagnostics;
			bool RequiresSave = false;
			bool IsValid = true;
		};
		// availableMidiDevices are opened registry IDs, never hardware selectors.
		static Resolution Resolve(const RigFile& rig,
			const std::vector<JamFile::Station>& stations,
			unsigned int availableAdcChannels,
			const std::vector<std::string>& availableMidiDevices,
			const std::vector<std::string>* availableSerialDevices = nullptr);
		static std::string NextTriggerName(const RigFile& rig);
		static RigFile WithUnboundTrigger(const RigFile& rig, RigFile::Diagnostics* diagnostics = nullptr);
		static std::optional<RigFile> WithoutTrigger(const RigFile& rig, size_t triggerIndex);
		static std::optional<RigFile> WithStationTarget(const RigFile& rig, size_t triggerIndex,
			std::optional<std::string> target);
		static std::optional<RigFile> WithAdcInput(const RigFile& rig, size_t triggerIndex, unsigned int channel);
		static std::optional<RigFile> WithoutAdcInput(const RigFile& rig, size_t triggerIndex, unsigned int channel);
		static std::optional<RigFile> WithMidiInput(const RigFile& rig, size_t triggerIndex, std::string device);
		static std::optional<RigFile> WithoutMidiInput(const RigFile& rig, size_t triggerIndex, const std::string& device);
	};
}
