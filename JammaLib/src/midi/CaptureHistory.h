#pragma once

#include <bitset>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "MidiEvent.h"
#include "MidiNote.h"

namespace engine { class LoopTake; }

namespace midi
{
	class MidiDelivery;

	// Physical input history and source-loss tracking. The serialized non-audio
	// owner feeds capture targets; held/pedal/history state keeps its existing
	// mutex boundary. Audio timing and capture admission remain with the caller.
	class CaptureHistory
	{
	public:
		struct RecentPhysicalMidi
		{
			midi::MidiEvent Event;
			std::string Device;
			std::uint8_t PriorDeviceVelocity = 0u;
			std::uint8_t PriorAnyVelocity = 0u;
			std::uint64_t Sequence = 0u;
		};
		struct Replay
		{
			std::vector<std::pair<std::string, MidiNoteSnapshot>> Held;
			std::vector<RecentPhysicalMidi> Events;
		};
		void Observe(const MidiEvent& event, const std::string& deviceName, float sampleRate, bool channelAllowed);
		void CompletePump(std::uint32_t deviceSample, bool overflow);
		std::optional<Replay> PrepareReplay(std::uint32_t earliest) const;
		void TrackTarget(const std::shared_ptr<engine::LoopTake>& take) { _midiCaptureTargets.push_back(take); }
		std::vector<MidiEvent> RemoveChannels(std::uint16_t removedMask);
		MidiNoteSnapshot ClearSources() noexcept;
		void LoseDevice(const std::string& deviceName) noexcept;
		void FlushPendingReleases(MidiDelivery& delivery) noexcept;
		void Reset();
		void ClearRecent();

	private:
		mutable std::mutex _liveHeldMidiMutex;
		std::vector<std::pair<std::string, midi::MidiNoteSnapshot>> _liveHeldMidi;
		std::vector<std::pair<std::string, std::bitset<16>>> _liveSustainMidi; // Same non-RT held-state mutex/owner.
		std::bitset<16> _pendingLostSustain; // Job retry, protected by held-state mutex.
		// Structural/MIDI job owner; guarded with the existing held-state mutex.
		std::vector<RecentPhysicalMidi> _recentPhysicalMidi;
		std::vector<std::weak_ptr<engine::LoopTake>> _midiCaptureTargets;
		std::optional<std::uint32_t> _midiHistoryDroppedThroughSample;
		std::uint64_t _physicalMidiSequence = 0u;
		midi::MidiNoteSnapshot _anonymousLiveHeldMidi;
		std::bitset<midi::MidiNote::TotalNoteSlots> _pendingLostLiveMidiNoteOffs;
	};
}
