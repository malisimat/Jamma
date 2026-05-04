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
#include "VstPlugin.h"
#include "../../include/Constants.h"

namespace vst
{
	// VstChain holds an ordered sequence of VstPlugin instances and applies
	// them in series to a mono audio buffer.
	//
	// Threading contract:
	//   AddPlugin / RemovePlugin – call from a non-RT thread only.
	//   ProcessBlock – call from the audio callback (real-time safe).
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
		void AddPlugin(std::shared_ptr<VstPlugin> plugin);

		// Remove the plugin at the given index.  Not RT-safe.
		void RemovePlugin(size_t index);

		// Returns the plugin at index, or nullptr if out of range.
		std::shared_ptr<VstPlugin> GetPlugin(size_t index) const;

		size_t NumPlugins() const noexcept { return _plugins.size(); }

		// Returns true if there is at least one loaded, non-bypassed plugin.
		// Real-time safe.
		bool IsActive() const noexcept;

		// Apply all active plugins in series to numSamps of mono audio.
		// monoBuf is modified in-place.
		// Real-time safe: no heap allocation.
		void ProcessBlock(float* monoBuf, int numSamps) noexcept;

	private:
		std::vector<std::shared_ptr<VstPlugin>> _plugins;

		// Scratch buffer used as the working copy between plugin stages.
		// On struct (not heap) to avoid RT-path allocations.
		float _scratch[constants::MaxBlockSize];
	};
}
