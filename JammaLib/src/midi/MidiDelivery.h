#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "MidiQueue.h"
#include "MidiNote.h"
#include "MidiVstOutputSink.h"

namespace engine { class LoopTake; }
namespace vst { class IVstPlugin; class VstChain; }

namespace midi
{
	class MidiLoop;

	// MIDI routing and delivery. Prepared topology is immutable while borrowed;
	// destination ownership/queues are callback-owned. The structural publisher
	// retains and collects binding/plugin lifetimes after reader acknowledgements.
	class MidiDelivery
	{
	public:
		// Prepared/pinned on the structural owner; MIDI ownership is mutated only
		// by audio dispatch. Counts are per destination, never per changing output index.
		struct MidiPlaybackBinding;
		struct MidiDestination
		{
			std::shared_ptr<vst::IVstPlugin> Plugin;
			unsigned int RenderChannels = 0u; // Immutable captured hosted width.
			std::bitset<midi::MidiNote::TotalNoteSlots> LiveHeld;
			// Callback-owned pedal accounting, independent of changing live routes.
			std::bitset<16> LiveSustain;
			std::array<std::uint32_t, 16> LiveSustainRevision{};
			std::uint16_t AppliedLiveChannels = 0u; // Callback-only effective route mask.
			std::bitset<16> PluginSustain;
			std::bitset<16> BlockSustainAtStart;
			std::bitset<16> PendingSustainRelease;
			std::array<unsigned int, 16> RecordedSustainOwners{};
			// Audio owner consumes; quiescent lifecycle owner marks actual delivered
			// live notes after all writers drain. Destination lifetime follows snapshots.
			std::bitset<midi::MidiNote::TotalNoteSlots> PendingLiveReset;
			std::array<std::uint32_t, midi::MidiNote::TotalNoteSlots> LiveNoteOnRevision{};
			std::array<unsigned int, midi::MidiNote::TotalNoteSlots> PlaybackOwners{};
			std::array<unsigned int, midi::MidiNote::TotalNoteSlots> RecordedAudibleOwners{}; // Audio delivery time.
			// Audio callback owner: rejected releases retry in later blocks.
			std::bitset<midi::MidiNote::TotalNoteSlots> PendingReleases;
			std::array<std::uint8_t, midi::MidiNote::TotalNoteSlots> PendingReleaseVelocity{};
			std::bitset<midi::MidiNote::TotalNoteSlots> PluginHeld;
			std::bitset<midi::MidiNote::TotalNoteSlots> BlockHeldAtStart;
			std::bitset<midi::MidiNote::TotalNoteSlots> BlockTouched;
			std::size_t ReleaseCursor = 0u;
			struct ScheduledMidi
			{
				midi::MidiEvent Event;
				std::uint64_t Sequence = 0u;
				MidiPlaybackBinding* Owner = nullptr; // Pinned by QueuedEvents.
				bool IsRealtime = false;
			};
			static constexpr std::size_t ScheduledCapacity = 1024u;
			static constexpr std::size_t ScheduledReleaseReserve = 256u;
			std::array<ScheduledMidi, ScheduledCapacity> Scheduled{};
			std::size_t ScheduledCount = 0u; // Audio callback only.
			std::uint64_t ScheduleSequence = 0u;
			std::atomic<unsigned int> CompensationDelaySamps{ 0u };
			std::atomic<std::uint32_t> CompensationEpoch{ 0u }; // Job publication.
			std::uint32_t AppliedCompensationEpoch = 0u; // Audio callback.
			unsigned int ActiveCompensationDelaySamps = 0u; // Audio callback.
			bool RecoveringCompensationChange = false; // Current callback block.
			bool LastBypassed = false; // Audio callback.
			std::atomic<bool> RetirementReleased{false}; // Audio publishes, structural owner reclaims.
		};
		struct MidiPlaybackBinding
		{
			std::shared_ptr<engine::LoopTake> Take;
			std::shared_ptr<midi::MidiLoop> Loop;
			std::shared_ptr<MidiDestination> Destination;
			std::bitset<midi::MidiNote::TotalNoteSlots> Held;
			std::bitset<midi::MidiNote::TotalNoteSlots> AudibleHeld; // Delayed note reached the plugin.
			std::bitset<16> AudibleSustain; // Callback-owned delivered pedal state.
			std::uint16_t AllowedChannels = 0xffffu; // Callback-owned route eligibility.
			std::array<std::uint16_t, midi::MidiNote::TotalNoteSlots> AudibleSlots{};
			std::array<std::uint16_t, midi::MidiNote::TotalNoteSlots> AudibleSlotPositions{};
			std::size_t AudibleSlotCount = 0u; // Dense callback-owned active slots.
			std::atomic<bool> HasHeld{ false }; // Audio publishes; job reclaims unused bindings.
			std::atomic<unsigned int> QueuedEvents{ 0u }; // Pins raw queue identity until removed.
			std::atomic<bool> RoutingActive{ true }; // Job publication; callback cancellation.
			std::uint64_t ObservedSourceRevision = 0u; // Audio callback.
			bool HasObservedSourceRevision = false;
			bool HasObservedSourceState = false;
			bool CancelScheduledThisBlock = false;
			std::uint64_t ObservedRelocationGeneration = 0u; // Audio callback.
			std::atomic<bool> ReleaseConsumed{ false };
			std::size_t ReleaseSlot = 0u; // Audio only; preserves the per-block ingress budget.
			bool ReleaseQueued = false; // Job producer only.
			bool EmitSourceLossRelease = true; // Frozen before queue publication.
		};
	private:
		struct SyntheticMidi
		{
			midi::MidiEvent Event;
			MidiPlaybackBinding* Release = nullptr; // Pinned until audio acknowledges.
			bool SourceLossNoteOff = false;
			std::uint32_t LiveNoteOnRevision = 0u;
		};
	public:
		struct PreparedState
		{
			std::shared_ptr<vst::VstChain> MidiChain;
			std::vector<std::shared_ptr<MidiDestination>> LiveMidiDestinations;
			std::array<std::vector<std::shared_ptr<MidiDestination>>, 16> LiveChannelDestinations;
			std::vector<std::uint16_t> LiveDestinationChannels; // Prepared, immutable.
			std::vector<std::shared_ptr<MidiDestination>> MidiDestinations;
			std::vector<std::shared_ptr<MidiDestination>> RetiredMidiDestinations;
			mutable std::atomic<bool> RetiredMidiQueueCancelled{ false };
			std::vector<std::vector<std::shared_ptr<MidiPlaybackBinding>>> MidiPlaybackRoutes;
			std::vector<bool> MidiPlaybackUsesChannelRoutes;
			std::array<bool, 16> RecordedChannelExplicit{};
			std::array<std::vector<std::shared_ptr<MidiDestination>>, 16> RecordedChannelDestinations;
		};

		void Prepare(PreparedState& state, const PreparedState* previous,
			std::shared_ptr<vst::VstChain> chain, const std::vector<std::shared_ptr<engine::LoopTake>>& takes,
			const MidiVstRoutingSnapshot* outputRoutes, const std::array<LiveVstChannelRoute, 16>& channelRoutes,
			unsigned int renderChannels);
		static bool ConfigureCompensation(const PreparedState& state, unsigned int referenceLatencySamps) noexcept;
		static void BeginRetiredBlock(const PreparedState& state,
			std::uint32_t blockStartSample, unsigned int numSamps) noexcept;
		void ProcessBlock(const PreparedState& state,
			std::span<const std::shared_ptr<engine::LoopTake>> takes, std::span<float* const> instrumentPtrs,
			bool vstActive, const std::atomic<bool>& membershipRemoving,
			const std::atomic<std::uint16_t>& allowedChannels, unsigned int numSamps,
			std::uint32_t blockStartSample) noexcept;
		static void RecoverSkippedBlocks(const PreparedState& state) noexcept { _RecoverSkippedMidiBlocks(state); }
		bool TryEnqueueImmediateLiveMidi(const MidiEvent& event) noexcept;
		bool TryEnqueueSyntheticLiveMidi(const MidiEvent& event) noexcept { return _TryEnqueueSyntheticMidi({event}); }
		bool TryEnqueueSourceLossRelease(const MidiEvent& event, std::uint32_t revision) noexcept
		{ return _TryEnqueueSyntheticMidi({event, nullptr, true, revision}); }
		void ResetLiveMidiWhileQuiescent() noexcept;
		std::uint32_t LiveSustainRevision(unsigned int channel) const noexcept
		{ return _liveSustainRevisions[channel].load(std::memory_order_acquire); }
		std::uint32_t LiveNoteOnRevision(std::size_t slot) const noexcept
		{ return _liveNoteOnRevisions[slot].load(std::memory_order_acquire); }
		void QueueLostPlaybackMidi(const std::shared_ptr<engine::LoopTake>& take, bool emitRelease = true);
		void FlushPendingLostPlaybackMidi();
		void Collect(); // Serialized structural owner only.
		bool HasUnconsumedRemovedBinding() const noexcept;
		void ClearAfterReadersStopped();

	protected:
		class MidiPlaybackSink final : public midi::IMidiOutputSink
		{
		public:
			explicit MidiPlaybackSink(const PreparedState& state) noexcept : _state(state) {}
			void OnEvent(unsigned int outputIndex, const midi::MidiEvent& event) noexcept override;
			bool TryOnEvent(unsigned int outputIndex, const midi::MidiEvent& event) noexcept override;
		private:
			const PreparedState& _state;
		};

		static bool _TryDeliverMidi(MidiDestination& destination, const midi::MidiEvent& event, bool isRealtime, MidiPlaybackBinding* owner = nullptr) noexcept;
		static bool _TryDeliverMidiNow(MidiDestination& destination, const midi::MidiEvent& event, bool isRealtime) noexcept;
		static bool _TryDeliverRecordedMidiNow(MidiDestination& destination, const midi::MidiEvent& event,
			MidiPlaybackBinding& owner, bool isRealtime) noexcept;
		static void _DropRecordedAudibleOwner(MidiDestination& destination, MidiPlaybackBinding& owner,
			bool emitRelease = true, std::uint16_t channels = 0xffffu) noexcept;
		static void _DrainScheduledMidi(const PreparedState& state, std::uint32_t blockStartSample, unsigned int numSamps) noexcept;
		static std::bitset<midi::MidiNote::TotalNoteSlots> _CancelScheduledMidiForOwner(
			MidiDestination& destination, MidiPlaybackBinding* owner) noexcept;
		static void _RecoverCancelledScheduledMidi(MidiDestination& destination,
			const std::bitset<midi::MidiNote::TotalNoteSlots>& affected) noexcept;
		static void _InvalidateScheduledMidiAtBlockStart(const PreparedState& state, bool forceCancel = false) noexcept;
		static void _DrainPendingMidiReleases(const PreparedState& state, std::uint32_t sample) noexcept;
		static void _RecoverSkippedMidiBlocks(const PreparedState& state) noexcept;
		static void _DispatchLiveMidi(const PreparedState& state, const midi::MidiEvent& event,
			bool sourceLossNoteOff, std::uint32_t liveNoteOnRevision) noexcept;
		static bool _ReleaseLostPlaybackMidi(MidiPlaybackBinding& binding, std::uint32_t sample,
			unsigned int& budget, unsigned int& slotBudget) noexcept;
		bool _TryEnqueueSyntheticMidi(SyntheticMidi entry) noexcept;

		static bool _TryEnqueueOrderedLiveMidi(MidiQueue<1024>& queue,
			bool& hasLastSample, std::uint32_t& lastSample, const MidiEvent& event) noexcept;

	private:
		midi::MidiQueue<1024> _immediateLiveMidiIngress;
		midi::MidiQueue<1024, SyntheticMidi> _syntheticLiveMidiIngress;
		std::vector<std::weak_ptr<MidiDestination>> _midiDestinations;
		std::vector<std::shared_ptr<MidiPlaybackBinding>> _midiPlaybackBindings;
		std::vector<std::shared_ptr<MidiPlaybackBinding>> _pendingLostPlaybackMidi;
		bool _hasLastImmediateLiveMidiSample = false;
		std::uint32_t _lastImmediateLiveMidiSample = 0u;
		bool _hasLastSyntheticLiveMidiSample = false;
		std::uint32_t _lastSyntheticLiveMidiSample = 0u;
		// Audio publishes delivered attacks; the job owner snapshots revisions for
		// source-loss releases. Rejected ingress never reserves ownership.
		std::array<std::atomic<std::uint32_t>, 16> _liveSustainRevisions{};
		std::array<std::atomic<std::uint32_t>, midi::MidiNote::TotalNoteSlots> _liveNoteOnRevisions{};
	};
}
