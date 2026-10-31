///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstChain.h"
#include <algorithm>
#include <limits>

using namespace vst;

VstChain::VstChain() :
	_plugins()
{
}

void VstChain::AddPlugin(std::shared_ptr<IVstPlugin> plugin)
{
	if (plugin)
		_plugins.push_back(std::move(plugin));
}

void VstChain::RemovePlugin(size_t index)
{
	if (index < _plugins.size())
		_plugins.erase(_plugins.begin() + index);
}

std::shared_ptr<IVstPlugin> VstChain::GetPlugin(size_t index) const
{
	if (index < _plugins.size())
		return _plugins[index];

	return nullptr;
}

bool VstChain::IsActive() const noexcept
{
	for (const auto& p : _plugins)
	{
		if (p && p->IsLoaded() && !p->IsBypassed())
			return true;
	}

	return false;
}

int VstChain::GetLatencySamples() const noexcept
{
	int total = 0;
	for (const auto& plugin : _plugins)
	{
		if (!plugin || !plugin->IsLoaded() || plugin->IsBypassed()
			|| plugin->Role() == RenderRole::Instrument) continue;
		total += std::min(std::max(0, plugin->GetLatencySamples()), (std::numeric_limits<int>::max)() - total);
	}
	return total;
}

int VstChain::GetLatencySamplesFrom(std::size_t firstPlugin) const noexcept
{
	int total = 0;
	for (auto index = firstPlugin; index < _plugins.size(); ++index)
	{
		const auto& plugin = _plugins[index];
		if (!plugin || !plugin->IsLoaded() || plugin->IsBypassed()) continue;
		if (index != firstPlugin && plugin->Role() == RenderRole::Instrument) continue;
		const auto latency = std::max(0, plugin->GetLatencySamples());
		total += std::min(latency, (std::numeric_limits<int>::max)() - total);
	}
	return total;
}

void VstChain::ProcessRack(float* const* channels, float* const* scratch,
	int numChannels, int numSamples) noexcept
{
	if (!channels || !scratch || numChannels <= 0 || numSamples <= 0
		|| static_cast<unsigned int>(numSamples) > constants::MaxBlockSize) return;
	for (const auto& plugin : _plugins)
	{
		if (!plugin || !plugin->IsLoaded() || plugin->IsBypassed()) continue;
		IVstPlugin::PluginCallGuard call(*plugin, true);
		if (!call) continue;
		plugin->FlushPendingMidiPanic();
		if (plugin->Role() != RenderRole::Instrument)
		{
			plugin->ProcessBlockMulti(channels, numChannels, numSamples);
			continue;
		}
		for (int channel = 0; channel < numChannels; ++channel)
			std::fill_n(scratch[channel], numSamples, 0.0f);
		plugin->ProcessBlockMulti(scratch, numChannels, numSamples);
		for (int channel = 0; channel < numChannels; ++channel)
			for (int sample = 0; sample < numSamples; ++sample)
				channels[channel][sample] += scratch[channel][sample];
	}
}

void VstChain::ProcessRackMono(float* channel, float* scratch, int numSamples) noexcept
{
	if (!channel || !scratch || numSamples <= 0
		|| static_cast<unsigned int>(numSamples) > constants::MaxBlockSize) return;
	for (const auto& plugin : _plugins)
	{
		if (!plugin || !plugin->IsLoaded() || plugin->IsBypassed()) continue;
		IVstPlugin::PluginCallGuard call(*plugin, true);
		if (!call) continue;
		plugin->FlushPendingMidiPanic();
		if (plugin->Role() != RenderRole::Instrument)
		{
			plugin->ProcessBlock(channel, numSamples);
			continue;
		}
		std::fill_n(scratch, numSamples, 0.0f);
		plugin->ProcessBlock(scratch, numSamples);
		for (int sample = 0; sample < numSamples; ++sample) channel[sample] += scratch[sample];
	}
}

void VstChain::PollPendingChanges() const noexcept
{
	for (const auto& plugin : _plugins)
		if (plugin) plugin->PollPendingChanges();
}

void VstChain::ProcessBlock(float* monoBuf, int numSamps) noexcept
{
	if (_plugins.empty())
		return;

	// Number of samples must not exceed the pre-allocated scratch size.
	if (numSamps <= 0 || static_cast<unsigned int>(numSamps) > constants::MaxBlockSize)
		return;

	// Run each plugin in sequence.  Each plugin modifies monoBuf in-place
	// (or is a no-op if bypassed / unloaded).
	for (const auto& p : _plugins)
	{
		if (p)
		{
			IVstPlugin::PluginCallGuard call(*p, true);
			if (call) { p->FlushPendingMidiPanic(); p->ProcessBlock(monoBuf, static_cast<int32_t>(numSamps)); }
		}
	}
}

void VstChain::ProcessBlockStereo(float* leftBuf, float* rightBuf, int numSamps) noexcept
{
	if (_plugins.empty())
		return;

	if (numSamps <= 0 || static_cast<unsigned int>(numSamps) > constants::MaxBlockSize)
		return;

	for (const auto& p : _plugins)
	{
		if (p)
		{
			IVstPlugin::PluginCallGuard call(*p, true);
			if (call) { p->FlushPendingMidiPanic(); p->ProcessBlockStereo(leftBuf, rightBuf, static_cast<int32_t>(numSamps)); }
		}
	}
}

void VstChain::ProcessBlockMulti(float* const* channelBufs, int numChannels, int numSamps) noexcept
{
	if (_plugins.empty())
		return;

	if (!channelBufs || numChannels <= 0 || numSamps <= 0
		|| static_cast<unsigned int>(numSamps) > constants::MaxBlockSize)
		return;

	for (const auto& p : _plugins)
	{
		if (p)
		{
			IVstPlugin::PluginCallGuard call(*p, true);
			if (call) { p->FlushPendingMidiPanic(); p->ProcessBlockMulti(channelBufs, numChannels, static_cast<int32_t>(numSamps)); }
		}
	}
}

void VstChain::BeginMidiBlock(std::uint32_t blockStartSample, std::uint32_t numSamples) noexcept
{
	for (const auto& p : _plugins)
	{
		if (p)
		{
			IVstPlugin::PluginCallGuard call(*p, true);
			if (call) { p->BeginMidiBlock(blockStartSample, numSamples); p->FlushPendingMidiPanic(); }
		}
	}
}

void VstChain::UpdateHostTime(const HostTimeState& state) noexcept
{
	for (const auto& p : _plugins)
	{
		if (p)
		{
			IVstPlugin::PluginCallGuard call(*p);
			if (call) p->UpdateHostTime(state);
		}
	}
}

void VstChain::SendMidiEvent(const midi::MidiEvent& event, bool isRealtime) noexcept
{
	for (const auto& p : _plugins)
	{
		if (p)
		{
			IVstPlugin::PluginCallGuard call(*p, true);
			if (call) { p->FlushPendingMidiPanic(); p->SendMidiEvent(event, isRealtime); }
		}
	}
}

void VstChain::SendMidiEventToPlugin(size_t index, const midi::MidiEvent& event, bool isRealtime) noexcept
{
	if (index < _plugins.size() && _plugins[index])
	{
		IVstPlugin::PluginCallGuard call(*_plugins[index], true);
		if (call) { _plugins[index]->FlushPendingMidiPanic(); _plugins[index]->SendMidiEvent(event, isRealtime); }
	}
}
