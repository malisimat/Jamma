#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "VstChain.h"
#include "RackMove.h"
#include "VstRackSlot.h"
#include "../actions/JobAction.h"
#include "../actions/ActionResult.h"
#include "../io/JamFile.h"

namespace vst
{
	// Non-audio rack metadata, queued identities and chain staging. The caller
	// publishes prepared audio topology and owns routing consequences/UI lifetime.
	class RackState
	{
	public:
		static constexpr std::size_t MaxRackPlugins = 32u;
		static constexpr std::size_t MaxRackDestinations = 64u;
		// Copied under the caller's serialized structural operation; never retained.
		struct Format
		{
			float SampleRate;
			unsigned int BlockSize, Channels;
			std::uint64_t MaxChildLatency;
		};
		std::shared_ptr<VstChain> ActiveChain() const noexcept { return _vstChain.load(std::memory_order_acquire); }
		const std::shared_ptr<VstChain>& StagedChain() const noexcept { return _backVstChain; }
		bool CommitChain();
		bool MoveSlot(const RackMove& move, const std::string& ownerId);
		std::pair<std::vector<actions::JobAction>, std::vector<actions::JobAction>> TakePendingJobs();
		bool LoadVstPlugin(std::wstring path, std::vector<std::uint8_t> initialState, std::size_t retiredDestinations);
		VstRackSlot GetVstRackSlot() const;
		std::vector<VstRackSlot> GetVstRackSlots(std::size_t offset = 0u, std::size_t count = MaxRackPlugins) const;
		bool LoadManagedVstSlot(std::wstring path, const std::string& retryIdentity, bool discardOnFailure, std::size_t retiredDestinations);
		std::optional<std::wstring> TakeFailedInteractiveVstLoad(const std::string& identity);
		bool UnloadManagedVstPlugin(const std::string& identity);
		void UnloadVstPlugin(std::size_t index);
		std::shared_ptr<IVstPlugin> GetVstPlugin(std::size_t index) const;
		std::size_t RackSlotCount() const;
		std::string RackDiagnostic() const;
		std::optional<std::vector<int>> SetMidiChannelRoutes(std::array<midi::MidiChannelRoute,16> routes,
			std::uint64_t revision, const std::vector<int>& allowedMidiChannels);
		bool ClearChannelRoutes(std::uint16_t removedMask);
		const std::array<midi::MidiChannelRoute,16>& ChannelRoutes() const { return _midiChannelRoutes; }
		std::uint64_t Revision() const noexcept { return _rackRevision.load(); }
		bool Inactive() const noexcept { return _rackInactive; }
		bool LatencySuspended() const noexcept { return _rackLatencySuspended; }
		static bool LatencyFeasible(const VstChain& chain, std::uint64_t maxChildLatency) noexcept;
		bool UpdateLatencyFeasibility(bool feasible, bool canResume);
		bool RemoveInactiveRackSlot(std::size_t ordinal, std::uint64_t revision);
		bool ClearInactiveRackRoutes(std::uint64_t revision);
		bool RecoverRack(std::uint64_t revision, std::size_t retiredDestinations);
		bool LoadVstPluginSynchronously(const std::wstring& path, const std::vector<std::uint8_t>& initialState,
			bool bypass, const std::string& stateId, const Format& format, const std::string& ownerName,
			const std::function<void()>& chainReady);
		std::vector<io::JamFile::VstEntry> VstEntries() const;
		bool TryGroupVstEntries(std::vector<io::JamFile::VstEntry>& output) const;
		bool TryRestoreGroupVstEntry(const io::JamFile::VstEntry& entry);
		actions::ActionResult OnAction(actions::JobAction action, const Format& format, const std::string& ownerName,
			std::size_t retiredDestinations);
		void RestoreSaved(const std::vector<io::JamFile::VstEntry>& entries, std::array<midi::MidiChannelRoute,16> routes, bool inactive);
		static void RetireChain(std::shared_ptr<VstChain> chain);
		void RetireChainsOnUiThread() const;
		void ClearAfterReadersStopped();

	private:
		// VST insert chain applied after all LoopTakes are mixed down,
		// just before each channel is sent to the output AudioMixer.
		// Published atomically for lock-free audio-thread reads.
		std::atomic<std::shared_ptr<vst::VstChain>> _vstChain;
		std::shared_ptr<vst::VstChain> _backVstChain;
		std::atomic<bool> _flipVstChain{ false };

		// Pending load/unload requests staged by LoadVstPlugin / UnloadVstPlugin.
		// Read only on the job thread (from OnAction(JobAction)).
		// Each entry carries the plugin path and an optional VST2 initial-state blob.
		std::vector<actions::JobAction> _pendingVstLoads;
		std::vector<actions::JobAction> _pendingVstUnloads;
		// _vstPluginPaths: written on job thread (OnAction), read on main thread (VstEntries).
		// Access is guarded by _vstPathsMutex in both directions.
		mutable std::mutex _vstPathsMutex;
		std::vector<std::wstring> _vstPluginPaths;
		std::vector<io::JamFile::VstEntry> _vstSavedEntries;
		// Job-side removals are applied to live cables by CommitChanges before
		// publishing the matching replacement chain and MIDI destinations.
		std::vector<std::string> _pendingDeletedVstRouteIds; // _vstPathsMutex.
		// Serialized structural owner writes; UI/export reads under Scene's owner
		// lock. Audio sees only prepared lists/policy in its immutable snapshot.
		std::array<midi::MidiChannelRoute, 16> _midiChannelRoutes{};
		std::atomic<std::uint64_t> _rackRevision{1u}; // UI/job metadata publication; audio never reads.
		std::vector<std::string> _pendingRackIds; // _vstPathsMutex; completion clears by identity.
		std::vector<std::pair<std::string, std::wstring>> _failedInteractiveVstLoads; // Bounded UI notices, _vstPathsMutex.
		bool _rackInactive = false; // Reconstruction/structural owner; raw saved entries remain exportable.
		bool _rackLatencySuspended = false; // Structural owner; callback sees prepared empty delivery.
		std::string _rackDiagnostic; // Structural owner/UI under existing Scene lock.
		// UI stages requests, job completes them. Audio never reads this counter.
		std::atomic<unsigned int> _vstJobsPending{ 0u };
	};
}
