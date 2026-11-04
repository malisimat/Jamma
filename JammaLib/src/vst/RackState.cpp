#include "RackState.h"
#include <algorithm>
#include <iostream>
#include "../io/NativeMidiSidecar.h"
#include "../utils/StringUtils.h"

using namespace vst;
using namespace actions;

void RackState::_StageRequestLocked(JobAction& action)
{
    action.VstRequestId=utils::GetGuid();
    _requests.push_back({action.VstRequestId,action.VstIdentity,action.JobActionType});
    _vstJobsPending.fetch_add(1u,std::memory_order_release);
}

bool RackState::_OwnsRequestLocked(const JobAction& action) const
{
    return !action.VstRequestId.empty() && std::any_of(_requests.begin(),_requests.end(),[&](const auto& request) {
        return request.Id==action.VstRequestId && request.Slot==action.VstIdentity && request.Type==action.JobActionType;
    });
}

void RackState::_FinishRequestLocked(const JobAction& action)
{
    if(!_OwnsRequestLocked(action)) return;
    std::erase_if(_requests,[&](const auto& request) { return request.Id==action.VstRequestId; });
    _vstJobsPending.fetch_sub(1u,std::memory_order_release);
}

bool RackState::StageCompatibleJob(JobAction& action)
{
    if(action.VstManaged || (action.JobActionType!=JobAction::JOB_LOADVST && action.JobActionType!=JobAction::JOB_UNLOADVST)) return false;
    std::lock_guard lock(_vstPathsMutex);
    if(_rackInactive || _requests.size()>=MaxRackDestinations || _OwnsRequestLocked(action)) return false;
    if(action.VstIdentity.empty()) action.VstIdentity=action.PreInitPlugin ? action.PreInitPlugin->StateId() : utils::GetGuid();
    if(action.VstIdentity.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(action.VstIdentity)) return false;
    _StageRequestLocked(action);
    return true;
}

void RackState::ResetAfterReadersStopped()
{
    RetireChain(_vstChain.exchange(nullptr)); RetireChain(std::move(_backVstChain));
    std::lock_guard lock(_vstPathsMutex);
    _pendingVstLoads.clear(); _pendingVstUnloads.clear(); _requests.clear(); _pendingRackIds.clear();
    _pendingDeletedVstRouteIds.clear(); _failedInteractiveVstLoads.clear();
    _vstPluginPaths.clear(); _vstSavedEntries.clear(); _midiChannelRoutes={};
    _vstJobsPending.store(0u,std::memory_order_release);
    _rackInactive=false; _rackLatencySuspended=false; _rackDiagnostic.clear(); ++_rackRevision;
    _backVstChain=std::make_shared<VstChain>(); _flipVstChain.store(true,std::memory_order_release);
}

bool RackState::LoadVstPlugin(std::wstring path, std::vector<std::uint8_t> initialState, std::size_t retiredDestinations)
{
	const auto before = _pendingVstLoads.size();
	const bool queued = LoadManagedVstSlot(std::move(path), {}, false, retiredDestinations);
	if (queued && _pendingVstLoads.size() > before)
	{
		_pendingVstLoads.back().VstInitialState = initialState;
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		_vstSavedEntries.back().State = io::JamFile::VstEntry::EncodeState(initialState);
	}
	return queued;
}

vst::VstRackSlot RackState::GetVstRackSlot() const
{
	vst::VstRackSlot slot;
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	slot.Pending = _vstJobsPending.load(std::memory_order_acquire) != 0u
		|| _flipVstChain.load(std::memory_order_acquire);
	if (_vstSavedEntries.empty()) return slot;
	const auto& entry = _vstSavedEntries.front();
	slot.Path = utils::DecodeUtf8(entry.Path);
	slot.Identity = entry.Id;
	const auto chain = _vstChain.load(std::memory_order_acquire);
	slot.Plugin = chain ? chain->GetPlugin(0u) : nullptr;
	slot.Failed = !slot.Plugin;
	return slot;
}

std::vector<vst::VstRackSlot> RackState::GetVstRackSlots(std::size_t offset, std::size_t count) const
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	std::vector<vst::VstRackSlot> result;
	for (std::size_t index = std::min(offset, _vstSavedEntries.size()), end = index + std::min({count, MaxRackPlugins, _vstSavedEntries.size() - index}); index < end; ++index)
	{
		const auto& entry = _vstSavedEntries[index];
		vst::VstRackSlot slot;
		slot.Identity = entry.Id;
		slot.Path = utils::DecodeUtf8(entry.Path);
		slot.Plugin = chain ? chain->GetPlugin(index) : nullptr;
		slot.Pending = std::find(_pendingRackIds.begin(), _pendingRackIds.end(), entry.Id) != _pendingRackIds.end();
		slot.Failed = !slot.Plugin && !slot.Pending;
		result.push_back(std::move(slot));
	}
	return result;
}

std::optional<std::wstring> RackState::TakeFailedInteractiveVstLoad(const std::string& identity)
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	const auto found = std::find_if(_failedInteractiveVstLoads.begin(), _failedInteractiveVstLoads.end(),
		[&](const auto& item) { return item.first == identity; });
	if (found == _failedInteractiveVstLoads.end()) return std::nullopt;
	auto path = std::move(found->second);
	_failedInteractiveVstLoads.erase(found);
	return path;
}

void RackState::UnloadVstPlugin(size_t index)
{
	JobAction job;
	job.JobActionType = JobAction::JOB_UNLOADVST;
	job.VstIndex = index;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		if (index >= _vstSavedEntries.size() || _requests.size()>=MaxRackDestinations) return;
        job.VstIdentity = _vstSavedEntries[index].Id;
        _StageRequestLocked(job);
	}
	_pendingVstUnloads.push_back(std::move(job));
}

std::shared_ptr<vst::IVstPlugin> RackState::GetVstPlugin(size_t index) const
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	if (!chain)
		return nullptr;

	return chain->GetPlugin(index);
}

std::size_t RackState::RackSlotCount() const
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	return _vstSavedEntries.size();
}

std::string RackState::RackDiagnostic() const { return _rackDiagnostic; }

std::vector<io::JamFile::VstEntry> RackState::VstEntries() const
{
	std::vector<io::JamFile::VstEntry> entries;

	auto chain = _vstChain.load(std::memory_order_acquire);
	std::vector<std::wstring> paths;
	std::vector<io::JamFile::VstEntry> savedEntries;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		paths = _vstPluginPaths;
		savedEntries = _vstSavedEntries;
		chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	}
	// Plugin state calls may pump native messages and reenter this owner.
	entries.reserve(paths.size());

	for (size_t i = 0; i < paths.size(); ++i)
	{
		io::JamFile::VstEntry entry = savedEntries[i];

		if (chain && i < chain->NumPlugins())
		{
			auto plugin = chain->GetPlugin(i);
			if (plugin)
			{
				entry.Id = plugin->StateId();
				entry.Bypass = plugin->IsBypassed();
				auto blob = plugin->GetState();
				if (!blob.empty())
					entry.State = io::JamFile::VstEntry::EncodeState(blob);
			}
		}

		entries.push_back(std::move(entry));
	}

	return entries;
}


bool RackState::TryGroupVstEntries(std::vector<io::JamFile::VstEntry>& output) const
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	std::vector<std::wstring> paths;
	std::vector<io::JamFile::VstEntry> savedEntries;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		paths = _vstPluginPaths;
		savedEntries = _vstSavedEntries;
		chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
	}
	// Plugin state calls may pump native messages and reenter this owner.
	std::vector<io::JamFile::VstEntry> entries;
	if (chain)
	{
		if (chain->NumPlugins() != paths.size()) return false;
		entries.reserve(chain->NumPlugins());
		for (size_t i = 0; i < chain->NumPlugins(); ++i)
		{
			auto plugin = chain->GetPlugin(i);
			if (!plugin) { entries.push_back(savedEntries[i]); continue; }
			std::vector<std::uint8_t> blob;
			if (!plugin->TryCaptureLiveState(blob)) return false;
			io::JamFile::VstEntry entry;
			entry.Id = plugin->StateId();
			entry.Path = utils::EncodeUtf8(paths[i]);
			entry.Bypass = plugin->IsBypassed();
			entry.State = io::JamFile::VstEntry::EncodeState(blob);
			entries.push_back(std::move(entry));
		}
	}
	output = std::move(entries);
	return true;
}

bool RackState::TryRestoreGroupVstEntry(const io::JamFile::VstEntry& entry)
{
	auto chain = _vstChain.load(std::memory_order_acquire);
	if (!chain || entry.Id.empty()) return true;
	for (size_t i = 0; i < chain->NumPlugins(); ++i)
	{
		auto plugin = chain->GetPlugin(i);
		if (!plugin || plugin->StateId() != entry.Id) continue;
		const auto blob = entry.DecodeState();
		if (!entry.State.empty() && blob.empty()) return true;
		if (!plugin->TryRestoreLiveState(blob)) return false;
		plugin->SetBypassed(entry.Bypass);
		return true;
	}
	return true; // Deleted plugins never match a replacement's position.
}

bool RackState::LoadManagedVstSlot(std::wstring path, const std::string& retryIdentity, bool discardOnFailure, std::size_t retiredDestinations)
{
	if (path.empty() || _rackInactive) return false;
	std::string identity = retryIdentity;
	std::vector<std::uint8_t> state;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		if (_requests.size()>=MaxRackDestinations || std::find(_pendingRackIds.begin(), _pendingRackIds.end(), identity) != _pendingRackIds.end()) return false;
		const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
		if (!identity.empty())
		{
			const auto found = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == identity; });
			if (found == _vstSavedEntries.end()) return false;
			const auto index = static_cast<std::size_t>(found - _vstSavedEntries.begin());
			if (chain && chain->GetPlugin(index)) return false;
			state = found->DecodeState();
		}
		else
		{
			if (_vstSavedEntries.size() >= MaxRackPlugins) { _rackDiagnostic = "Rack supports at most 32 plugins; remove a slot first."; return false; }
			if (_vstSavedEntries.size() + retiredDestinations >= MaxRackDestinations) { _rackDiagnostic = "Waiting for removed plugin release recovery; retry shortly."; return false; }
			identity = utils::GetGuid();
			io::JamFile::VstEntry entry;
			entry.Id = identity;
			entry.Path = utils::EncodeUtf8(path);
			_vstSavedEntries.push_back(std::move(entry));
			_vstPluginPaths.push_back(path);
		}
		_pendingRackIds.push_back(identity);
		++_rackRevision;
	}
	JobAction job;
	job.JobActionType = JobAction::JOB_LOADVST;
	job.VstPath = std::move(path);
	job.VstInitialState = std::move(state);
	job.VstIdentity = identity;
	job.VstManaged = true;
	job.VstDiscardOnFailure = discardOnFailure && retryIdentity.empty();
    { std::lock_guard lock(_vstPathsMutex); _StageRequestLocked(job); }
	_pendingVstLoads.push_back(std::move(job));
	return true;
}

bool RackState::UnloadManagedVstPlugin(const std::string& identity)
{
	std::size_t index;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		const auto found = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == identity; });
		if (identity.empty() || found == _vstSavedEntries.end()) return false;
		index = static_cast<std::size_t>(found - _vstSavedEntries.begin());
		// A load completing after this gesture sees the identity gone, even
		// while its SDK call is in flight. Its processor is retired on the UI.
		std::erase(_pendingRackIds, identity);
	}
	UnloadVstPlugin(index);
	return true;
}

std::optional<std::vector<int>> RackState::SetMidiChannelRoutes(std::array<midi::MidiChannelRoute, 16> routes, std::uint64_t revision, const std::vector<int>& allowedMidiChannels)
{
	if (revision != _rackRevision) { _rackDiagnostic = "Rack changed; retry the gesture."; return std::nullopt; }
	auto allowed = ValidateChannelRoutes(routes, _midiChannelRoutes, GetVstRackSlots(), allowedMidiChannels, _rackDiagnostic);
	if (!allowed) return std::nullopt;
	_midiChannelRoutes = std::move(routes);
	if (allowed->size() != allowedMidiChannels.size()) std::sort(allowed->begin(), allowed->end());
	++_rackRevision;
	if (!_rackInactive && !_rackLatencySuspended) _rackDiagnostic.clear();
	return allowed;
}

bool RackState::RemoveInactiveRackSlot(std::size_t ordinal, std::uint64_t revision)
{
	if (!_rackInactive || revision != _rackRevision) return false;
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	if (ordinal >= _vstSavedEntries.size()) return false;
	const auto deletedId = _vstSavedEntries[ordinal].Id;
	_vstSavedEntries.erase(_vstSavedEntries.begin() + ordinal);
	_vstPluginPaths.erase(_vstPluginPaths.begin() + ordinal);
	if (std::none_of(_vstSavedEntries.begin(), _vstSavedEntries.end(),
		[&](const auto& entry) { return entry.Id == deletedId; }))
		for (auto& route : _midiChannelRoutes) std::erase(route.PluginIds, deletedId);
	++_rackRevision;
	return true;
}

bool RackState::ClearInactiveRackRoutes(std::uint64_t revision)
{
	if (!_rackInactive || revision != _rackRevision) return false;
	_midiChannelRoutes = {};
	++_rackRevision;
	return true;
}

bool RackState::RecoverRack(std::uint64_t revision, std::size_t retiredDestinations)
{
	if (!_rackInactive || revision != _rackRevision || RackSlotCount() > MaxRackPlugins) return false;
	for (const auto& route : _midiChannelRoutes)
	{
		if (route.PluginIds.size() > MaxRackPlugins) return false;
		for (const auto& id : route.PluginIds)
			if (id.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(id)) { _rackDiagnostic = "Remove malformed cable identities before resuming."; return false; }
	}
	std::vector<io::JamFile::VstEntry> entries;
	{
		std::lock_guard<std::mutex> lock(_vstPathsMutex);
		std::vector<std::string> ambiguous;
		for (const auto& entry : _vstSavedEntries)
			if (std::count_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& other) { return entry.Id == other.Id; }) > 1) ambiguous.push_back(entry.Id);
		for (auto& entry : _vstSavedEntries)
			if (entry.Id.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(entry.Id) || std::find(ambiguous.begin(), ambiguous.end(), entry.Id) != ambiguous.end()) entry.Id = utils::GetGuid();
		entries = _vstSavedEntries;
	}
	_rackInactive = false;
	_rackDiagnostic.clear();
	++_rackRevision;
	for (const auto& entry : entries) LoadManagedVstSlot(utils::DecodeUtf8(entry.Path), entry.Id, false, retiredDestinations);
	return true;
}

bool RackState::LoadVstPluginSynchronously(const std::wstring& path, const std::vector<std::uint8_t>& initialState, bool bypass, const std::string& stateId, const Format& format, const std::string& ownerName, const std::function<void()>& chainReady)
{
	// Startup construction completes before Scene::InitAudio publishes this station.
	if (_rackInactive || _vstSavedEntries.size() >= MaxRackPlugins || (!stateId.empty() && (!io::NativeMidiSidecar::ValidatePluginIdentity(stateId)
		|| std::any_of(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == stateId; }))))
	{
		_rackDiagnostic = "Rack limit or invalid identity; saved data requires explicit recovery.";
		std::cerr << "[ERROR][VST] station='" << ownerName << "' path='" << utils::EncodeUtf8(path)
			<< "' id='" << stateId << "' stage=admission inactive=" << _rackInactive << " slots=" << _vstSavedEntries.size() << std::endl;
		return false;
	}
	auto plugin = vst::MakePluginForPath(path);
	auto hostChannels = format.Channels;
	if (hostChannels == 0u)
		hostChannels = 1u;
	std::clog << "[VST][restore] station='" << ownerName << "' path='" << utils::EncodeUtf8(path)
		<< "' id='" << stateId << "' stage=begin state-bytes=" << initialState.size()
		<< " rate=" << format.SampleRate << " block=" << format.BlockSize << " channels=" << hostChannels << std::endl;
	const bool preInit = plugin->PreInit(path);
	const bool activated = preInit && plugin->Load(path, format.SampleRate, format.BlockSize, hostChannels, vst::HostedLayoutMode::Exact);
	const char* failure = !preInit ? "preinit" : !activated ? "load" : plugin->Role() == vst::RenderRole::Unknown
		? "unsupported-role" : plugin->GetLatencySamples() > 262144 ? "latency-limit" : "none";
	bool loaded = preInit && activated && plugin->Role() != vst::RenderRole::Unknown && plugin->GetLatencySamples() <= 262144;

	plugin->SetStateId(stateId);
	if (loaded && !initialState.empty())
		plugin->SetState(initialState);
	plugin->SetBypassed(bypass);
	const auto identity = plugin->StateId();
	if (!loaded) { vst::QueueForUiThreadDestroy(std::move(plugin)); plugin.reset(); }

	auto chain = _vstChain.load(std::memory_order_acquire);
	auto replacement = std::make_shared<vst::VstChain>();
	if (chain)
	{
		for (std::size_t index = 0u; index < chain->NumPlugins(); ++index)
			replacement->AddSlot(chain->GetPlugin(index));
	}
	replacement->AddSlot(std::move(plugin));
	if (!LatencyFeasible(*replacement, format.MaxChildLatency))
	{
		failure = "rack-latency-budget";
		loaded = false;
		vst::QueueForUiThreadDestroy(replacement->GetPlugin(replacement->NumPlugins() - 1u));
		replacement->ReplaceSlot(replacement->NumPlugins() - 1u, nullptr);
	}
	_vstChain.store(std::move(replacement), std::memory_order_release);
	chainReady();
	// Startup still exclusively owns this station.
	_vstPluginPaths.push_back(path);
	io::JamFile::VstEntry saved;
	saved.Path = utils::EncodeUtf8(path);
	saved.Id = identity;
	saved.Bypass = bypass;
	saved.State = io::JamFile::VstEntry::EncodeState(initialState);
	_vstSavedEntries.push_back(std::move(saved));
	(loaded ? std::clog : std::cerr) << (loaded ? "[VST][restore]" : "[ERROR][VST][restore]")
		<< " station='" << ownerName << "' path='" << utils::EncodeUtf8(path) << "' id='" << identity
		<< "' stage=complete loaded=" << loaded << " reason=" << failure << " saved-slots=" << _vstSavedEntries.size()
		<< "; saved state retained" << std::endl;
	return loaded;
}

actions::ActionResult RackState::OnAction(actions::JobAction action, const Format& format, const std::string& ownerName, std::size_t retiredDestinations)
{
    if(action.JobActionType==JobAction::JOB_LOADVST || action.JobActionType==JobAction::JOB_UNLOADVST)
    {
        std::lock_guard lock(_vstPathsMutex);
        if(!_OwnsRequestLocked(action))
        {
            if(action.PreInitPlugin) QueueForUiThreadDestroy(std::move(action.PreInitPlugin));
            return ActionResult::NoAction();
        }
    }
    switch (action.JobActionType)
    {
    case JobAction::JOB_LOADVST:
	{
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			const bool overLimit = !action.VstManaged && (_vstSavedEntries.size() >= MaxRackPlugins || (_vstSavedEntries.size() + retiredDestinations >= MaxRackDestinations)
				|| (!action.VstIdentity.empty() && std::any_of(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == action.VstIdentity; })));
			const bool cancelled = action.VstManaged && std::find(_pendingRackIds.begin(), _pendingRackIds.end(), action.VstIdentity) == _pendingRackIds.end();
			if (_rackInactive || overLimit || cancelled || (!action.VstIdentity.empty() && !io::NativeMidiSidecar::ValidatePluginIdentity(action.VstIdentity)))
			{
                _FinishRequestLocked(action);
                if(action.PreInitPlugin) QueueForUiThreadDestroy(std::move(action.PreInitPlugin));
                _rackDiagnostic = cancelled ? "" : "Rack capacity or identity rejected; remove a slot and retry.";
				std::cerr << "[ERROR][VST][load] station='" << ownerName << "' path='" << utils::EncodeUtf8(action.VstPath)
					<< "' id='" << action.VstIdentity << "' stage=admission inactive=" << _rackInactive
					<< " over-limit=" << overLimit << " cancelled=" << cancelled << std::endl;
				return ActionResult::NoAction();
			}
		}
        std::shared_ptr<vst::IVstPlugin> plugin;
        bool activated=false,loaded=false;
        const auto hostChannels=std::max(1u,format.Channels);
        try
        {
        plugin = action.PreInitPlugin ? action.PreInitPlugin : vst::MakePluginForPath(action.VstPath);
        std::clog << "[VST][load] station='" << ownerName << "' path='" << utils::EncodeUtf8(action.VstPath)
			<< "' id='" << action.VstIdentity << "' stage=begin preinitialized=" << bool(action.PreInitPlugin)
			<< " state-bytes=" << action.VstInitialState.size() << std::endl;
		activated = plugin->Load(action.VstPath, format.SampleRate, format.BlockSize, hostChannels, vst::HostedLayoutMode::Exact);
		loaded = activated && plugin->Role() != vst::RenderRole::Unknown;
		if (action.VstIdentity.empty()) action.VstIdentity = plugin->StateId();
		plugin->SetStateId(action.VstIdentity);
		if (loaded && !action.VstInitialState.empty()) plugin->SetState(action.VstInitialState);
		if (action.VstManaged)
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			const auto found = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == action.VstIdentity; });
			if (found != _vstSavedEntries.end()) plugin->SetBypassed(found->Bypass);
		}
        loaded = loaded && plugin->GetLatencySamples() <= 262144;
        }
        catch(...)
        {
            if(plugin) QueueForUiThreadDestroy(std::move(plugin));
            std::lock_guard lock(_vstPathsMutex);
            if(_OwnsRequestLocked(action) && action.VstManaged) std::erase(_pendingRackIds,action.VstIdentity);
            _FinishRequestLocked(action);
            throw; // Preserve Scene's existing fatal job failure policy.
        }
        if (!loaded) std::cerr << "[ERROR][VST][load] station='" << ownerName << "' path='" << utils::EncodeUtf8(action.VstPath)
			<< "' id='" << action.VstIdentity << "' stage=" << (!activated ? "load" : plugin->Role() == vst::RenderRole::Unknown ? "unsupported-role" : "latency-limit")
			<< " latency=" << plugin->GetLatencySamples() << " rate=" << format.SampleRate << " block=" << format.BlockSize << " channels=" << hostChannels << std::endl;
		if (!loaded) { vst::QueueForUiThreadDestroy(std::move(plugin)); plugin.reset(); }
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
            if(!_OwnsRequestLocked(action))
            {
                if(plugin) QueueForUiThreadDestroy(std::move(plugin));
                return ActionResult::NoAction();
            }
            // Multiple serialized jobs can complete before the UI publishes. Compose
			// against the latest delivered chain so no earlier edit is discarded.
			const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
			auto replacement = std::make_shared<vst::VstChain>();
			if (chain) for (size_t i = 0u; i < chain->NumPlugins(); ++i) replacement->AddSlot(chain->GetPlugin(i));
			io::JamFile::VstEntry saved;
			saved.Id = action.VstIdentity;
			saved.Path = utils::EncodeUtf8(action.VstPath);
			saved.State = io::JamFile::VstEntry::EncodeState(action.VstInitialState);
			if (action.VstManaged)
			{
				const auto found = std::find_if(_vstSavedEntries.begin(), _vstSavedEntries.end(), [&](const auto& entry) { return entry.Id == action.VstIdentity; });
				const bool cancelled = std::find(_pendingRackIds.begin(), _pendingRackIds.end(), action.VstIdentity) == _pendingRackIds.end();
				if (found == _vstSavedEntries.end() || cancelled)
				{
					if (plugin) vst::QueueForUiThreadDestroy(std::move(plugin));
					_FinishRequestLocked(action);
					return ActionResult::NoAction();
				}
				const auto index = static_cast<std::size_t>(found - _vstSavedEntries.begin());
				while (replacement->NumPlugins() <= index) replacement->AddSlot(nullptr);
				replacement->ReplaceSlot(index, std::move(plugin));
				if (!LatencyFeasible(*replacement, format.MaxChildLatency))
				{
					loaded = false;
					vst::QueueForUiThreadDestroy(replacement->GetPlugin(index));
					replacement->ReplaceSlot(index, nullptr);
				}
				_vstPluginPaths[index] = action.VstPath;
				saved.Bypass = found->Bypass;
				// A failed retry must keep the original opaque state for recovery.
				if (!loaded) saved.State = found->State;
				*found = std::move(saved);
				std::erase(_pendingRackIds, action.VstIdentity);
				if (!loaded && action.VstDiscardOnFailure)
				{
					// Fresh picker appends are provisional. Saved/imported slots and retries
					// keep their opaque state; a failed new slot restores the prior rack.
					replacement->RemovePlugin(index);
					_vstPluginPaths.erase(_vstPluginPaths.begin() + static_cast<std::ptrdiff_t>(index));
					_vstSavedEntries.erase(_vstSavedEntries.begin() + static_cast<std::ptrdiff_t>(index));
					_pendingDeletedVstRouteIds.push_back(action.VstIdentity);
					if (_failedInteractiveVstLoads.size() == 32u) _failedInteractiveVstLoads.erase(_failedInteractiveVstLoads.begin());
					_failedInteractiveVstLoads.emplace_back(action.VstIdentity, action.VstPath);
				}
			}
			else
			{
				replacement->AddSlot(std::move(plugin));
				_vstPluginPaths.push_back(action.VstPath);
				_vstSavedEntries.push_back(std::move(saved));
				if (!LatencyFeasible(*replacement, format.MaxChildLatency))
				{
					loaded = false;
					vst::QueueForUiThreadDestroy(replacement->GetPlugin(replacement->NumPlugins() - 1u));
					replacement->ReplaceSlot(replacement->NumPlugins() - 1u, nullptr);
				}
			}
			if (!loaded && !action.VstDiscardOnFailure)
				_rackDiagnostic = "Plugin could not be activated: check supported role, layout and latency (262144 samples maximum). Saved state is retained.";
			_backVstChain = std::move(replacement);
			(loaded ? std::clog : std::cerr) << (loaded ? "[VST][load]" : "[ERROR][VST][load]")
				<< " station='" << ownerName << "' path='" << utils::EncodeUtf8(action.VstPath) << "' id='" << action.VstIdentity
				<< "' stage=complete loaded=" << loaded << " discard-on-failure=" << action.VstDiscardOnFailure
				<< " saved-slots=" << _vstSavedEntries.size() << std::endl;
			++_rackRevision;
			_flipVstChain.store(true, std::memory_order_release);
		}
		{ std::lock_guard lock(_vstPathsMutex); _FinishRequestLocked(action); }
		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	case JobAction::JOB_UNLOADVST:
	{
		{
			std::lock_guard<std::mutex> lock(_vstPathsMutex);
			const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : _vstChain.load(std::memory_order_acquire);
			// Identity is captured when requested. An index alone can drift while queued.
			auto removeIndex = _vstSavedEntries.size();
			for (size_t i = 0u; i < _vstSavedEntries.size(); ++i)
				if (!action.VstIdentity.empty() && _vstSavedEntries[i].Id == action.VstIdentity) { removeIndex = i; break; }
			if (removeIndex < _vstSavedEntries.size())
			{
				std::clog << "[VST][remove] station='" << ownerName << "' id='" << action.VstIdentity
					<< "' path='" << _vstSavedEntries[removeIndex].Path << "'" << std::endl;
				auto replacement = std::make_shared<vst::VstChain>();
				if (chain) for (size_t i = 0u; i < chain->NumPlugins(); ++i)
				{
					auto existing = chain->GetPlugin(i);
					if (i == removeIndex) { if (existing) vst::QueueForUiThreadDestroy(std::move(existing)); }
					else replacement->AddSlot(std::move(existing));
				}
				_vstPluginPaths.erase(_vstPluginPaths.begin() + static_cast<std::ptrdiff_t>(removeIndex));
				_vstSavedEntries.erase(_vstSavedEntries.begin() + static_cast<std::ptrdiff_t>(removeIndex));
				_pendingDeletedVstRouteIds.push_back(action.VstIdentity);
				++_rackRevision;
				_backVstChain = std::move(replacement);
				_flipVstChain.store(true, std::memory_order_release);
			}
		}
		{ std::lock_guard lock(_vstPathsMutex); _FinishRequestLocked(action); }
		ActionResult res;
		res.IsEaten = true;
		res.ResultType = actions::ACTIONRESULT_DEFAULT;
		return res;
	}
	default:
		break;
	}

	return ActionResult::NoAction();
}

bool RackState::LatencyFeasible(const VstChain& chain, std::uint64_t maxChildLatency) noexcept
{
	return chain.FitsCompensationCeiling()
		&& static_cast<std::uint64_t>(std::max(0, chain.GetLatencySamples())) + maxChildLatency <= 262144u;
}

bool RackState::UpdateLatencyFeasibility(bool feasible, bool canResume)
{
	if (_rackInactive) return false;
	if (!feasible && !_rackLatencySuspended)
	{
		_rackLatencySuspended = true;
		_rackDiagnostic = "Rack paused: plugin latency exceeds 262144 samples. Reduce latency to resume; state and cables are preserved.";
		++_rackRevision;
		return true;
	}
	if (feasible && _rackLatencySuspended && canResume)
	{
		_rackLatencySuspended = false;
		_rackDiagnostic.clear();
		++_rackRevision;
		return true;
	}
	return false;
}

bool RackState::ClearChannelRoutes(std::uint16_t removedMask)
{
	bool cleared = false;
	for (std::uint8_t channel = 0; channel < 16; ++channel)
	{
		if ((removedMask & (1u << channel)) == 0u) continue;
		auto& route = _midiChannelRoutes[channel];
		if (route.PluginIds.empty()) continue;
		route = {};
		cleared = true;
	}
	if (removedMask != 0u) ++_rackRevision;
	return cleared;
}

bool RackState::CommitChain()
{
	if (!_flipVstChain.load(std::memory_order_acquire)) return false;
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	for (auto& route : _midiChannelRoutes)
		for (const auto& identity : _pendingDeletedVstRouteIds) std::erase(route.PluginIds, identity);
	_pendingDeletedVstRouteIds.clear();
	_flipVstChain.store(false, std::memory_order_release);
	_vstChain.store(_backVstChain, std::memory_order_release);
	return true;
}

std::pair<std::vector<JobAction>, std::vector<JobAction>> RackState::TakePendingJobs()
{
	return {std::move(_pendingVstUnloads), std::move(_pendingVstLoads)};
}

void RackState::RestoreSaved(const std::vector<io::JamFile::VstEntry>& entries,
	std::array<midi::MidiChannelRoute, 16> routes, bool inactive)
{
	_midiChannelRoutes = std::move(routes);
	_rackInactive = inactive;
	if (!inactive) return;
	_rackDiagnostic = "Rack paused: remove excess/invalid entries or cables, then choose Resume rack. Saved data is preserved.";
	for (const auto& entry : entries)
	{
		_vstSavedEntries.push_back(entry);
		_vstPluginPaths.push_back(utils::DecodeUtf8(entry.Path));
	}
}

void RackState::RetireChain(std::shared_ptr<VstChain> chain)
{
	if (!chain) return;
	for (std::size_t index = 0; index < chain->NumPlugins(); ++index)
		if (auto plugin = chain->GetPlugin(index)) QueueForUiThreadDestroy(std::move(plugin));
	chain.reset();
}

void RackState::RetireChainsOnUiThread() const
{
	RetireChain(_vstChain.load(std::memory_order_acquire));
	RetireChain(_backVstChain);
}

void RackState::ClearAfterReadersStopped()
{
	RetireChain(_vstChain.exchange(nullptr, std::memory_order_acq_rel));
	RetireChain(std::move(_backVstChain));
	_flipVstChain.store(false, std::memory_order_release);
	_pendingVstLoads.clear();
	_pendingVstUnloads.clear();
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
    _requests.clear(); _pendingRackIds.clear();
	_vstPluginPaths.clear();
	_vstSavedEntries.clear();
	_failedInteractiveVstLoads.clear();
	_vstJobsPending.store(0u, std::memory_order_release);
}

// Structural/UI owner only. Surviving processors are shared with the old snapshot;
// AudioHost's callback admission is the exclusive processing owner for both orders.
bool RackState::MoveSlot(const RackMove& move, const std::string& ownerId)
{
	std::lock_guard<std::mutex> lock(_vstPathsMutex);
	if (_vstJobsPending.load(std::memory_order_acquire) != 0u
		|| !move.Valid(RackMove::Scope::Station, ownerId, Revision(), _vstSavedEntries)) return false;
	const auto chain = _flipVstChain.load(std::memory_order_acquire) ? _backVstChain : ActiveChain();
	auto replacement = move.Stage(chain, _vstSavedEntries, _vstPluginPaths);
	if (!replacement) return false;
	_backVstChain = std::move(replacement);
	++_rackRevision;
	_flipVstChain.store(true, std::memory_order_release);
	return true; // Explicit MIDI routes are identities and remain unchanged.
}

std::optional<std::vector<int>> RackState::ValidateChannelRoutes(
	std::array<midi::MidiChannelRoute,16>& routes,
	const std::array<midi::MidiChannelRoute,16>& prior,
	const std::vector<VstRackSlot>& slots, const std::vector<int>& allowedChannels,
	std::string& diagnostic)
{
	for (auto channel = 0u; channel < 16u; ++channel)
	{
		auto& route = routes[channel];
		std::sort(route.PluginIds.begin(), route.PluginIds.end());
		route.PluginIds.erase(std::unique(route.PluginIds.begin(), route.PluginIds.end()), route.PluginIds.end());
		if (route.PluginIds.size() > MaxRackPlugins) { diagnostic = "Each channel supports at most 32 destinations."; return std::nullopt; }
		for (const auto& id : route.PluginIds)
		{
			if (id.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(id)) { diagnostic = "Invalid plugin identity; remove or repair the cable."; return std::nullopt; }
			const auto& oldIds = prior[channel].PluginIds;
			if (std::find(oldIds.begin(), oldIds.end(), id) != oldIds.end()) continue;
			const auto found = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) { return slot.Identity == id; });
			if (found == slots.end() || !found->Plugin) { diagnostic = "Destination is unavailable; retry its load first."; return std::nullopt; }
			const auto capability = found->Plugin->MidiInput();
			if (!capability.Known || (!capability.Notes && !capability.Controllers)) { diagnostic = "This plugin has no supported MIDI input."; return std::nullopt; }
		}
	}
	std::vector<int> allowed = allowedChannels;
	for (auto channel = 0u; channel < 16u; ++channel)
	{
		const auto newlyConnected = std::any_of(routes[channel].PluginIds.begin(), routes[channel].PluginIds.end(), [&](const auto& id) {
			const auto& oldIds = prior[channel].PluginIds;
			return std::find(oldIds.begin(), oldIds.end(), id) == oldIds.end();
		});
		if (newlyConnected && std::find(allowed.begin(), allowed.end(), static_cast<int>(channel + 1u)) == allowed.end())
			allowed.push_back(static_cast<int>(channel + 1u));
	}
	std::sort(allowed.begin(), allowed.end());
	return allowed;
}

bool RackState::ValidSavedRouting(const std::vector<io::JamFile::VstEntry>& entries,
	const std::array<midi::MidiChannelRoute,16>& routes, bool allowMissingIdentities)
{
	if (entries.size() > MaxRackPlugins) return false;
	std::vector<std::string> ids;
	for (const auto& entry : entries)
	{
		// Import precedes collision-aware legacy identity migration in IoSessionLoader.
		if (allowMissingIdentities && entry.Id.empty()) continue;
		if (entry.Id.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(entry.Id)
			|| std::find(ids.begin(), ids.end(), entry.Id) != ids.end()) return false;
		ids.push_back(entry.Id);
	}
	for (const auto& route : routes)
	{
		if (route.PluginIds.size() > MaxRackPlugins) return false;
		ids.clear();
		for (const auto& id : route.PluginIds)
		{
			if (id.empty() || !io::NativeMidiSidecar::ValidatePluginIdentity(id)
				|| std::find(ids.begin(), ids.end(), id) != ids.end()) return false;
			ids.push_back(id);
		}
	}
	return true;
}
