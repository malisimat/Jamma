#include "MidiVstOutputSink.h"

#include "../vst/VstChain.h"

namespace midi
{
	MidiVstOutputSink::MidiVstOutputSink(vst::VstChain* chain,
		const MidiVstRoutingSnapshot* routes) noexcept :
		_chain(chain),
		_routes(routes)
	{
	}

	void MidiVstOutputSink::OnEvent(unsigned int outputIndex, const MidiEvent& event) noexcept
	{
		SendMidiToVstChain(_chain, _routes, event, false, outputIndex);
	}

	void SendMidiToVstChain(vst::VstChain* chain,
		const MidiVstRoutingSnapshot* routes,
		const MidiEvent& event,
		bool isRealtime,
		unsigned int midiOutputIndex) noexcept
	{
		if (!chain)
			return;

		if (!routes || !routes->HasRoutes())
		{
			chain->SendMidiEvent(event, isRealtime);
			return;
		}

		const auto pluginIndex = routes->PluginForOutput(midiOutputIndex);
		if (pluginIndex != MidiVstRoutingSnapshot::NoPlugin && pluginIndex < chain->NumPlugins())
		{
			chain->SendMidiEventToPlugin(pluginIndex, event, isRealtime);
			return;
		}

		chain->SendMidiEvent(event, isRealtime);
	}
}