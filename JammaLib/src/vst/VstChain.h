///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include "IVstPlugin.h"
#include "../../include/Constants.h"

namespace vst
{
	// VstChain holds an ordered sequence of IVstPlugin instances (VST2 or VST3)
	// and applies them in series to mono, stereo, or exact-match multichannel buffers.
	//
	// Threading contract:
	//   AddPlugin / RemovePlugin – call from a non-RT thread only.
	//   ProcessBlock / BeginMidiBlock / SendMidiEvent* – call from the audio callback (real-time safe).
	//   IsActive – safe to call from any thread.
	class VstChain
	{
	public:
		VstChain();
		~VstChain() = default;

		// Not copyable
		VstChain(const VstChain&) = delete;
		VstChain& operator=(const VstChain&) = delete;

	public:
		// Add a plugin to the end of the chain.  Not RT-safe.
		void AddPlugin(std::shared_ptr<IVstPlugin> plugin);

		// Remove the plugin at the given index.  Not RT-safe.
		void RemovePlugin(size_t index);

		// Returns the plugin at index, or nullptr if out of range.
		std::shared_ptr<IVstPlugin> GetPlugin(size_t index) const;

		size_t NumPlugins() const noexcept { return _plugins.size(); }

		// Returns true if there is at least one loaded, non-bypassed plugin.
		// Real-time safe.
		bool IsActive() const noexcept;

		// Apply all active plugins in series to numSamps of mono audio.
		// monoBuf is modified in-place.
		// Real-time safe: no heap allocation.
		void ProcessBlock(float* monoBuf, int numSamps) noexcept;

		// Apply all active plugins in series to numSamps of stereo audio.
		// leftBuf/rightBuf are modified in-place.
		void ProcessBlockStereo(float* leftBuf, float* rightBuf, int numSamps) noexcept;

		// Apply all active plugins in series to numSamps of an exact-match
		// multichannel bus. channelBufs are modified in-place.
		void ProcessBlockMulti(float* const* channelBufs, int numChannels, int numSamps) noexcept;

		void BeginMidiBlock(std::uint32_t blockStartSample, std::uint32_t numSamples) noexcept;
		void SendMidiEvent(const midi::MidiEvent& event, bool isRealtime) noexcept;
		// Direct indexed delivery for routing snapshots; avoids GetPlugin's shared_ptr copy on the RT path.
		void SendMidiEventToPlugin(size_t index, const midi::MidiEvent& event, bool isRealtime) noexcept;

		// Propagate host transport/tempo context to all plugins before the block.
		// Real-time safe.
		void UpdateHostTime(const HostTimeState& state) noexcept;

	private:
		std::vector<std::shared_ptr<IVstPlugin>> _plugins;
	};
}
