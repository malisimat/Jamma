// Legacy stub — no implementation needed; all VST3 logic is in Vst3Plugin.cpp and VstChain.cpp.

#include "IVstPlugin.h"

namespace vst
{
	// Definition of the global last-touched parameter registry declared in IVstPlugin.h.
	LastTouchedParameter _lastTouchedParam;

	void PublishLastTouchedParameter(IVstPlugin* plugin,
		unsigned int parameterIndex,
		float value) noexcept
	{
		_lastTouchedParam.Plugin.store(plugin, std::memory_order_relaxed);
		_lastTouchedParam.ParameterIndex.store(parameterIndex, std::memory_order_relaxed);
		_lastTouchedParam.Value.store(value, std::memory_order_relaxed);
		_lastTouchedParam.Sequence.fetch_add(1u, std::memory_order_release);
	}
}

