#include "MidiDelivery.h"
#include <algorithm>
#include "MidiBlockTiming.h"
#include "MidiLoop.h"
#include "../engine/LoopTake.h"
#include "../vst/VstChain.h"

using namespace midi;
using engine::LoopTake;

bool MidiDelivery::ConfigureCompensation(const PreparedState& state, unsigned int referenceLatencySamps) noexcept
{
	constexpr unsigned int MaxCompensationSamps = 262144u;
	if (!state.MidiChain) return true;
	for (auto index = std::size_t{ 0u }; index < state.MidiDestinations.size(); ++index)
	{
		const auto remaining = std::max(0, state.MidiChain->GetLatencySamplesFrom(index));
		if (static_cast<unsigned int>(remaining) > referenceLatencySamps
			|| referenceLatencySamps - static_cast<unsigned int>(remaining) > MaxCompensationSamps)
			return false;
	}
	for (auto index = std::size_t{ 0u }; index < state.MidiDestinations.size(); ++index)
	{
		const auto remaining = std::max(0, state.MidiChain->GetLatencySamplesFrom(index));
		const auto delay = referenceLatencySamps - static_cast<unsigned int>(remaining);
		auto& destination = *state.MidiDestinations[index];
		if (destination.CompensationDelaySamps.load(std::memory_order_relaxed) != delay)
		{
			destination.CompensationDelaySamps.store(delay, std::memory_order_release);
			destination.CompensationEpoch.fetch_add(1u, std::memory_order_release);
		}
	}
	return true;
}

void MidiDelivery::Prepare(PreparedState& state, const PreparedState* previous,
	std::shared_ptr<vst::VstChain> chain, const std::vector<std::shared_ptr<engine::LoopTake>>& takes,
	std::uint16_t enabledChannels, const std::array<MidiChannelRoute, 16>& channelRoutes,
	unsigned int renderChannels)
{
	state.MidiChain = std::move(chain);
	state.EnabledChannels = enabledChannels;
	auto queueRemovedBindings = [&]() {
		if (!previous) return;
		for (const auto& routes : previous->MidiPlaybackRoutes)
			for (const auto& binding : routes)
			{
				if (!binding || binding->ReleaseQueued
					|| binding->ReleaseConsumed.load(std::memory_order_acquire)) continue;
				bool stillRouted = false;
				for (const auto& currentRoutes : state.MidiPlaybackRoutes)
					for (const auto& current : currentRoutes) stillRouted |= current == binding;
				if (stillRouted
					|| std::find(_pendingLostPlaybackMidi.begin(), _pendingLostPlaybackMidi.end(), binding)
						!= _pendingLostPlaybackMidi.end()) continue;
				binding->EmitSourceLossRelease = true;
				_pendingLostPlaybackMidi.push_back(binding);
			}
		FlushPendingLostPlaybackMidi();
	};
	auto retainRetired = [&]() {
		auto retain = [&](const std::shared_ptr<MidiDestination>& candidate) {
			if (!candidate
				|| std::find(state.MidiDestinations.begin(), state.MidiDestinations.end(), candidate) != state.MidiDestinations.end()
				|| std::find(state.RetiredMidiDestinations.begin(), state.RetiredMidiDestinations.end(), candidate) != state.RetiredMidiDestinations.end()) return;
			state.RetiredMidiDestinations.push_back(candidate);
		};
		if (previous)
		{
			for (const auto& destination : previous->MidiDestinations) retain(destination);
			for (const auto& destination : previous->RetiredMidiDestinations)
				if (!destination->RetirementReleased.load(std::memory_order_acquire)) retain(destination);
		}
		for (const auto& binding : _midiPlaybackBindings)
			if (binding->QueuedEvents.load(std::memory_order_acquire) > 0u) retain(binding->Destination);
	};
	if (!state.MidiChain)
	{
		retainRetired();
		queueRemovedBindings();
		for (const auto& binding : _midiPlaybackBindings)
			binding->RoutingActive.store(false, std::memory_order_release);
		return;
	}
	std::erase_if(_midiDestinations, [](const auto& entry) { return entry.expired(); });
	std::vector<std::shared_ptr<MidiDestination>> destinations;
	for (std::size_t index = 0u; index < state.MidiChain->NumPlugins(); ++index)
	{
		const auto plugin = state.MidiChain->GetPlugin(index);
		std::shared_ptr<MidiDestination> destination;
		for (const auto& weak : _midiDestinations)
			if (const auto existing = weak.lock(); existing && existing->Plugin == plugin && !existing->RetirementReleased.load(std::memory_order_acquire))
				{ destination = existing; break; }
		if (!destination)
		{
			destination = std::make_shared<MidiDestination>();
			destination->Plugin = plugin;
			destination->RenderChannels = plugin && plugin->HostedChannelCount() > 0u ? plugin->HostedChannelCount()
				: std::max(1u, renderChannels);
			_midiDestinations.push_back(destination);
		}
		destinations.push_back(std::move(destination));
	}
	// The host currently has no plugin-to-plugin MIDI forwarding (VST3 outputEvents
	// is null). Audio rack order alone does not imply an upstream MIDI supply.
	for (auto channel = 0u; channel < 16u; ++channel)
	{
		if (!(enabledChannels & (1u << channel))) continue;
		const auto& ids = channelRoutes[channel].PluginIds;
		for (const auto& destination : destinations)
		{
			if (!destination->Plugin) continue;
			const auto capabilities = destination->Plugin->MidiInput();
			if (!capabilities.Known || (!capabilities.Notes && !capabilities.Controllers)) continue;
			// Retained unavailable identities still make a channel cabled, never broadcast.
			if (ids.empty() || std::find(ids.begin(), ids.end(), destination->Plugin->StateId()) != ids.end())
				state.LiveChannelDestinations[channel].push_back(destination);
		}
	}
	// Route history matters within the current chain. Removed plugins are unloaded
	// by the structural owner; do not repin their destinations on every publication.
	state.MidiDestinations = destinations;
	state.DestinationChannels.assign(destinations.size(), 0u);
	for (auto index = 0u; index < destinations.size(); ++index)
		for (auto channel = 0u; channel < 16u; ++channel)
			if (std::find(state.LiveChannelDestinations[channel].begin(), state.LiveChannelDestinations[channel].end(), destinations[index]) != state.LiveChannelDestinations[channel].end())
				state.DestinationChannels[index] |= static_cast<std::uint16_t>(1u << channel);
	state.DestinationRetirements.resize(destinations.size());
	if (previous)
		for (auto index = std::size_t{0u}; index < destinations.size(); ++index)
		{
			const auto old = std::find(previous->MidiDestinations.begin(), previous->MidiDestinations.end(), destinations[index]);
			if (old == previous->MidiDestinations.end()) continue;
			const auto oldIndex = static_cast<std::size_t>(old - previous->MidiDestinations.begin());
			auto& retirements = state.DestinationRetirements[index];
			retirements = previous->DestinationRetirements[oldIndex];
			const auto removed = previous->DestinationChannels[oldIndex] & ~state.DestinationChannels[index];
			for (auto channel = 0u; channel < 16u; ++channel)
				if (removed & (1u << channel)) ++retirements[channel];
		}
	for (const auto& take : takes)
		for (const auto& loop : take->GetMidiLoopSnapshot())
		{
			std::vector<std::shared_ptr<MidiPlaybackBinding>> bindings;
			for (const auto& destination : destinations)
			{
				std::shared_ptr<MidiPlaybackBinding> binding;
				for (const auto& existing : _midiPlaybackBindings)
					if (existing->Take == take &&
						existing->Loop == loop && existing->Destination == destination)
						{ binding = existing; break; }
				if (!binding)
				{
					binding = std::make_shared<MidiPlaybackBinding>();
					binding->Take = take;
					binding->Loop = loop;
					binding->Destination = destination;
					_midiPlaybackBindings.push_back(binding);
				}
				bindings.push_back(std::move(binding));
			}
			state.MidiPlaybackRoutes.push_back(std::move(bindings));
		}
	for (const auto& binding : _midiPlaybackBindings)
	{
		bool active = false;
		for (const auto& routes : state.MidiPlaybackRoutes)
			for (const auto& route : routes)
				active |= route == binding;
		binding->RoutingActive.store(active, std::memory_order_release);
	}
	retainRetired();
	queueRemovedBindings();
}

bool MidiDelivery::_TryDeliverMidi(MidiDestination& destination, const MidiEvent& event,
	bool isRealtime, MidiPlaybackBinding* owner) noexcept
{
	if (isRealtime) return owner
		? _TryDeliverRecordedMidiNow(destination, event, *owner, true)
		: _TryDeliverMidiNow(destination, event, true);
	const auto delay = destination.ActiveCompensationDelaySamps;
	if (delay == 0u) return owner
		? _TryDeliverRecordedMidiNow(destination, event, *owner, false)
		: _TryDeliverMidiNow(destination, event, false);
	if (!destination.Plugin || !destination.Plugin->IsLoaded()) return event.IsNoteOff();
	const auto& scheduled = destination.Scheduled;
	const auto count = destination.ScheduledCount;
	const bool release = event.IsNoteOff() || (event.MessageType() == 0xb0u && event.data1 == 64u && event.data2 < 64u);
	if (count >= MidiDestination::ScheduledCapacity
		|| (!release && count >= MidiDestination::ScheduledCapacity
			- MidiDestination::ScheduledReleaseReserve)) return false;
	MidiDestination::ScheduledMidi entry;
	entry.Event = event;
	entry.Event.sampleOffset += delay; // Modular sample-clock arithmetic.
	entry.IsRealtime = isRealtime;
	entry.Owner = owner;
	if (owner) owner->QueuedEvents.fetch_add(1u, std::memory_order_release);
	entry.Sequence = ++destination.ScheduleSequence;
	auto precedes = [](const MidiDestination::ScheduledMidi& left,
		const MidiDestination::ScheduledMidi& right) noexcept {
		const auto delta = midi::MidiSampleDelta(left.Event.sampleOffset,
			right.Event.sampleOffset);
		if (delta != 0) return delta < 0;
		if (left.Event.IsNoteOff() != right.Event.IsNoteOff()) return left.Event.IsNoteOff();
		return left.Sequence < right.Sequence;
	};
	auto index = destination.ScheduledCount++;
	while (index > 0u)
	{
		const auto parent = (index - 1u) / 2u;
		if (!precedes(entry, scheduled[parent])) break;
		destination.Scheduled[index] = scheduled[parent];
		index = parent;
	}
	destination.Scheduled[index] = entry;
	return true;
}

void MidiDelivery::_DrainScheduledMidi(const PreparedState& state,
	std::uint32_t blockStartSample, unsigned int numSamps) noexcept
{
	for (const auto& destination : state.MidiDestinations)
	{
		if (!destination) continue;
		auto precedes = [](const MidiDestination::ScheduledMidi& left,
			const MidiDestination::ScheduledMidi& right) noexcept {
			const auto delta = midi::MidiSampleDelta(left.Event.sampleOffset,
				right.Event.sampleOffset);
			if (delta != 0) return delta < 0;
			if (left.Event.IsNoteOff() != right.Event.IsNoteOff()) return left.Event.IsNoteOff();
			return left.Sequence < right.Sequence;
		};
		while (destination->ScheduledCount > 0u
			&& midi::ClassifyMidiSampleInBlock(destination->Scheduled[0].Event.sampleOffset,
				blockStartSample, numSamps) != midi::MidiBlockSamplePosition::Future)
		{
			auto entry = destination->Scheduled[0];
			auto last = destination->Scheduled[--destination->ScheduledCount];
			auto parent = std::size_t{ 0u };
			while (parent * 2u + 1u < destination->ScheduledCount)
			{
				auto child = parent * 2u + 1u;
				if (child + 1u < destination->ScheduledCount
					&& precedes(destination->Scheduled[child + 1u], destination->Scheduled[child])) ++child;
				if (!precedes(destination->Scheduled[child], last)) break;
				destination->Scheduled[parent] = destination->Scheduled[child];
				parent = child;
			}
			if (destination->ScheduledCount > 0u) destination->Scheduled[parent] = last;
			if (midi::ClassifyMidiSampleInBlock(entry.Event.sampleOffset,
				blockStartSample, numSamps) == midi::MidiBlockSamplePosition::Late)
				entry.Event.sampleOffset = blockStartSample;
			const bool delivered = entry.Owner
				? _TryDeliverRecordedMidiNow(*destination, entry.Event, *entry.Owner, entry.IsRealtime)
				: _TryDeliverMidiNow(*destination, entry.Event, entry.IsRealtime);
			if (!delivered && entry.Event.IsNoteOn() && entry.Owner)
			{
				if (entry.Owner->Loop) entry.Owner->Loop->RequestHeldFlush();
				_RecoverCancelledScheduledMidi(*destination,
					_CancelScheduledMidiForOwner(*destination, entry.Owner));
			}
			if (entry.Owner) entry.Owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
		}
	}
}

std::bitset<midi::MidiNote::TotalNoteSlots> MidiDelivery::_CancelScheduledMidiForOwner(MidiDestination& destination,
	MidiPlaybackBinding* owner) noexcept
{
	std::bitset<midi::MidiNote::TotalNoteSlots> affected;
	if (!owner || destination.ScheduledCount == 0u) return affected;
	owner->QueuedEvents.fetch_add(1u, std::memory_order_release); // Pin through heap compaction.
	_DropRecordedAudibleOwner(destination, *owner);
	for (auto index = std::size_t{ 0u }; index < destination.ScheduledCount;)
	{
		if (destination.Scheduled[index].Owner != owner) { ++index; continue; }
		const auto& removed = destination.Scheduled[index];
		if (removed.Event.IsNoteOn() || removed.Event.IsNoteOff())
			affected.set(midi::MidiNote::NoteSlot(removed.Event.Channel(), removed.Event.data1));
		owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
		destination.Scheduled[index] = destination.Scheduled[--destination.ScheduledCount];
	}
	auto precedes = [](const MidiDestination::ScheduledMidi& left,
		const MidiDestination::ScheduledMidi& right) noexcept {
		const auto delta = midi::MidiSampleDelta(left.Event.sampleOffset,
			right.Event.sampleOffset);
		if (delta != 0) return delta < 0;
		if (left.Event.IsNoteOff() != right.Event.IsNoteOff()) return left.Event.IsNoteOff();
		return left.Sequence < right.Sequence;
	};
	for (auto root = destination.ScheduledCount / 2u; root > 0u;)
	{
		--root;
		const auto entry = destination.Scheduled[root];
		auto parent = root;
		while (parent * 2u + 1u < destination.ScheduledCount)
		{
			auto child = parent * 2u + 1u;
			if (child + 1u < destination.ScheduledCount
				&& precedes(destination.Scheduled[child + 1u], destination.Scheduled[child])) ++child;
			if (!precedes(destination.Scheduled[child], entry)) break;
			destination.Scheduled[parent] = destination.Scheduled[child];
			parent = child;
		}
		destination.Scheduled[parent] = entry;
	}
	owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
	return affected;
}

void MidiDelivery::_RecoverCancelledScheduledMidi(MidiDestination& destination,
	const std::bitset<midi::MidiNote::TotalNoteSlots>& affected) noexcept
{
	if (affected.none()) return;
	destination.RecoveringCompensationChange = true;
	for (auto slot = std::size_t{ 0u }; slot < midi::MidiNote::TotalNoteSlots; ++slot)
		if (affected.test(slot) && destination.PluginHeld.test(slot)
			&& destination.PlaybackOwners[slot] == 0u
			&& destination.RecordedAudibleOwners[slot] == 0u
			&& !destination.LiveHeld.test(slot))
			destination.PendingReleases.set(slot);
}

void MidiDelivery::_InvalidateScheduledMidiAtBlockStart(const PreparedState& state, bool forceCancel) noexcept
{
	// Retired plugins are outside the active chain; their future notes must never drain.
	for (const auto& destination : state.RetiredMidiDestinations)
	{
		if (!destination) continue;
		for (auto index = std::size_t{ 0u }; index < destination->ScheduledCount; ++index)
		{
			if (auto* owner = destination->Scheduled[index].Owner)
			{
				if (owner->Loop) owner->Loop->RequestHeldFlush();
				_DropRecordedAudibleOwner(*destination, *owner);
				owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
			}
		}
		destination->ScheduledCount = 0u;
	}
	for (auto output = std::size_t{0u}; output < state.MidiPlaybackRoutes.size(); ++output)
		for (auto index = std::size_t{0u}; index < state.MidiPlaybackRoutes[output].size(); ++index)
		{
			const auto& binding = state.MidiPlaybackRoutes[output][index];
			if (!binding || !binding->Loop || !binding->Destination) continue;
			const auto channels = state.DestinationChannels[index];
			auto removed = static_cast<std::uint16_t>(binding->AllowedChannels & ~channels);
			for (auto channel = 0u; channel < 16u; ++channel)
				if (binding->AppliedRetirements[channel] != state.DestinationRetirements[index][channel])
					removed |= static_cast<std::uint16_t>(1u << channel);
			binding->AppliedRetirements = state.DestinationRetirements[index];
			binding->CancelChannelsThisBlock = removed;
			binding->AllowedChannels = channels;
			if (removed)
			{
				// Channel cables retire only this binding's removed channels. A source-wide
				// flush would also release surviving channels and fanout destinations.
				auto& destination = *binding->Destination;
				_DropRecordedAudibleOwner(destination, *binding, true, removed);
				for (auto channel = 0u; channel < 16u; ++channel)
				{
					if (!(removed & (1u << channel))) continue;
					for (auto note = 0u; note < 128u; ++note)
					{
						const auto slot = midi::MidiNote::NoteSlot(static_cast<std::uint8_t>(channel),
							static_cast<std::uint8_t>(note));
						if (!binding->Held.test(slot)) continue;
						binding->Held.reset(slot);
						--destination.PlaybackOwners[slot];
					}
				}
				binding->HasHeld.store(binding->Held.any(), std::memory_order_release);
			}
			const auto revision = binding->Loop->PublishedRevisionAtAudioBoundary();
			const auto relocation = binding->Loop->RelocationGenerationAtAudioBoundary();
			const bool changed = (binding->HasObservedSourceState
				&& (static_cast<bool>(revision) != binding->HasObservedSourceRevision
					|| (revision && *revision != binding->ObservedSourceRevision)))
				|| relocation != binding->ObservedRelocationGeneration;
			binding->HasObservedSourceState = true;
			binding->HasObservedSourceRevision = revision.has_value();
			if (revision) binding->ObservedSourceRevision = *revision;
			binding->ObservedRelocationGeneration = relocation;
			binding->CancelScheduledThisBlock = changed || binding->Take->IsMuted()
				|| binding->Take->IsCaptureCancelled();
			if (changed) binding->Loop->RequestHeldFlush();
		}
	// A removed route can still own future entries in a destination retained by the new graph.
	for (const auto& destination : state.MidiDestinations)
	{
		if (!destination) continue;
		const bool bypassed = destination->Plugin && destination->Plugin->IsBypassed();
		const bool bypassStarted = bypassed && !destination->LastBypassed;
		if (bypassStarted)
		{
			destination->PendingReleases |= destination->LiveHeld;
			destination->LiveHeld.reset();
			destination->PendingSustainRelease |= destination->LiveSustain;
			destination->LiveSustain.reset();
		}
		destination->LastBypassed = bypassed;
		std::bitset<midi::MidiNote::TotalNoteSlots> affected;
		auto retained = std::size_t{ 0u };
		for (auto index = std::size_t{ 0u }; index < destination->ScheduledCount; ++index)
		{
			const auto entry = destination->Scheduled[index];
			auto* owner = entry.Owner;
			const bool invalid = owner && (forceCancel || bypassStarted
				|| !owner->RoutingActive.load(std::memory_order_acquire)
				|| owner->CancelScheduledThisBlock);
			if (owner && (invalid || !(owner->AllowedChannels & (1u << entry.Event.Channel()))
				|| (owner->CancelChannelsThisBlock & (1u << entry.Event.Channel()))))
			{
				if (invalid)
				{
					if (owner->Loop) owner->Loop->RequestHeldFlush();
					_DropRecordedAudibleOwner(*destination, *owner);
					if (entry.Event.IsNoteOn() || entry.Event.IsNoteOff())
						affected.set(midi::MidiNote::NoteSlot(entry.Event.Channel(), entry.Event.data1));
				}
				owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
			}
			else destination->Scheduled[retained++] = entry;
		}
		if (retained != destination->ScheduledCount)
		{
			destination->ScheduledCount = retained;
			auto precedes = [](const MidiDestination::ScheduledMidi& left,
				const MidiDestination::ScheduledMidi& right) noexcept {
				const auto delta = midi::MidiSampleDelta(left.Event.sampleOffset, right.Event.sampleOffset);
				if (delta != 0) return delta < 0;
				if (left.Event.IsNoteOff() != right.Event.IsNoteOff()) return left.Event.IsNoteOff();
				return left.Sequence < right.Sequence;
			};
			for (auto root = retained / 2u; root > 0u;)
			{
				--root;
				const auto entry = destination->Scheduled[root];
				auto parent = root;
				while (parent * 2u + 1u < retained)
				{
					auto child = parent * 2u + 1u;
					if (child + 1u < retained && precedes(destination->Scheduled[child + 1u], destination->Scheduled[child])) ++child;
					if (!precedes(destination->Scheduled[child], entry)) break;
					destination->Scheduled[parent] = destination->Scheduled[child];
					parent = child;
				}
				destination->Scheduled[parent] = entry;
			}
			_RecoverCancelledScheduledMidi(*destination, affected);
		}
	}
}

void MidiDelivery::_DropRecordedAudibleOwner(MidiDestination& destination, MidiPlaybackBinding& owner,
	bool emitRelease, std::uint16_t channels) noexcept
{
	for (auto channel = 0u; channel < 16u; ++channel)
		if ((channels & (1u << channel)) && owner.AudibleSustain.test(channel))
		{
			owner.AudibleSustain.reset(channel);
			if (destination.RecordedSustainOwners[channel]) --destination.RecordedSustainOwners[channel];
			if (!destination.RecordedSustainOwners[channel] && !destination.LiveSustain.test(channel))
				destination.PendingSustainRelease.set(channel);
		}
	auto retained = std::size_t{0u};
	for (auto index = std::size_t{ 0u }; index < owner.AudibleSlotCount; ++index)
	{
		const auto slot = owner.AudibleSlots[index];
		if (!(channels & (1u << (slot / 128u))))
		{
			owner.AudibleSlotPositions[slot] = static_cast<std::uint16_t>(retained);
			owner.AudibleSlots[retained++] = slot;
			continue;
		}
		owner.AudibleHeld.reset(slot);
		if (destination.RecordedAudibleOwners[slot] > 0u)
			--destination.RecordedAudibleOwners[slot];
		if (emitRelease && destination.RecordedAudibleOwners[slot] == 0u
			&& !destination.LiveHeld.test(slot) && destination.PluginHeld.test(slot))
			destination.PendingReleases.set(slot);
	}
	owner.AudibleSlotCount = retained;
}

bool MidiDelivery::_TryDeliverRecordedMidiNow(MidiDestination& destination, const MidiEvent& event,
	MidiPlaybackBinding& owner, bool isRealtime) noexcept
{
	if (event.MessageType() == 0xb0u && event.data1 == 64u)
	{
		const auto channel = event.Channel();
		const bool down = event.data2 >= 64u;
		if (down)
		{
			if (!_TryDeliverMidiNow(destination, event, isRealtime)) return false;
			if (!owner.AudibleSustain.test(channel))
			{
				owner.AudibleSustain.set(channel);
				++destination.RecordedSustainOwners[channel];
			}
		}
		else if (owner.AudibleSustain.test(channel))
		{
			if (destination.RecordedSustainOwners[channel] == 1u && !destination.LiveSustain.test(channel)
				&& !_TryDeliverMidiNow(destination, event, isRealtime)) return false;
			owner.AudibleSustain.reset(channel);
			--destination.RecordedSustainOwners[channel];
		}
		return true;
	}
	if (!event.IsNoteOn() && !event.IsNoteOff())
		return _TryDeliverMidiNow(destination, event, isRealtime);
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	if (event.IsNoteOn())
	{
		if (!_TryDeliverMidiNow(destination, event, isRealtime)) return false;
		if (!owner.AudibleHeld.test(slot))
		{
			owner.AudibleHeld.set(slot);
			owner.AudibleSlotPositions[slot] = static_cast<std::uint16_t>(owner.AudibleSlotCount);
			owner.AudibleSlots[owner.AudibleSlotCount++] = static_cast<std::uint16_t>(slot);
			++destination.RecordedAudibleOwners[slot];
		}
		return true;
	}
	if (!owner.AudibleHeld.test(slot)) return true;
	if (destination.RecordedAudibleOwners[slot] == 1u && !destination.LiveHeld.test(slot)
		&& !_TryDeliverMidiNow(destination, event, isRealtime)) return false;
	owner.AudibleHeld.reset(slot);
	const auto index = owner.AudibleSlotPositions[slot];
	const auto moved = owner.AudibleSlots[--owner.AudibleSlotCount];
	owner.AudibleSlots[index] = moved;
	owner.AudibleSlotPositions[moved] = index;
	--destination.RecordedAudibleOwners[slot];
	return true;
}

bool MidiDelivery::_TryDeliverMidiNow(MidiDestination& destination, const MidiEvent& event,
	bool isRealtime) noexcept
{
	if (!destination.Plugin || !destination.Plugin->IsLoaded()) return event.IsNoteOff();
	if (destination.Plugin->IsBypassed() && (event.IsNoteOn() || (event.MessageType() == 0xb0u && event.data1 == 64u && event.data2 >= 64u))) return false;
	const bool sustain = event.MessageType() == 0xb0u && event.data1 == 64u;
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	// A replacement pedal owner must follow the retained release in adapter
	// order. Reject its attack if the old release still cannot be accepted.
	if (sustain && event.data2 >= 64u && destination.PendingSustainRelease.test(event.Channel()))
	{
		if (!destination.Plugin->TrySendMidiEvent({event.sampleOffset, event.status, 64u, 0u}, true)) return false;
		destination.PendingSustainRelease.reset(event.Channel());
		destination.PluginSustain.reset(event.Channel());
	}
	if (event.IsNoteOn() && destination.PendingReleases.test(slot))
	{
		const auto release = MidiEvent::MakeNoteOff(event.sampleOffset, event.Channel(), event.data1,
			destination.PendingReleaseVelocity[slot]);
		if (!destination.Plugin->TrySendMidiEvent(release, true)) return false;
		destination.PendingReleaseVelocity[slot] = 0u;
		destination.PendingReleases.reset(slot);
		destination.PluginHeld.reset(slot);
		destination.BlockTouched.set(slot);
	}
	if (!destination.Plugin->TrySendMidiEvent(event, isRealtime))
	{
		if (sustain && event.data2 < 64u)
		{
			destination.PendingSustainRelease.set(event.Channel());
			return true;
		}
		if (event.IsNoteOff())
		{
			destination.PendingReleaseVelocity[slot] = event.data2;
			destination.PendingReleases.set(slot);
			return true; // Retained for bounded retry at the next audio block.
		}
		return false;
	}
	if (event.IsNoteOn()) destination.PluginHeld.set(slot);
	if (sustain)
	{
		destination.PluginSustain.set(event.Channel(), event.data2 >= 64u);
		if (event.data2 < 64u) destination.PendingSustainRelease.reset(event.Channel());
	}
	else if (event.IsNoteOff())
	{
		destination.PluginHeld.reset(slot);
		destination.PendingReleaseVelocity[slot] = 0u;
		destination.PendingReleases.reset(slot);
	}
	if (event.IsNoteOn() || event.IsNoteOff()) destination.BlockTouched.set(slot);
	return true;
}

void MidiDelivery::_DrainPendingMidiReleases(const PreparedState& state, std::uint32_t sample) noexcept
{
	constexpr unsigned int MaxRecoveryReleasesPerDestination = 64u;
	for (const auto& destination : state.MidiDestinations)
	{
		if (destination && destination->Plugin)
			for (auto channel = 0u; channel < 16u; ++channel)
				if (destination->PendingSustainRelease.test(channel))
					_TryDeliverMidiNow(*destination, {sample, static_cast<std::uint8_t>(0xb0u | channel), 64u, 0u}, true);
		if (!destination || !destination->Plugin || !destination->Plugin->IsLoaded()
			|| destination->PendingReleases.none()) continue;
		auto budget = MaxRecoveryReleasesPerDestination;
		auto scanned = std::size_t{ 0u };
		while (budget > 0u && scanned < midi::MidiNote::TotalNoteSlots)
		{
			const auto slot = destination->ReleaseCursor;
			if (destination->PendingReleases.test(slot))
			{
				const auto release = MidiEvent::MakeNoteOff(sample,
					static_cast<std::uint8_t>(slot / 128u), static_cast<std::uint8_t>(slot % 128u),
					destination->PendingReleaseVelocity[slot]);
				if (!destination->Plugin->TrySendMidiEvent(release, true)) break;
				destination->PendingReleaseVelocity[slot] = 0u;
				destination->PendingReleases.reset(slot);
				destination->PluginHeld.reset(slot);
				destination->BlockTouched.set(slot);
				--budget;
			}
			destination->ReleaseCursor = (slot + 1u) % midi::MidiNote::TotalNoteSlots;
			++scanned;
		}
	}
}

void MidiDelivery::_RecoverSkippedMidiBlocks(const PreparedState& state) noexcept
{
	for (const auto& destination : state.MidiDestinations)
	{
		if (!destination || !destination->Plugin || destination->Plugin->MidiBlockWasProcessed()) continue;
		// All accepted events in this block were discarded by the adapter.
		destination->PendingReleases |= destination->BlockHeldAtStart | destination->BlockTouched;
		destination->PendingSustainRelease |= destination->BlockSustainAtStart | destination->PluginSustain;
		for (const auto& routes : state.MidiPlaybackRoutes)
			for (const auto& binding : routes)
				if (binding && binding->Destination == destination)
					_DropRecordedAudibleOwner(*destination, *binding);
		destination->PluginHeld.reset();
		for (const auto& routes : state.MidiPlaybackRoutes)
			for (const auto& binding : routes)
				if (binding && binding->Destination == destination && binding->Loop)
					binding->Loop->RequestHeldFlush();
	}
}

void MidiDelivery::_DispatchLiveMidi(const PreparedState& state, const MidiEvent& event,
	bool sourceLossNoteOff, std::uint32_t liveNoteOnRevision) noexcept
{
	if (event.MessageType() == 0xb0u && event.data1 == 64u)
	{
		const auto channel = event.Channel();
		if (event.data2 < 64u)
		{
			for (const auto& destination : state.MidiDestinations)
				if (destination->LiveSustain.test(channel))
				{
					if (sourceLossNoteOff && static_cast<std::int32_t>(destination->LiveSustainRevision[channel] - liveNoteOnRevision) > 0) continue;
					destination->LiveSustain.reset(channel);
					if (!destination->RecordedSustainOwners[channel]) _TryDeliverMidi(*destination, event, true);
				}
		}
		else
			for (const auto& destination : state.LiveChannelDestinations[channel])
				if (_TryDeliverMidi(*destination, event, true))
				{
					destination->LiveSustain.set(channel);
					destination->LiveSustainRevision[channel] = liveNoteOnRevision;
				}
		return;
	}
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	auto hasNewLiveOwner = [&](const MidiDestination& destination) noexcept {
		return destination.LiveHeld.test(slot) &&
			static_cast<std::int32_t>(destination.LiveNoteOnRevision[slot] - liveNoteOnRevision) > 0;
	};
	if (event.IsNoteOff())
	{
		for (const auto& destination : state.MidiDestinations)
		{
			const bool wasHeld = destination->LiveHeld.test(slot);
			if (sourceLossNoteOff && hasNewLiveOwner(*destination)) continue;
			destination->LiveHeld.reset(slot);
			if (wasHeld && destination->RecordedAudibleOwners[slot] == 0u &&
				std::find(state.LiveChannelDestinations[event.Channel()].begin(), state.LiveChannelDestinations[event.Channel()].end(), destination) == state.LiveChannelDestinations[event.Channel()].end())
				_TryDeliverMidi(*destination, event, true);
		}
	}
	for (const auto& destination : state.LiveChannelDestinations[event.Channel()])
	{
		if (event.IsNoteOff() && (destination->RecordedAudibleOwners[slot] != 0u
			|| (sourceLossNoteOff && hasNewLiveOwner(*destination))
			|| (destination->BlockTouched.test(slot) && !destination->PluginHeld.test(slot)))) continue;
		if (_TryDeliverMidi(*destination, event, true) && event.IsNoteOn())
		{
			destination->PendingLiveReset.reset(slot);
			destination->LiveHeld.set(slot);
			destination->LiveNoteOnRevision[slot] = liveNoteOnRevision;
		}
	}
}

void MidiDelivery::MidiPlaybackSink::OnEvent(unsigned int outputIndex, const MidiEvent& event) noexcept
{
	(void)TryOnEvent(outputIndex, event);
}

bool MidiDelivery::MidiPlaybackSink::TryOnEvent(unsigned int outputIndex, const MidiEvent& event) noexcept
{
	if (outputIndex >= _state.MidiPlaybackRoutes.size()) return false;
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	// Every destination must retain a release before the source forgets it.
	// Attacks still establish source state if any destination accepts them.
	const bool sustainRelease = event.MessageType() == 0xb0u && event.data1 == 64u && event.data2 < 64u;
	const bool release = event.IsNoteOff() || sustainRelease;
	bool accepted = release;
	bool eligible = false;
	for (const auto& binding : _state.MidiPlaybackRoutes[outputIndex])
	{
		if (binding->Take->IsSourceLossCancelled() && event.IsNoteOn()) continue;
		auto& destination = *binding->Destination;
		const auto channel = event.Channel();
		if (!(binding->AllowedChannels & (1u << channel)) &&
			!(event.IsNoteOff() && binding->Held.test(slot)) &&
			!(sustainRelease && binding->AudibleSustain.test(channel))) continue;
		eligible = true;
		const bool suppressSource = binding->Take->IsMuted() || binding->Take->IsSourceLossCancelled();
		if (suppressSource && release)
			MidiDelivery::_RecoverCancelledScheduledMidi(destination,
				MidiDelivery::_CancelScheduledMidiForOwner(destination, binding.get()));
		if (event.IsNoteOff() && !binding->Held.test(slot))
			continue;
		const bool immediateRelease = release && destination.RecoveringCompensationChange;
		const bool delivered = MidiDelivery::_TryDeliverMidi(destination, event,
			suppressSource || immediateRelease, binding.get());
		if (release) accepted &= delivered;
		else accepted |= delivered;
		if (event.IsNoteOn() && delivered && !binding->Held.test(slot))
		{
			binding->Held.set(slot);
			++destination.PlaybackOwners[slot];
			binding->HasHeld.store(true, std::memory_order_release);
		}
		else if (event.IsNoteOff() && delivered && binding->Held.test(slot))
		{
			binding->Held.reset(slot);
			--destination.PlaybackOwners[slot];
			binding->HasHeld.store(binding->Held.any(), std::memory_order_release);
		}
	}
	// Suppression consumes the source event; it must not replay after re-enable.
	return !eligible || accepted;
}

bool MidiDelivery::_ReleaseLostPlaybackMidi(MidiPlaybackBinding& binding, std::uint32_t sample,
	unsigned int& budget, unsigned int& slotBudget) noexcept
{
	auto& destination = *binding.Destination;
	_DropRecordedAudibleOwner(destination, binding, false);
	const auto cancelled = _CancelScheduledMidiForOwner(destination, &binding);
	if (binding.EmitSourceLossRelease) _RecoverCancelledScheduledMidi(destination, cancelled);
	if (binding.Held.none())
	{
		binding.HasHeld.store(false, std::memory_order_release);
		return true;
	}
	while (binding.ReleaseSlot < midi::MidiNote::TotalNoteSlots && budget > 0u && slotBudget > 0u)
	{
		const auto slot = binding.ReleaseSlot++;
		--slotBudget;
		if (!binding.Held.test(slot)) continue;
		--budget;
		binding.Held.reset(slot);
		--destination.PlaybackOwners[slot];
		if (binding.EmitSourceLossRelease && destination.PlaybackOwners[slot] == 0u
			&& destination.RecordedAudibleOwners[slot] == 0u && !destination.LiveHeld.test(slot))
		{
			destination.PendingReleaseVelocity[slot] = 0u;
			destination.PendingReleases.reset(slot); // This explicit release supersedes queued recovery.
			_TryDeliverMidi(destination, MidiEvent::MakeNoteOff(sample,
				static_cast<std::uint8_t>(slot / 128u), static_cast<std::uint8_t>(slot % 128u)), true);
		}
	}
	if (binding.ReleaseSlot < midi::MidiNote::TotalNoteSlots) return false;
	binding.HasHeld.store(false, std::memory_order_release);
	return true;
}

void MidiDelivery::QueueLostPlaybackMidi(const std::shared_ptr<LoopTake>& take, bool emitRelease)
{
	for (const auto& binding : _midiPlaybackBindings)
		if (binding->Take == take &&
			!binding->ReleaseQueued && !binding->ReleaseConsumed.load(std::memory_order_acquire) &&
			std::find(_pendingLostPlaybackMidi.begin(), _pendingLostPlaybackMidi.end(), binding) == _pendingLostPlaybackMidi.end())
		{
			binding->EmitSourceLossRelease = emitRelease;
			_pendingLostPlaybackMidi.push_back(binding);
		}
	FlushPendingLostPlaybackMidi();
}

void MidiDelivery::FlushPendingLostPlaybackMidi()
{
	std::erase_if(_pendingLostPlaybackMidi, [](const auto& binding) {
		return binding->ReleaseConsumed.load(std::memory_order_acquire);
	});
	for (const auto& binding : _pendingLostPlaybackMidi)
	{
		if (binding->ReleaseQueued) continue;
		auto event = MidiEvent::MakeNoteOff(_hasLastSyntheticLiveMidiSample ? _lastSyntheticLiveMidiSample : 0u, 0u, 0u);
		if (!_TryEnqueueSyntheticMidi({ event, binding.get() })) return;
		binding->ReleaseQueued = true;
	}
}

void MidiDelivery::BeginRetiredBlock(const PreparedState& state,
	std::uint32_t blockStartSample, unsigned int sampsToRead) noexcept
{
	for (const auto& destination : state.RetiredMidiDestinations)
		if (destination->Plugin && !destination->RetirementReleased.load(std::memory_order_acquire))
		{
			vst::IVstPlugin::PluginCallGuard call(*destination->Plugin, true);
			if (call) destination->Plugin->BeginMidiBlock(blockStartSample, sampsToRead);
			destination->BlockHeldAtStart = destination->PluginHeld;
			destination->BlockSustainAtStart = destination->PluginSustain;
			destination->PendingReleases |= destination->PluginHeld;
			destination->PendingSustainRelease |= destination->PluginSustain;
			destination->LiveHeld.reset();
			destination->LiveSustain.reset();
		}
}

void MidiDelivery::ProcessBlock(const PreparedState& state,
	std::span<const std::shared_ptr<engine::LoopTake>> takes, std::span<float* const> instrumentPtrs,
	bool vstActive, const std::atomic<bool>& membershipRemoving,
	unsigned int sampsToRead,
	std::uint32_t blockStartSample) noexcept
{
	if (vstActive)
	{
		// A route/mask publication releases only this live owner's contribution.
		// Recorded owners keep their notes and pedal state on the same processor.
		for (auto index = 0u; index < state.MidiDestinations.size(); ++index)
		{
			const auto& destination = state.MidiDestinations[index];
			const auto effective = static_cast<std::uint16_t>(state.DestinationChannels[index]);
			auto removed = static_cast<std::uint16_t>(destination->AppliedLiveChannels & ~effective);
			for (auto channel = 0u; channel < 16u; ++channel)
				if (destination->AppliedLiveRetirements[channel] != state.DestinationRetirements[index][channel])
					removed |= static_cast<std::uint16_t>(1u << channel);
			destination->AppliedLiveRetirements = state.DestinationRetirements[index];
			destination->AppliedLiveChannels = effective;
			if (!removed) continue;
			for (auto channel = 0u; channel < 16u; ++channel)
			{
				if (!(removed & (1u << channel))) continue;
				if (destination->LiveSustain.test(channel))
				{
					destination->LiveSustain.reset(channel);
					if (!destination->RecordedSustainOwners[channel]) destination->PendingSustainRelease.set(channel);
				}
				for (auto note = 0u; note < 128u; ++note)
				{
					const auto slot = midi::MidiNote::NoteSlot(static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(note));
					if (!destination->LiveHeld.test(slot)) continue;
					destination->LiveHeld.reset(slot);
					if (!destination->RecordedAudibleOwners[slot]) destination->PendingReleases.set(slot);
				}
			}
		}
		for (const auto& destination : state.MidiDestinations)
		{
			destination->RecoveringCompensationChange = false;
			const auto epoch = destination->CompensationEpoch.load(std::memory_order_acquire);
			if (epoch != destination->AppliedCompensationEpoch)
			{
				const auto recordedHeld = destination->PluginHeld & ~destination->LiveHeld;
				for (const auto& routes : state.MidiPlaybackRoutes)
					for (const auto& binding : routes)
						if (binding && binding->Destination == destination)
							_DropRecordedAudibleOwner(*destination, *binding);
				const bool affected = destination->ScheduledCount > 0u || recordedHeld.any();
				for (auto index = std::size_t{ 0u }; index < destination->ScheduledCount; ++index)
					if (auto* owner = destination->Scheduled[index].Owner)
						owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
				destination->ScheduledCount = 0u;
				destination->PendingReleases |= recordedHeld;
				destination->ActiveCompensationDelaySamps = destination->CompensationDelaySamps.load(std::memory_order_acquire);
				destination->AppliedCompensationEpoch = epoch;
				destination->RecoveringCompensationChange = affected;
				if (affected)
					for (const auto& routes : state.MidiPlaybackRoutes)
						for (const auto& binding : routes)
							if (binding && binding->Destination == destination && binding->Loop)
								binding->Loop->RequestHeldFlush();
			}
			destination->BlockHeldAtStart = destination->PluginHeld;
			destination->BlockSustainAtStart = destination->PluginSustain;
			destination->BlockTouched.reset();
		}
		_InvalidateScheduledMidiAtBlockStart(state);
		_DrainPendingMidiReleases(state, blockStartSample);
	}

	constexpr auto MaxLiveMidiEventsPerBlock = 64u;
	// Reset releases are staged while quiescent, then delivered after the plugin
	// begins its block. Sending them on the UI thread would be cleared by Begin.
	unsigned int resetReleaseBudget = MaxLiveMidiEventsPerBlock;
	auto resetScanBudget = midi::MidiNote::TotalNoteSlots;
	if (vstActive)
		for (const auto& destination : state.MidiDestinations)
		{
			if (resetScanBudget == 0u || resetReleaseBudget == 0u) break;
			if (destination->PendingLiveReset.none()) continue;
			for (std::size_t slot = 0; slot < midi::MidiNote::TotalNoteSlots && resetReleaseBudget > 0u && resetScanBudget > 0u; ++slot)
			{
				--resetScanBudget;
				if (!destination->PendingLiveReset.test(slot)) continue;
				destination->PendingLiveReset.reset(slot);
				--resetReleaseBudget;
				if (destination->RecordedAudibleOwners[slot] == 0u && !destination->LiveHeld.test(slot)
					&& destination->PluginHeld.test(slot))
					_TryDeliverMidiNow(*destination, MidiEvent::MakeNoteOff(blockStartSample,
						static_cast<std::uint8_t>(slot / 128u), static_cast<std::uint8_t>(slot % 128u)), true);
			}
		}
	auto releaseSlotBudget = static_cast<unsigned int>(midi::MidiNote::TotalNoteSlots);
	for (auto dispatched = 0u; dispatched < MaxLiveMidiEventsPerBlock; ++dispatched)
	{
		MidiEvent immediate{};
		SyntheticMidi syntheticEntry{};
		const auto hasImmediate = _immediateLiveMidiIngress.Peek(immediate);
		const auto hasSynthetic = _syntheticLiveMidiIngress.Peek(syntheticEntry);
		const auto& synthetic = syntheticEntry.Event;
		const auto immediatePosition = hasImmediate
			? midi::ClassifyMidiSampleInBlock(immediate.sampleOffset, blockStartSample, sampsToRead)
			: midi::MidiBlockSamplePosition::Future;
		const auto syntheticPosition = hasSynthetic
			? midi::ClassifyMidiSampleInBlock(synthetic.sampleOffset, blockStartSample, sampsToRead)
			: midi::MidiBlockSamplePosition::Future;
		if (immediatePosition == midi::MidiBlockSamplePosition::Future
			&& syntheticPosition == midi::MidiBlockSamplePosition::Future)
			break;

		const auto immediateDelta = midi::MidiSampleDelta(immediate.sampleOffset, blockStartSample);
		const auto syntheticDelta = midi::MidiSampleDelta(synthetic.sampleOffset, blockStartSample);
		bool chooseSynthetic = immediatePosition == midi::MidiBlockSamplePosition::Future;
		if (!chooseSynthetic && syntheticPosition != midi::MidiBlockSamplePosition::Future)
		{
			chooseSynthetic = syntheticDelta < immediateDelta
				|| (syntheticDelta == immediateDelta
					&& (synthetic.IsNoteOff() != immediate.IsNoteOff()
						? synthetic.IsNoteOff()
						: true));
		}

		MidiEvent liveMidi{};
		if (chooseSynthetic)
		{
			if (syntheticEntry.Release)
			{
				auto budget = MaxLiveMidiEventsPerBlock - dispatched;
				if (!_ReleaseLostPlaybackMidi(*syntheticEntry.Release, blockStartSample, budget, releaseSlotBudget)) break;
				dispatched += std::max(1u, MaxLiveMidiEventsPerBlock - dispatched - budget) - 1u;
				_syntheticLiveMidiIngress.Pop(syntheticEntry);
				// Last borrower access: job reclamation follows this acknowledgement.
				syntheticEntry.Release->ReleaseConsumed.store(true, std::memory_order_release);
				continue;
			}
			_syntheticLiveMidiIngress.Pop(syntheticEntry);
			liveMidi = syntheticEntry.Event;
		}
		else
			_immediateLiveMidiIngress.Pop(liveMidi);

		liveMidi.sampleOffset = midi::RebaseMidiSampleForVstBlock(liveMidi.sampleOffset,
			blockStartSample,
			blockStartSample);
		const bool release = liveMidi.IsNoteOff() || (liveMidi.MessageType() == 0xb0u && liveMidi.data1 == 64u && liveMidi.data2 < 64u);
		if (vstActive && (!membershipRemoving.load(std::memory_order_acquire) || release) && ((state.EnabledChannels & (1u << liveMidi.Channel())) != 0u || release))
		{
			auto revision = syntheticEntry.LiveNoteOnRevision;
			if (liveMidi.MessageType() == 0xb0u && liveMidi.data1 == 64u && liveMidi.data2 >= 64u)
				revision = _liveSustainRevisions[liveMidi.Channel()].fetch_add(1u, std::memory_order_release) + 1u;
			if (liveMidi.IsNoteOn())
				revision = _liveNoteOnRevisions[midi::MidiNote::NoteSlot(liveMidi.Channel(), liveMidi.data1)].fetch_add(1u, std::memory_order_release) + 1u;
			_DispatchLiveMidi(state, liveMidi, chooseSynthetic && syntheticEntry.SourceLossNoteOff, revision);
		}
	}

	// Removed processors retain their callback-owned release recovery until a
	// processed block accepts it. Their output is discarded before destruction.
	bool allRetiredReleased = !state.RetiredMidiDestinations.empty();
	for (const auto& destination : state.RetiredMidiDestinations)
	{
		if (destination->RetirementReleased.load(std::memory_order_acquire)) continue;
		const auto& plugin = destination->Plugin;
		if (plugin && plugin->IsLoaded() && !plugin->IsBypassed())
		{
			for (auto channel = 0u; channel < 16u; ++channel)
				if (destination->PendingSustainRelease.test(channel))
					_TryDeliverMidiNow(*destination, {blockStartSample, static_cast<std::uint8_t>(0xb0u | channel), 64u, 0u}, true);
			auto budget = 64u;
			for (auto slot = 0u; slot < midi::MidiNote::TotalNoteSlots && budget; ++slot)
				if (destination->PendingReleases.test(slot))
				{
					--budget;
					_TryDeliverMidiNow(*destination, MidiEvent::MakeNoteOff(blockStartSample,
						static_cast<std::uint8_t>(slot / 128u), static_cast<std::uint8_t>(slot % 128u)), true);
				}
			vst::IVstPlugin::PluginCallGuard call(*plugin, true);
			if (call)
			{
				for (auto channel = 0u; channel < destination->RenderChannels; ++channel)
					std::fill_n(instrumentPtrs[channel], sampsToRead, 0.0f);
				plugin->ProcessBlockMulti(instrumentPtrs.data(), static_cast<int>(destination->RenderChannels), sampsToRead);
			}
			if (!plugin->MidiBlockWasProcessed())
			{
				destination->PendingReleases |= destination->PluginHeld | destination->BlockHeldAtStart;
				destination->PendingSustainRelease |= destination->PluginSustain | destination->BlockSustainAtStart;
				allRetiredReleased = false;
				continue;
			}
		}
		// Bypassed/unloaded removed processors are silent and terminal; teardown
		// itself ends their voices, without touching any surviving processor.
		// A latency suspension may later reuse this instance. Preserve its existing
		// guarded reset receipt so bypass cannot resurrect old voices on resume.
		if (plugin && (plugin->IsBypassed() || !plugin->IsLoaded())) plugin->RequestMidiPanic();
		const bool released = !plugin || !plugin->IsLoaded() || plugin->IsBypassed()
			|| (destination->PendingReleases.none() && destination->PendingSustainRelease.none());
		destination->RetirementReleased.store(released, std::memory_order_release);
		allRetiredReleased &= released;
	}
	if (allRetiredReleased) state.RetiredMidiQueueCancelled.store(true, std::memory_order_release);
	if (!vstActive)
	{
		_InvalidateScheduledMidiAtBlockStart(state, true);
		return;
	}

	auto midiOutputIndex = 0u;
	MidiPlaybackSink midiSink(state);
	for (const auto& take : takes)
	{
		if (!take)
			continue;
		midiOutputIndex += take->ReadMidiBlock(blockStartSample,
			sampsToRead,
			midiSink,
			midiOutputIndex);
	}

	_DrainScheduledMidi(state, blockStartSample, sampsToRead);
}

bool MidiDelivery::_TryEnqueueOrderedLiveMidi(midi::MidiQueue<1024>& queue,
	bool& hasLastSample,
	std::uint32_t& lastSample,
	const midi::MidiEvent& event) noexcept
{
	auto queuedEvent = event;
	if (hasLastSample
		&& midi::MidiSampleDelta(queuedEvent.sampleOffset, lastSample) < 0)
	{
		queuedEvent.sampleOffset = lastSample;
	}

	if (!queue.Push(queuedEvent))
		return false;

	lastSample = queuedEvent.sampleOffset;
	hasLastSample = true;
	return true;
}

void MidiDelivery::ResetLiveMidiWhileQuiescent() noexcept
{
	_immediateLiveMidiIngress.Clear();
	_hasLastImmediateLiveMidiSample = false;
	_lastImmediateLiveMidiSample = 0u;
	// Playback release records carry pinned binding acknowledgements. Retain
	// them exactly; discard monitoring events, including undelivered NoteOns.
	std::array<SyntheticMidi, 1024u> playbackReleases{};
	std::size_t count = 0u;
	SyntheticMidi entry{};
	while (_syntheticLiveMidiIngress.Pop(entry))
		if (entry.Release) playbackReleases[count++] = entry;
	_syntheticLiveMidiIngress.Clear();
	_hasLastSyntheticLiveMidiSample = false;
	_lastSyntheticLiveMidiSample = 0u;
	for (std::size_t i = 0; i < count; ++i)
	{
		playbackReleases[i].Event.sampleOffset = 0u;
		_TryEnqueueSyntheticMidi(playbackReleases[i]);
	}
	for (const auto& weak : _midiDestinations)
		if (const auto destination = weak.lock())
		{
			destination->PendingLiveReset |= destination->LiveHeld;
			destination->LiveHeld.reset();
			for (auto channel = 0u; channel < 16u; ++channel)
				if (destination->LiveSustain.test(channel) && !destination->RecordedSustainOwners[channel])
					destination->PendingSustainRelease.set(channel);
			destination->LiveSustain.reset();
		}
}

bool MidiDelivery::_TryEnqueueSyntheticMidi(SyntheticMidi entry) noexcept
{
	auto& queued = entry.Event;
	if (_hasLastSyntheticLiveMidiSample &&
		midi::MidiSampleDelta(queued.sampleOffset, _lastSyntheticLiveMidiSample) < 0)
		queued.sampleOffset = _lastSyntheticLiveMidiSample;
	if (!_syntheticLiveMidiIngress.Push(entry)) return false;
	_lastSyntheticLiveMidiSample = queued.sampleOffset;
	_hasLastSyntheticLiveMidiSample = true;
	return true;
}

void MidiDelivery::Collect()
{
	std::erase_if(_midiPlaybackBindings, [](const auto& binding) {
		return binding.use_count() == 1 && !binding->HasHeld.load(std::memory_order_acquire)
			&& binding->QueuedEvents.load(std::memory_order_acquire) == 0u;
	});
}

bool MidiDelivery::HasUnconsumedRemovedBinding() const noexcept
{
	for (const auto& binding : _midiPlaybackBindings)
		if (!binding->RoutingActive.load(std::memory_order_acquire)
			&& !binding->ReleaseConsumed.load(std::memory_order_acquire)) return true;
	return false;
}

void MidiDelivery::ClearAfterReadersStopped()
{
	for (const auto& binding : _midiPlaybackBindings)
	{
		vst::QueueForUiThreadDestroy(binding->Destination->Plugin);
		// A removed take may still be pinned by a held playback route.
		binding->Take->ForceUnloadAllVstPlugins();
	}
	_syntheticLiveMidiIngress.Clear();
	_pendingLostPlaybackMidi.clear();
	_midiPlaybackBindings.clear();
	_midiDestinations.clear();
}

bool MidiDelivery::TryEnqueueImmediateLiveMidi(const MidiEvent& event) noexcept
{
	return _TryEnqueueOrderedLiveMidi(_immediateLiveMidiIngress,
		_hasLastImmediateLiveMidiSample, _lastImmediateLiveMidiSample, event);
}
