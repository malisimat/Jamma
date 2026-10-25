#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "../base/PublishedAudioSnapshot.h"

namespace engine { class LoopTake; }
namespace vst { class IVstPlugin; class VstChain; }

namespace midi
{
	class MidiLoop;

	// Prepared automation and callback cursors. The serialized structural owner
	// resolves targets and collects publications; audio only borrows and processes.
	class AutomationPlayback
	{
	public:
		void Rebuild(const std::vector<std::shared_ptr<engine::LoopTake>>& takes,
			const std::shared_ptr<vst::VstChain>& stationChain);
		void ProcessBlock(std::uint32_t blockStartSample, std::uint32_t numSamps) noexcept;
		void Collect() { _automationDispatch.collect(); } // Serialized non-audio owner.
		void ClearAfterReadersStopped();

	private:
		// --- Parameter automation dispatch ---

		// One pre-resolved automation mapping ready for the audio thread. All
		// pointer chasing and lane metadata are baked in by Rebuild
		// so the audio path is a flat loop over a hot cache line.
		// Immutable while an audio snapshot lease can reference it.
		struct AutomationDispatch
		{
			vst::IVstPlugin* plugin = nullptr;          // pinned by AutomationDispatchSnapshot
			unsigned int     paramIdx = 0u;
			midi::MidiLoop*                   loop = nullptr;            // pinned by AutomationDispatchSnapshot
			std::uint8_t                      laneIdx = 0u;              // which lane within loop to read
			std::uint32_t                     loopLengthSamps = 0u;      // frozen at rebuild
			std::uint32_t                     automationGlobalSampleOrigin = 0u; // frozen at rebuild; automation origin
			const std::atomic<std::int32_t>*  anchorCorrection = nullptr; // live correction from owning LoopTake
			// effective origin = automationGlobalSampleOrigin + anchorCorrection (modular uint32)
		};
		// Per-entry playback state owned exclusively by the audio thread.
		// Kept separate from immutable dispatch publication.
		struct AutomationPlaybackState
		{
			std::uint16_t cursorIdx = 0u;
			std::uint64_t stateGeneration = 0;
			float         lastValue = -2.0f;  // sentinel: force first write
		};
		static constexpr std::size_t MaxAutomationDispatches = 64u;
		struct AutomationDispatchSnapshot
		{
			std::uint64_t Generation = 0u;
			std::size_t Count = 0u;
			std::array<AutomationDispatch, MaxAutomationDispatches> Entries{};
			// Shared owners are touched only by the serialized non-audio publisher
			// and collector. A callback borrow protects all three lifetime boundaries.
			std::array<std::shared_ptr<vst::IVstPlugin>, MaxAutomationDispatches> Plugins;
			std::array<std::shared_ptr<engine::LoopTake>, MaxAutomationDispatches> Takes;
			std::array<std::shared_ptr<midi::MidiLoop>, MaxAutomationDispatches> Loops;
		};
		static constexpr float AutomationEpsilon = 1.0f / 65536.0f; // below 16-bit param resolution

		// Scene-serialized non-audio writers build/collect complete pinned lists;
		// callback reads only via bounded borrow. Shutdown stops readers, then
		// clears this publication before the existing UI plugin retirement drain.
		base::PublishedAudioSnapshot<const AutomationDispatchSnapshot> _automationDispatch;
		std::uint64_t _nextAutomationDispatchGeneration = 1u;
		// Audio-thread-only playback state, parallel to the active dispatch list.
		// Reset whenever ProcessBlock detects a new list was published.
		AutomationPlaybackState  _automationPlaybackState[MaxAutomationDispatches]{};
		// Callback-only generation: allocator address reuse cannot hide a rebuild.
		std::uint64_t _automationDispatchGeneration = 0u;

	};
}
