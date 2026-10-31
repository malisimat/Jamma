#include "CaptureHistory.h"
#include <algorithm>
#include "MidiBlockTiming.h"
#include "MidiDelivery.h"
#include "../engine/LoopTake.h"

using namespace midi;

void CaptureHistory::Observe(const MidiEvent& event, const std::string& deviceName,
	float sampleRate, bool channelAllowed)
{
	if (event.MessageType() == 0xb0u && event.data1 == 64u)
	{
		if (event.data2 >= 64u && !channelAllowed) return;
		std::scoped_lock lock(_liveHeldMidiMutex);
		auto found = std::find_if(_liveSustainMidi.begin(), _liveSustainMidi.end(), [&](const auto& item) { return item.first == deviceName; });
		if (found == _liveSustainMidi.end())
		{
			_liveSustainMidi.push_back({deviceName, {}});
			found = std::prev(_liveSustainMidi.end());
		}
		found->second.set(event.Channel(), event.data2 >= 64u);
		return; // Preserve the existing note-only capture contract.
	}
	if (!event.IsNoteOn() && !event.IsNoteOff()) return;
	if (event.IsNoteOn() && !channelAllowed) return;
	{
		const auto channel = event.Channel();
		const auto note = static_cast<std::uint8_t>(event.data1 & 0x7Fu);
		const auto slot = midi::MidiNote::NoteSlot(channel, note);
		std::scoped_lock lock(_liveHeldMidiMutex);
		auto upsert = [&](const std::string& key) {
			auto it = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
				[&](const auto& item) { return item.first == key; });
			if (it == _liveHeldMidi.end())
			{
				_liveHeldMidi.push_back({key, MidiNoteSnapshot{}});
				it = std::prev(_liveHeldMidi.end());
			}
			const auto prior = it->second.Velocity[slot];
			if (event.IsNoteOn()) it->second.Set(channel, note, event.data2);
			else it->second.Clear(channel, note);
			return prior;
		};
		const auto priorAny = upsert("");
		const auto priorDevice = deviceName.empty() ? priorAny : upsert(deviceName);
		if (deviceName.empty())
		{
			if (event.IsNoteOn()) _anonymousLiveHeldMidi.Set(channel, note, event.data2);
			else _anonymousLiveHeldMidi.Clear(channel, note);
		}
		_recentPhysicalMidi.push_back({event, deviceName, priorDevice, priorAny, ++_physicalMidiSequence});
		// Capacity plus one second of activation grace, at the active format.
		// Scan all entries: endpoints are drained separately, not in time order.
		const auto retained = static_cast<std::int64_t>(sampleRate * 1.2f);
		auto drop = [&](const RecentPhysicalMidi& item) {
			if (!_midiHistoryDroppedThroughSample || midi::MidiSampleDelta(item.Event.sampleOffset,
				*_midiHistoryDroppedThroughSample) > 0)
				_midiHistoryDroppedThroughSample = item.Event.sampleOffset;
		};
		std::erase_if(_recentPhysicalMidi, [&](const auto& item) {
			if (midi::MidiSampleDelta(event.sampleOffset, item.Event.sampleOffset) <= retained) return false;
			drop(item);
			return true;
		});
		if (_recentPhysicalMidi.size() > 4096u)
		{
			drop(_recentPhysicalMidi.front());
			_recentPhysicalMidi.erase(_recentPhysicalMidi.begin());
		}
	}
	// Job targets cover START -> publication. This is the only ordinary recording
	// delivery; published callback topology is deliberately not the capture list.
	for (auto& target : _midiCaptureTargets)
		if (auto take = target.lock(); take && !take->IsCaptureCancelled())
			take->RecordMidiEvent(event, deviceName, event.sampleOffset);
}

void CaptureHistory::CompletePump(std::uint32_t deviceSample, bool overflow)
{
	std::erase_if(_midiCaptureTargets, [&](auto& target) {
		auto take = target.lock();
		if (!take || take->IsCaptureCancelled()) return true;
		if (overflow)
		{
			take->CancelCapture(true);
			return true;
		}
		take->CompleteMidiIngressPump(deviceSample);
		return !take->MidiTriggerCapturePending() && !take->IsArmed();
	});
}

std::optional<CaptureHistory::Replay> CaptureHistory::PrepareReplay(std::uint32_t earliest) const
{
	Replay result;
	{
		std::scoped_lock lock(_liveHeldMidiMutex);
		// Admission includes one second of structural/debounce grace. Missing
		// retained events fail explicitly, never substitute a held attack.
		if (_midiHistoryDroppedThroughSample &&
			midi::MidiSampleDelta(*_midiHistoryDroppedThroughSample, earliest) >= 0)
		{
			return std::nullopt;
		}
		result.Held = _liveHeldMidi;
		for (auto it = _recentPhysicalMidi.rbegin(); it != _recentPhysicalMidi.rend(); ++it)
		{
			if (midi::MidiSampleDelta(it->Event.sampleOffset, earliest) < 0) continue;
			result.Events.push_back(*it);
			const auto channel = it->Event.Channel();
			const auto note = it->Event.data1;
			auto restore = [&](const std::string& device, std::uint8_t velocity) {
				auto found = std::find_if(result.Held.begin(), result.Held.end(),
					[&](const auto& item) { return item.first == device; });
				if (found == result.Held.end()) return;
				if (velocity) found->second.Set(channel, note, velocity);
				else found->second.Clear(channel, note);
			};
			restore("", it->PriorAnyVelocity);
			if (!it->Device.empty()) restore(it->Device, it->PriorDeviceVelocity);
		}
	}
	std::stable_sort(result.Events.begin(), result.Events.end(), [earliest](const auto& a, const auto& b) {
		const auto left = midi::MidiSampleDelta(a.Event.sampleOffset, earliest);
		const auto right = midi::MidiSampleDelta(b.Event.sampleOffset, earliest);
		if (left != right) return left < right;
		if (a.Device != b.Device) return a.Device < b.Device;
		return a.Sequence < b.Sequence;
	});
	return result;
}

std::vector<MidiEvent> CaptureHistory::RemoveChannels(std::uint16_t removedMask)
{
	std::vector<MidiEvent> noteOffs;
	{
		std::scoped_lock lock(_liveHeldMidiMutex);
		for (std::uint8_t channel = 0; channel < 16; ++channel)
			if ((removedMask & (1u << channel)) != 0u)
				for (std::uint8_t note = 0; note < 128; ++note)
					_anonymousLiveHeldMidi.Clear(channel, note);
		for (auto& [deviceName, heldSnapshot] : _liveHeldMidi)
		{
			const bool queueNoteOffs = deviceName.empty();
			for (std::uint8_t ch = 0u; ch < 16u; ++ch)
			{
				const auto bit = static_cast<std::uint16_t>(1u << ch);
				if ((removedMask & bit) == 0u)
					continue;

				for (std::uint8_t note = 0u; note < 128u; ++note)
				{
					if (!heldSnapshot.Held.test(MidiNote::NoteSlot(ch, note)))
						continue;

					if (queueNoteOffs)
						noteOffs.push_back(MidiEvent::MakeNoteOff(0u, ch, note));
					heldSnapshot.Clear(ch, note);
				}
			}
		}
	}

	return noteOffs;
}

midi::MidiNoteSnapshot CaptureHistory::ClearSources() noexcept
{
	midi::MidiNoteSnapshot heldSnapshot;
	{
		std::scoped_lock lock(_liveHeldMidiMutex);
		auto liveHeld = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
			[](const auto& pair) { return pair.first.empty(); });
		if (liveHeld != _liveHeldMidi.end())
			heldSnapshot = liveHeld->second;
		_liveHeldMidi.clear();
		for (const auto& pedal : _liveSustainMidi) _pendingLostSustain |= pedal.second;
		_liveSustainMidi.clear();
		_recentPhysicalMidi.clear();
		_midiHistoryDroppedThroughSample.reset();
		_anonymousLiveHeldMidi = {};
	}

	return heldSnapshot;
}

void CaptureHistory::LoseDevice(const std::string& deviceName) noexcept
{
	if (deviceName.empty())
		return;
	midi::MidiNoteSnapshot released;
	{
		std::scoped_lock lock(_liveHeldMidiMutex);
		auto lost = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
			[&deviceName](const auto& entry) { return entry.first == deviceName; });
		if (lost != _liveHeldMidi.end())
		{
			released = lost->second;
			_liveHeldMidi.erase(lost);
		}
		auto pedal = std::find_if(_liveSustainMidi.begin(), _liveSustainMidi.end(), [&](const auto& item) { return item.first == deviceName; });
		if (pedal != _liveSustainMidi.end())
		{
			auto releasedPedal = pedal->second;
			_liveSustainMidi.erase(pedal);
			for (const auto& healthy : _liveSustainMidi) releasedPedal &= ~healthy.second;
			_pendingLostSustain |= releasedPedal;
		}
		auto remaining = _anonymousLiveHeldMidi;
		for (const auto& entry : _liveHeldMidi)
			if (!entry.first.empty())
				for (size_t slot = 0; slot < midi::MidiNote::TotalNoteSlots; ++slot)
					if (entry.second.Held.test(slot)) {
						remaining.Held.set(slot);
						remaining.Velocity[slot] = entry.second.Velocity[slot];
					}
		released.Held &= ~remaining.Held;
		auto aggregate = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
			[](const auto& entry) { return entry.first.empty(); });
		if (aggregate != _liveHeldMidi.end()) aggregate->second = remaining;
		_pendingLostLiveMidiNoteOffs |= released.Held;
	}
}

void CaptureHistory::FlushPendingReleases(MidiDelivery& delivery) noexcept
{
	delivery.FlushPendingLostPlaybackMidi();
	// Job producer only. Retain release bits across a full synthetic queue;
	// a surviving endpoint taking ownership of the same note suppresses release.
	std::scoped_lock lock(_liveHeldMidiMutex);
	for (auto channel = 0u; channel < 16u; ++channel)
		if (_pendingLostSustain.test(channel))
		{
			const bool healthy = std::any_of(_liveSustainMidi.begin(), _liveSustainMidi.end(), [channel](const auto& item) { return item.second.test(channel); });
			if (!healthy && !delivery.TryEnqueueSourceLossRelease({0u, static_cast<std::uint8_t>(0xb0u | channel), 64u, 0u}, delivery.LiveSustainRevision(channel))) return;
			_pendingLostSustain.reset(channel);
		}
	if (_pendingLostLiveMidiNoteOffs.none()) return;
	const auto aggregate = std::find_if(_liveHeldMidi.begin(), _liveHeldMidi.end(),
		[](const auto& entry) { return entry.first.empty(); });
	if (aggregate != _liveHeldMidi.end())
		_pendingLostLiveMidiNoteOffs &= ~aggregate->second.Held;
	for (std::uint8_t channel = 0; channel < 16; ++channel)
		for (std::uint8_t note = 0; note < 128; ++note)
		{
			const auto slot = midi::MidiNote::NoteSlot(channel, note);
			if (_pendingLostLiveMidiNoteOffs.test(slot))
			{
				if (!delivery.TryEnqueueSourceLossRelease(MidiEvent::MakeNoteOff(0u, channel, note),
					delivery.LiveNoteOnRevision(slot)))
					return;
				_pendingLostLiveMidiNoteOffs.reset(slot);
			}
		}
}

void CaptureHistory::Reset()
{
	std::scoped_lock lock(_liveHeldMidiMutex);
	_liveHeldMidi.clear();
	for (const auto& pedal : _liveSustainMidi) _pendingLostSustain |= pedal.second;
	_liveSustainMidi.clear();
	_recentPhysicalMidi.clear();
	_midiHistoryDroppedThroughSample.reset();
	_anonymousLiveHeldMidi = {};
	_pendingLostLiveMidiNoteOffs.reset();
}

void CaptureHistory::ClearRecent()
{
	std::scoped_lock lock(_liveHeldMidiMutex);
	_recentPhysicalMidi.clear();
	_midiHistoryDroppedThroughSample.reset();
}
