#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "MidiLoop.h"

namespace vst
{
	class VstChain;
}

namespace midi
{
	static constexpr unsigned int LiveMidiOutputIndex = ~0u;

	struct MidiVstRoutingSnapshot
	{
		static constexpr size_t NoPlugin = (std::numeric_limits<size_t>::max)();

		std::vector<size_t> PluginByMidiOutput;
		size_t LivePlugin = NoPlugin;

		bool HasRoutes() const noexcept
		{
			if (LivePlugin != NoPlugin)
				return true;
			for (const auto pluginIndex : PluginByMidiOutput)
			{
				if (pluginIndex != NoPlugin)
					return true;
			}
			return false;
		}

		size_t PluginForOutput(unsigned int midiOutputIndex) const noexcept
		{
			if (midiOutputIndex == LiveMidiOutputIndex)
				return LivePlugin;
			if (midiOutputIndex < PluginByMidiOutput.size())
				return PluginByMidiOutput[midiOutputIndex];
			return NoPlugin;
		}
	};

	void SendMidiToVstChain(vst::VstChain* chain,
		const MidiVstRoutingSnapshot* routes,
		const MidiEvent& event,
		bool isRealtime,
		unsigned int midiOutputIndex) noexcept;

	class MidiVstOutputSink final : public IMidiOutputSink
	{
	public:
		MidiVstOutputSink(vst::VstChain* chain,
			const MidiVstRoutingSnapshot* routes) noexcept;

		void OnEvent(unsigned int outputIndex, const MidiEvent& event) noexcept override;

	private:
		vst::VstChain* _chain;
		const MidiVstRoutingSnapshot* _routes;
	};
}