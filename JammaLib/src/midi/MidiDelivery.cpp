#include "MidiDelivery.h"
#include <algorithm>
#include <cmath>
#include "MidiBlockTiming.h"
#include "MidiLoop.h"
#include "../engine/LoopTake.h"
#include "../vst/VstChain.h"

using namespace midi;
using engine::LoopTake;

bool MidiDelivery::CanConfigureCompensation(const PreparedState& state, unsigned int referenceLatencySamps) noexcept
{
	constexpr unsigned int MaxCompensationSamps = 262144u;
	for (const auto path : state.DestinationPathLatency)
		if (path > referenceLatencySamps || referenceLatencySamps - path > MaxCompensationSamps) return false;
	return true;
}

bool MidiDelivery::ConfigureCompensation(const PreparedState& state, unsigned int referenceLatencySamps) noexcept
{
	if (!CanConfigureCompensation(state, referenceLatencySamps)) return false;
	for (auto index = std::size_t{0u}; index < state.MidiDestinations.size(); ++index)
	{
		const auto delay = static_cast<unsigned int>(referenceLatencySamps - state.DestinationPathLatency[index]);
		auto& destination = *state.MidiDestinations[index];
		if (destination.CompensationDelaySamps.load(std::memory_order_relaxed) != delay)
		{
			destination.CompensationDelaySamps.store(delay, std::memory_order_release);
			destination.CompensationEpoch.fetch_add(1u, std::memory_order_release);
		}
	}
	return true;
}

bool MidiDelivery::Prepare(PreparedState& state, const PreparedState* previous,
	std::shared_ptr<vst::VstChain> chain, const std::vector<std::shared_ptr<engine::LoopTake>>& takes,
	std::uint16_t enabledChannels, const std::array<MidiChannelRoute,16>& channelRoutes,
	unsigned int renderChannels, std::span<const PreparedRack> childRacks,
	std::span<const SourceGroup> sources)
{
	state.MidiChain = chain;
	state.Racks.push_back({chain, nullptr, nullptr, renderChannels, enabledChannels, channelRoutes});
	state.Racks.insert(state.Racks.end(), childRacks.begin(), childRacks.end());
	if (!sources.empty()) state.SourceGroups.assign(sources.begin(), sources.end());
	else for (const auto& take : takes)
		if (take) state.SourceGroups.push_back({take, take->GetMidiLoopSnapshot()});
	std::size_t destinationCount = 0u;
	for (const auto& rack : state.Racks) if (rack.Chain) destinationCount += rack.Chain->NumPlugins();
	if (std::count_if(state.Racks.begin(), state.Racks.end(), [](const auto& rack) { return rack.Chain && rack.Chain->NumPlugins(); }) > MaxRacks
		|| destinationCount > MaxDestinations) return false;

	std::erase_if(_midiDestinations, [](const auto& entry) { return entry.expired(); });
	std::vector<engine::LoopTake*> destinationTakes;
	const auto stationLatency = chain ? static_cast<std::uint64_t>(std::max(0, chain->GetLatencySamples())) : 0u;
	for (const auto& rack : state.Racks)
	{
		if (!rack.Chain) continue;
		std::uint64_t ancestorLatency = rack.Take ? stationLatency : 0u;
		if (rack.Loop)
			for (const auto& ancestor : state.Racks)
				if (ancestor.Take == rack.Take && !ancestor.Loop && ancestor.Chain)
					ancestorLatency += static_cast<std::uint64_t>(std::max(0, ancestor.Chain->GetLatencySamples()));
		state.EnabledChannels |= rack.EnabledChannels;
		for (std::size_t index = 0; index < rack.Chain->NumPlugins(); ++index)
		{
			const auto plugin = rack.Chain->GetPlugin(index);
			std::shared_ptr<MidiDestination> destination;
			for (const auto& weak : _midiDestinations)
				if (const auto existing = weak.lock(); existing && existing->Plugin == plugin && plugin
					&& !existing->RetirementReleased.load(std::memory_order_acquire)) { destination = existing; break; }
			if (!destination)
			{
				destination = std::make_shared<MidiDestination>();
				destination->Plugin = plugin;
				destination->AudioLoop = rack.Loop;
				destination->RenderChannels = plugin && plugin->HostedChannelCount() > 0u
					? plugin->HostedChannelCount() : std::max(1u, rack.RenderChannels);
				_midiDestinations.push_back(destination);
			}
			// One processor has exactly one block lifecycle even in malformed shared topology.
			if (std::find(state.MidiDestinations.begin(), state.MidiDestinations.end(), destination) != state.MidiDestinations.end()) return false;
			state.MidiDestinations.push_back(destination);
			destinationTakes.push_back(rack.Take);
			state.DestinationLegacyVelocity.push_back(!rack.Take);
			state.DestinationPathLatency.push_back(ancestorLatency + static_cast<std::uint64_t>(std::max(0, rack.Chain->GetLatencySamplesFrom(index))));
			std::uint16_t mask = 0u;
			bool acceptsMidi = false;
			if (plugin)
			{
				const auto capability = plugin->MidiInput();
				acceptsMidi = capability.Known && (capability.Notes || capability.Controllers);
				if (capability.Known && (capability.Notes || capability.Controllers))
					for (auto channel = 0u; channel < 16u; ++channel)
					{
						if (!(rack.EnabledChannels & (1u << channel))) continue;
						const auto& ids = rack.ChannelRoutes[channel].PluginIds;
						if (!ids.empty() && std::find(ids.begin(), ids.end(), plugin->StateId()) == ids.end()) continue;
						mask |= static_cast<std::uint16_t>(1u << channel);
						state.LiveChannelDestinations[channel].push_back(destination);
					}
			}
			state.DestinationHasMidiInput.push_back(acceptsMidi);
			state.DestinationChannels.push_back(mask);
			std::array<std::uint64_t,16> retirements{};
			if (previous)
			{
				const auto old = std::find(previous->MidiDestinations.begin(), previous->MidiDestinations.end(), destination);
				if (old != previous->MidiDestinations.end())
				{
					const auto oldIndex = static_cast<std::size_t>(old - previous->MidiDestinations.begin());
					retirements = previous->DestinationRetirements[oldIndex];
					const auto removed = static_cast<std::uint16_t>(previous->DestinationChannels[oldIndex] & ~mask);
					for (auto channel = 0u; channel < 16u; ++channel) if (removed & (1u << channel)) ++retirements[channel];
				}
			}
			state.DestinationRetirements.push_back(retirements);
		}
	}
	// No-rack/disabled takes impose no MIDI callback work and must never freeze
	// ordinary audio membership merely because they contain many recorded streams.
	std::erase_if(state.SourceGroups, [&](const auto& group) {
		for (std::size_t index = 0u; index < state.MidiDestinations.size(); ++index)
			if (state.DestinationHasMidiInput[index] && (!destinationTakes[index] || destinationTakes[index] == group.Take.get())) return false;
		return true;
	});
	std::size_t sourceCount = 0u, bindingCount = 0u;
	for (const auto& group : state.SourceGroups)
	{
		sourceCount += group.Sources.size();
		for (std::size_t index = 0u; index < state.MidiDestinations.size(); ++index)
			if (state.DestinationHasMidiInput[index] && (!destinationTakes[index] || destinationTakes[index] == group.Take.get()))
				bindingCount += group.Sources.size();
	}
	std::size_t retainedCount = state.MidiDestinations.size();
	if (previous)
	{
		for (const auto& destination : previous->MidiDestinations)
			if (std::find(state.MidiDestinations.begin(), state.MidiDestinations.end(), destination) == state.MidiDestinations.end()) ++retainedCount;
		for (const auto& destination : previous->RetiredMidiDestinations)
			if (!destination->RetirementReleased.load(std::memory_order_acquire)) ++retainedCount;
	}
	if (sourceCount > MaxSources || bindingCount > MaxBindings || retainedCount > MaxRetainedDestinations) return false;
	// Source replacement can outpace bounded release acknowledgements while audio
	// is stopped. Count unreclaimed bindings and genuinely new source/destination
	// identities before touching active routes or queueing any retirement.
	auto retainedBindings = _midiPlaybackBindings.size();
	for (const auto& group : state.SourceGroups)
		for (const auto& loop : group.Sources)
			for (std::size_t index=0u; index<state.MidiDestinations.size(); ++index)
			{
				if (!state.DestinationHasMidiInput[index] || (destinationTakes[index] && destinationTakes[index]!=group.Take.get())) continue;
				const auto& destination=state.MidiDestinations[index];
				const bool reused=std::any_of(_midiPlaybackBindings.begin(),_midiPlaybackBindings.end(),[&](const auto& existing) {
					return existing->Take==group.Take && existing->Loop==loop && existing->Destination==destination
						&& !existing->ReleaseQueued && !existing->ReleaseConsumed.load(std::memory_order_acquire)
						&& existing->RoutingActive.load(std::memory_order_acquire);
				});
				if (!reused && ++retainedBindings>MaxRetainedBindings) return false;
			}
	// All admission checks precede binding-state changes and retirement queues.
	for (const auto& group : state.SourceGroups)
		for (const auto& loop : group.Sources)
		{
			std::vector<std::shared_ptr<MidiPlaybackBinding>> bindings;
			std::vector<std::size_t> indices;
			for (std::size_t index = 0u; index < state.MidiDestinations.size(); ++index)
			{
				if (!state.DestinationHasMidiInput[index] || (destinationTakes[index] && destinationTakes[index] != group.Take.get())) continue;
				const auto& destination = state.MidiDestinations[index];
				std::shared_ptr<MidiPlaybackBinding> binding;
				for (const auto& existing : _midiPlaybackBindings)
					if (existing->Take == group.Take && existing->Loop == loop && existing->Destination == destination
						&& !existing->ReleaseQueued && !existing->ReleaseConsumed.load(std::memory_order_acquire)
						&& existing->RoutingActive.load(std::memory_order_acquire)) { binding = existing; break; }
				if (!binding)
				{
					binding = std::make_shared<MidiPlaybackBinding>();
					binding->Take = group.Take;
					binding->Loop = loop;
					binding->Destination = destination;
					_midiPlaybackBindings.push_back(binding);
				}
				bindings.push_back(binding);
				indices.push_back(index);
			}
			state.MidiPlaybackRoutes.push_back(std::move(bindings));
			state.PlaybackDestinationIndices.push_back(std::move(indices));
		}
	for (const auto& binding : _midiPlaybackBindings)
	{
		bool active = false;
		for (const auto& routes : state.MidiPlaybackRoutes)
			for (const auto& route : routes) active |= route == binding;
		binding->RoutingActive.store(active, std::memory_order_release);
		if (!active && !binding->ReleaseQueued && !binding->ReleaseConsumed.load(std::memory_order_acquire)
			&& std::find(_pendingLostPlaybackMidi.begin(), _pendingLostPlaybackMidi.end(), binding) == _pendingLostPlaybackMidi.end())
		{
			binding->EmitSourceLossRelease = true;
			_pendingLostPlaybackMidi.push_back(binding);
		}
	}
	auto retain = [&](const std::shared_ptr<MidiDestination>& candidate) {
		if (candidate && std::find(state.MidiDestinations.begin(), state.MidiDestinations.end(), candidate) == state.MidiDestinations.end()
			&& std::find(state.RetiredMidiDestinations.begin(), state.RetiredMidiDestinations.end(), candidate) == state.RetiredMidiDestinations.end())
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
	FlushPendingLostPlaybackMidi();
	return true;
}

bool MidiDelivery::_TryDeliverMidi(MidiDestination& destination, const MidiEvent& event,
	bool isRealtime, MidiPlaybackBinding* owner, const midi::MidiVoiceEvent& voice) noexcept
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
	entry.Voice = voice;
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

void MidiDelivery::_InvalidateScheduledMidiAtBlockStart(const PreparedState& state, std::uint32_t blockStartSample, bool forceCancel) noexcept
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
			const auto destinationIndex = state.PlaybackDestinationIndices[output][index];
			const auto channels = static_cast<std::uint16_t>(binding->Destination->AudioLoop && binding->Destination->AudioLoop->IsMuted()
				? 0u : state.DestinationChannels[destinationIndex]);
			auto removed = static_cast<std::uint16_t>(binding->AllowedChannels & ~channels);
			for (auto channel = 0u; channel < 16u; ++channel)
				if (binding->AppliedRetirements[channel] != state.DestinationRetirements[destinationIndex][channel])
					removed |= static_cast<std::uint16_t>(1u << channel);
			binding->AppliedRetirements = state.DestinationRetirements[destinationIndex];
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
			const auto sourceState = binding->Loop->PublishedStateAtAudioBoundary();
			const auto revision = sourceState ? std::optional<std::uint64_t>{sourceState->Revision} : std::nullopt;
			const auto recovery = sourceState ? std::optional<std::uint64_t>{sourceState->RecoveryEpoch} : std::nullopt;
			const auto relocation = binding->Loop->RelocationGenerationAtAudioBoundary();
			const bool changed = (binding->HasObservedSourceState
				&& (static_cast<bool>(revision) != binding->HasObservedSourceRevision
					|| (revision && *revision != binding->ObservedSourceRevision)))
				|| relocation != binding->ObservedRelocationGeneration;
			const bool recoveryChanged = binding->HasObservedSourceState
				&& (recovery.has_value() != binding->HasObservedSourceRevision
					|| (recovery && *recovery != binding->ObservedRecoveryEpoch));
			const bool relocate = relocation != binding->ObservedRelocationGeneration;
			binding->ReconcileScheduledThisBlock = changed && !recoveryChanged && !relocate;
			if (recovery) binding->ObservedRecoveryEpoch = *recovery;
			binding->HasObservedSourceState = true;
			binding->HasObservedSourceRevision = revision.has_value();
			if (revision) binding->ObservedSourceRevision = *revision;
			binding->ObservedRelocationGeneration = relocation;
			binding->CancelScheduledThisBlock = recoveryChanged || relocate || binding->Take->IsMuted()
				|| binding->Take->IsCaptureCancelled();
			if (recoveryChanged || relocate) binding->Loop->RequestHeldFlush();
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
		bool reconciled = false;
		for (auto index = std::size_t{ 0u }; index < destination->ScheduledCount; ++index)
		{
			auto entry = destination->Scheduled[index];
			auto* owner = entry.Owner;
			if (owner && owner->ReconcileScheduledThisBlock && entry.Voice.Token && !owner->CancelScheduledThisBlock)
			{
				reconciled = true;
				auto delta = std::int32_t{0};
				if (!owner->Loop->ReconcileQueuedVoice(entry.Voice, entry.Event, delta))
				{
					if (entry.Event.IsNoteOn())
					{
						owner->QueuedEvents.fetch_sub(1u, std::memory_order_release);
						continue;
					}
					// This release may be the only remaining owner after the logical
					// source already accepted its old delayed endpoint.
					entry.Event.sampleOffset = blockStartSample;
					entry.Voice = {};
				}
				else entry.Event.sampleOffset += delta;
			}
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
		if (retained != destination->ScheduledCount || reconciled)
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
	return TryOnVoiceEvent(outputIndex, event, {});
}

bool MidiDelivery::MidiPlaybackSink::TryOnVoiceEvent(unsigned int outputIndex, const MidiEvent& event,
	const midi::MidiVoiceEvent& voice) noexcept
{
	if (outputIndex >= _state.MidiPlaybackRoutes.size()) return false;
	const auto slot = midi::MidiNote::NoteSlot(event.Channel(), event.data1);
	// Every destination must retain a release before the source forgets it.
	// Attacks still establish source state if any destination accepts them.
	const bool sustainRelease = event.MessageType() == 0xb0u && event.data1 == 64u && event.data2 < 64u;
	const bool release = event.IsNoteOff() || sustainRelease;
	bool accepted = release;
	bool eligible = false;
	for (auto routeIndex = std::size_t{0u}; routeIndex < _state.MidiPlaybackRoutes[outputIndex].size(); ++routeIndex)
	{
		const auto& binding = _state.MidiPlaybackRoutes[outputIndex][routeIndex];
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
		auto deliveredEvent = event;
		if (event.IsNoteOn() && _state.DestinationLegacyVelocity[_state.PlaybackDestinationIndices[outputIndex][routeIndex]])
		{
			deliveredEvent.data2 = static_cast<std::uint8_t>(std::clamp(static_cast<int>(
				event.data2 * binding->Take->RecordedMidiVelocityScale() + 0.5f), 0, 127));
			if (!deliveredEvent.IsNoteOn()) continue; // Preserve the legacy zero-scale rejection.
		}
		const bool delivered = MidiDelivery::_TryDeliverMidi(destination, deliveredEvent,
			suppressSource || immediateRelease, binding.get(), voice);
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
	std::uint32_t blockStartSample, bool discontinuity) noexcept
{
	(void)takes; // SourceGroups pin the accepted membership.
	if (discontinuity)
	{
		_hasDiscontinuityCutoff = true;
		_discontinuityCutoff = blockStartSample;
		for (const auto& destination : state.MidiDestinations)
		{
			destination->PendingReleases |= destination->PluginHeld | destination->LiveHeld;
			destination->PendingSustainRelease |= destination->PluginSustain | destination->LiveSustain;
			destination->LiveHeld.reset();
			destination->LiveSustain.reset();
		}
		for (const auto& routes : state.MidiPlaybackRoutes)
			for (const auto& binding : routes)
				if (binding && binding->Loop) binding->Loop->RequestHeldFlush();
	}

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
		_InvalidateScheduledMidiAtBlockStart(state, blockStartSample, discontinuity);
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

		if (_hasDiscontinuityCutoff && liveMidi.IsNoteOn() && midi::MidiSampleDelta(liveMidi.sampleOffset, _discontinuityCutoff) < 0) continue;
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

	if (_hasDiscontinuityCutoff)
	{
		MidiEvent immediate{};
		SyntheticMidi synthetic{};
		const bool immediateBehind = _immediateLiveMidiIngress.Peek(immediate)
			&& midi::MidiSampleDelta(immediate.sampleOffset, _discontinuityCutoff) < 0;
		const bool syntheticBehind = _syntheticLiveMidiIngress.Peek(synthetic)
			&& midi::MidiSampleDelta(synthetic.Event.sampleOffset, _discontinuityCutoff) < 0;
		if (!immediateBehind && !syntheticBehind) _hasDiscontinuityCutoff = false;
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
		_InvalidateScheduledMidiAtBlockStart(state, blockStartSample, true);
		return;
	}

	auto midiOutputIndex = 0u;
	MidiPlaybackSink midiSink(state);
	for (const auto& group : state.SourceGroups)
		if (group.Take)
			midiOutputIndex += group.Take->ReadPreparedMidiBlock(group.Sources, blockStartSample,
				sampsToRead, midiSink, midiOutputIndex, true, group.Audio.get());

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

void MidiDelivery::FinalReleaseWhileQuiescent(const PreparedState& state) noexcept
{
    // Removed processors will not begin another audio block. One bounded final
    // attempt uses the existing accepted/rejected release accounting before retirement.
    const auto release=[](const auto& destination) {
        if(!destination || !destination->Plugin || !destination->Plugin->IsLoaded()) return;
        vst::IVstPlugin::PluginCallGuard call(*destination->Plugin,true);
        if(!call) return;
        destination->Plugin->BeginMidiBlock(0u,1u);
        destination->PendingSustainRelease |= destination->PluginSustain;
        destination->PendingReleases |= destination->PluginHeld;
        for(auto channel=0u;channel<16u;++channel) if(destination->PendingSustainRelease.test(channel))
            _TryDeliverMidiNow(*destination,{0u,static_cast<std::uint8_t>(0xb0u|channel),64u,0u},true);
        for(std::size_t slot=0u;slot<MidiNote::TotalNoteSlots;++slot) if(destination->PendingReleases.test(slot))
            _TryDeliverMidiNow(*destination,MidiEvent::MakeNoteOff(0u,static_cast<std::uint8_t>(slot/128u),static_cast<std::uint8_t>(slot%128u)),true);
    };
    for(const auto& destination:state.MidiDestinations) release(destination);
    for(const auto& destination:state.RetiredMidiDestinations) release(destination);
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

bool MidiDelivery::LatencyPathsChanged(const PreparedState& state) noexcept
{
	const auto station = state.MidiChain ? static_cast<std::uint64_t>(std::max(0, state.MidiChain->GetLatencySamples())) : 0u;
	std::size_t destination = 0u;
	for (const auto& rack : state.Racks)
	{
		if (!rack.Chain) continue;
		auto ancestor = rack.Take ? station : 0u;
		if (rack.Loop)
			for (const auto& parent : state.Racks)
				if (parent.Take == rack.Take && !parent.Loop && parent.Chain)
					ancestor += static_cast<std::uint64_t>(std::max(0, parent.Chain->GetLatencySamples()));
		for (std::size_t index=0u; index<rack.Chain->NumPlugins(); ++index, ++destination)
			if (destination >= state.DestinationPathLatency.size()
				|| state.DestinationPathLatency[destination] != ancestor + static_cast<std::uint64_t>(std::max(0,rack.Chain->GetLatencySamplesFrom(index)))) return true;
	}
	return destination != state.DestinationPathLatency.size();
}
