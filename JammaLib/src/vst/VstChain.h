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
	// and applies them to mono, stereo, or exact-match multichannel buffers.
	//
	// Threading contract:
	//   AddPlugin / RemovePlugin – call from a non-RT thread only.
	//   ProcessBlock / BeginMidiBlock / SendMidiEvent* – call from the audio callback (real-time safe).
	//   IsActive – safe to call from any thread.
	class VstChain
	{
	public:
		// Scratch is owned by the caller's
		// callback state, so concurrent snapshot readers never share mutable buses.
		void ProcessRack(float* const* channels, float* const* instrumentScratch,
			int numChannels, int numSamples) noexcept;
		VstChain();
		~VstChain() = default;

		// Not copyable
		VstChain(const VstChain&) = delete;
		VstChain& operator=(const VstChain&) = delete;

	public:
		// Add a plugin to the end of the chain.  Not RT-safe.
		void AddPlugin(std::shared_ptr<IVstPlugin> plugin);
		// Preserve failed saved entries as silent slots so automation indices stay aligned.
		void AddSlot(std::shared_ptr<IVstPlugin> plugin) { _plugins.push_back(std::move(plugin)); }
		void ReplaceSlot(size_t index, std::shared_ptr<IVstPlugin> plugin) { if (index < _plugins.size()) _plugins[index] = std::move(plugin); }

		// Remove the plugin at the given index.  Not RT-safe.
		void RemovePlugin(size_t index);

		// Returns the plugin at index, or nullptr if out of range.
		std::shared_ptr<IVstPlugin> GetPlugin(size_t index) const;
		// Non-RT ownership lookup. A saved ID is authoritative; never dereference
		// an observer which may name a removed processor or reuse its old address.
		std::shared_ptr<IVstPlugin> FindPlugin(const IVstPlugin* observer,
			const std::string& identity, std::size_t& index) const
		{
			for (std::size_t i = 0u; i < _plugins.size(); ++i)
				if (_plugins[i] && (identity.empty() ? _plugins[i].get() == observer
					: _plugins[i]->StateId() == identity))
				{ index = i; return _plugins[i]; }
			return nullptr;
		}

		size_t NumPlugins() const noexcept { return _plugins.size(); }
		bool ReconfigureAudioFormat(float sampleRate, unsigned int blockSize)
		{
			for (const auto& plugin : _plugins)
				if (plugin && !plugin->ReconfigureAudioFormat(sampleRate, blockSize)) return false;
			return true;
		}

		// Returns true if plugin is one of the instances held by this chain.
		// Identity comparison only (never dereferences). Not RT-safe (walks the
		// plugin vector); call from a non-RT thread only.
		bool ContainsPlugin(const IVstPlugin* plugin) const noexcept
		{
			if (!plugin)
				return false;
			for (const auto& p : _plugins)
				if (p.get() == plugin)
					return true;
			return false;
		}

		// Returns true if there is at least one loaded, non-bypassed plugin.
		// Real-time safe.
		bool IsActive() const noexcept;

		// Sum of the rack's active effect latency. Bypassed/unloaded plugins
		// contribute no delay. Negative reports are ignored; sums saturate.
		int GetLatencySamples() const noexcept;
		// MIDI injected at a plugin incurs that plugin and following effects.
		int GetLatencySamplesFrom(std::size_t firstPlugin) const noexcept;
		bool FitsCompensationCeiling() const noexcept
		{
			if (GetLatencySamples() > 262144) return false;
			for (std::size_t index = 0; index < _plugins.size(); ++index)
				if (GetLatencySamplesFrom(index) > 262144) return false;
			return true;
		}
		// Job/UI pump, including plugins whose editors are closed. Never callback.
		void PollPendingChanges() const noexcept;

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
