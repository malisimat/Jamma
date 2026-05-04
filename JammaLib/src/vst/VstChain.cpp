///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "VstChain.h"
#include <algorithm>

using namespace vst;

VstChain::VstChain() :
	_plugins(),
	_scratch{}
{
}

void VstChain::AddPlugin(std::shared_ptr<VstPlugin> plugin)
{
	if (plugin)
		_plugins.push_back(std::move(plugin));
}

void VstChain::RemovePlugin(size_t index)
{
	if (index < _plugins.size())
		_plugins.erase(_plugins.begin() + index);
}

std::shared_ptr<VstPlugin> VstChain::GetPlugin(size_t index) const
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
			p->ProcessBlock(monoBuf, static_cast<int32_t>(numSamps));
	}
}
