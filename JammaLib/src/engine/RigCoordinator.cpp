#include "RigCoordinator.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace engine;

bool RigCoordinator::_EquivalentActivation(const io::RigFile::Trigger& lhs,
	const io::RigFile::Trigger& rhs) noexcept
{
	// Names are presentation. Control-section order has already been resolved
	// by normalization into these independent, conflict-free lists.
	return lhs.PreDelayMs == rhs.PreDelayMs &&
		lhs.ControlBindings.Activate == rhs.ControlBindings.Activate &&
		lhs.ControlBindings.Ditch == rhs.ControlBindings.Ditch;
}

bool RigCoordinator::_EquivalentCaptureRouting(const io::RigFile::Trigger& lhs,
	const io::RigFile::Trigger& rhs) noexcept
{
	return lhs.CaptureInputs == rhs.CaptureInputs;
}

std::uint64_t RigCoordinator::_AllocateRevision() noexcept
{
	return _nextRevision.fetch_add(1u, std::memory_order_relaxed);
}

RigCoordinator::SnapshotPtr RigCoordinator::_BuildSnapshot(std::uint64_t revision,
	const io::RigFile& rig,
	const std::vector<io::JamFile::Station>& stationDescriptors,
	const std::vector<std::shared_ptr<Station>>& stations,
	unsigned int availableAdcChannels,
	const std::vector<std::string>& availableMidiDevices,
	const TriggerParams& triggerParams,
	const SnapshotPtr& acceptedSnapshot,
	const std::vector<std::string>* availableSerialDevices)
{
	if (!availableSerialDevices && acceptedSnapshot && acceptedSnapshot->AvailableSerialDevices)
		availableSerialDevices = &*acceptedSnapshot->AvailableSerialDevices;
	auto resolution = io::RigFileRouting::Resolve(rig, stationDescriptors,
		availableAdcChannels, availableMidiDevices, availableSerialDevices);
	if (!resolution.IsValid) return {};
	const auto& normalizedRig = resolution.CandidateRig;
	auto snapshot = std::make_shared<RigSnapshot>();
	snapshot->Revision = revision;
	snapshot->TopologyRevision = acceptedSnapshot ? acceptedSnapshot->TopologyRevision : revision;
	snapshot->Rig = normalizedRig;
	snapshot->Stations = std::make_shared<const std::vector<std::shared_ptr<Station>>>(stations);
	snapshot->Diagnostics = std::move(resolution.Diagnostics);
	snapshot->AvailableAdcChannels = availableAdcChannels;
	snapshot->AvailableMidiDevices = availableMidiDevices;
	if (availableSerialDevices) snapshot->AvailableSerialDevices = *availableSerialDevices;
	snapshot->Graph = { revision, std::move(resolution.Triggers) };
	snapshot->InputDispatch.Revision = revision;
	std::unordered_map<std::string, size_t> acceptedById;
	if (acceptedSnapshot)
	{
		acceptedById.reserve(acceptedSnapshot->Triggers.size());
		for (size_t acceptedIndex = 0u; acceptedIndex < acceptedSnapshot->Triggers.size(); ++acceptedIndex)
			acceptedById.emplace(acceptedSnapshot->Triggers[acceptedIndex].Id, acceptedIndex);
	}
	std::unordered_set<std::string> retainedAcceptedIds;
	retainedAcceptedIds.reserve(normalizedRig.Triggers.size());

	for (size_t triggerIndex = 0u; triggerIndex < normalizedRig.Triggers.size(); ++triggerIndex)
	{
		std::optional<size_t> stationIndex;
		if (triggerIndex < snapshot->Graph.Triggers.size())
			stationIndex = snapshot->Graph.Triggers[triggerIndex].StationIndex;
		if (stationIndex.has_value() && stationIndex.value() >= stations.size()) return {};

		const auto& fileTrigger = normalizedRig.Triggers[triggerIndex];
		const auto acceptedFound = acceptedById.find(fileTrigger.Id);
		const bool hasAcceptedIdentity = acceptedFound != acceptedById.end();
		const auto acceptedIndex = hasAcceptedIdentity ? acceptedFound->second : 0u;
		const bool canReuse = hasAcceptedIdentity &&
			acceptedIndex < acceptedSnapshot->Rig.Triggers.size() &&
			_EquivalentActivation(fileTrigger, acceptedSnapshot->Rig.Triggers[acceptedIndex]);
		std::shared_ptr<Trigger> instance;
		if (canReuse)
		{
			instance = acceptedSnapshot->Triggers[acceptedIndex].Instance;
			retainedAcceptedIds.insert(fileTrigger.Id);
			if (!_EquivalentCaptureRouting(fileTrigger, acceptedSnapshot->Rig.Triggers[acceptedIndex]) ||
				(stationIndex ? stations[*stationIndex].get() : nullptr) != acceptedSnapshot->Triggers[acceptedIndex].BoundReceiver.get())
				snapshot->TriggerRouteUpdates.push_back({ instance, triggerIndex,
					(stationIndex ? stations[*stationIndex].get() : nullptr) != acceptedSnapshot->Triggers[acceptedIndex].BoundReceiver.get() });
		}
		else
		{
			auto trigger = Trigger::FromFile(triggerParams, fileTrigger);
			if (!trigger.has_value()) return {};
			instance = std::move(trigger.value());
			if (hasAcceptedIdentity)
			{
				retainedAcceptedIds.insert(fileTrigger.Id);
				snapshot->TriggerReplacementChecks.push_back(
					{ acceptedSnapshot->Triggers[acceptedIndex].Instance, instance });
			}
		}
		std::shared_ptr<base::ActionReceiver> receiver;
		if (stationIndex.has_value())
		{
			const auto& station = stations[stationIndex.value()];
			if (!station) return {};
			receiver = station;
			if (!canReuse)
				instance->SetReceiver(receiver);
			snapshot->InputDispatch.SerialTriggers.push_back(instance);
			snapshot->InputDispatch.KeyboardTriggers.push_back(instance);
		}
		if (stationIndex.has_value())
		{
			std::vector<std::string> routedDevices;
			for (const auto* bindings : { &fileTrigger.ControlBindings.Activate, &fileTrigger.ControlBindings.Ditch })
				for (const auto& binding : *bindings)
				{
					if (binding.Source != io::RigFile::BindingSource::Midi ||
						std::find(routedDevices.begin(), routedDevices.end(), binding.Device) != routedDevices.end())
						continue;
					routedDevices.push_back(binding.Device);
					snapshot->InputDispatch.MidiTriggers.push_back({ binding.Device, instance });
				}
		}
		auto overdubMixer = std::make_shared<audio::AudioMixer>(
			Trigger::GetOverdubMixerParams(fileTrigger.CaptureInputs.Audio));
		auto overdubWriter = Trigger::CreateBounceWriter(overdubMixer);
		snapshot->Triggers.push_back({ fileTrigger.Id, triggerIndex, instance, stationIndex, std::make_shared<const std::string>(fileTrigger.Name), receiver,
			fileTrigger.CaptureInputs.Audio, fileTrigger.CaptureInputs.Midi,
			std::move(overdubMixer), std::move(overdubWriter), receiver });
	}
	if (acceptedSnapshot)
	{
		for (const auto& acceptedTrigger : acceptedSnapshot->Triggers)
		{
			if (!retainedAcceptedIds.contains(acceptedTrigger.Id))
				snapshot->TriggerReplacementChecks.push_back({ acceptedTrigger.Instance });
		}
	}

	for (const auto& device : availableMidiDevices)
	{
		LiveMidiDispatch route{ device, {} };
		for (size_t stationIndex = 0u; stationIndex < stations.size(); ++stationIndex)
		{
			const auto& station = stations[stationIndex];
			if (!station || station->IsRemote()) continue;
			const auto accepts = std::any_of(snapshot->Triggers.begin(), snapshot->Triggers.end(),
				[stationIndex, &device](const RigSnapshotTrigger& trigger) {
					return trigger.StationIndex == stationIndex &&
						std::find(trigger.MidiInputDevices.begin(), trigger.MidiInputDevices.end(), device) !=
							trigger.MidiInputDevices.end();
				});
			if (accepts) route.Recipients.push_back(station);
		}
		snapshot->InputDispatch.LiveMidi.push_back(std::move(route));
	}
	return snapshot;
}

bool RigCoordinator::BuildInitial(const io::RigFile& rig,
	const std::vector<io::JamFile::Station>& stationDescriptors,
	const std::vector<std::shared_ptr<Station>>& stations,
	unsigned int availableAdcChannels,
	const std::vector<std::string>& availableMidiDevices,
	const TriggerParams& triggerParams,
	const PersistRig& persistMigration)
{
	if (_accepted.load(std::memory_order_acquire) || _shuttingDown.load(std::memory_order_acquire)) return false;
	_stationDescriptors = stationDescriptors;
	_stations = stations;
	_triggerParams = triggerParams;
	auto resolution = io::RigFileRouting::Resolve(rig, stationDescriptors, availableAdcChannels, availableMidiDevices);
	if (!resolution.IsValid) return false;
	// Startup normalization is kept in memory. Selected rig files must
	// remain byte-identical until the user explicitly changes rig settings.
	(void)persistMigration;
	auto snapshot = _BuildSnapshot(_AllocateRevision(), rig, stationDescriptors, stations,
		availableAdcChannels, availableMidiDevices, triggerParams, {});
	if (!snapshot) return false;
	_accepted.store(snapshot, std::memory_order_release);
	_audioAcknowledgement.store(snapshot->Revision, std::memory_order_release);
	_inputAcknowledgement.store(snapshot->Revision, std::memory_order_release);
	_editsEnabled.store(true, std::memory_order_release);
	return true;
}

RigCoordinator::SnapshotPtr RigCoordinator::RefreshRuntimeAvailability(unsigned int availableAdcChannels,
	std::vector<std::string> availableMidiDevices,
	std::optional<std::vector<std::string>> availableSerialDevices,
	std::optional<io::UserConfig::AudioSettings> confirmedGeneratedAudio)
{
	if (_shuttingDown.load(std::memory_order_acquire) || !EditsEnabled()) return {};
	const auto accepted = Accepted();
	if (!accepted) return {};
	auto rig = accepted->Rig;
	// Only the first-run caller supplies preferences confirmed by an already
	// opened stream. This acknowledges device setup; it does not apply edits.
	if (confirmedGeneratedAudio) rig.User.Audio = *confirmedGeneratedAudio;
	const auto* serial = availableSerialDevices ? &*availableSerialDevices : nullptr;
	auto snapshot = _BuildSnapshot(_AllocateRevision(), rig, _stationDescriptors, _stations,
		availableAdcChannels, availableMidiDevices, _triggerParams, accepted, serial);
	if (!snapshot) return {};
	_accepted.store(snapshot, std::memory_order_release);
	return snapshot;
}

RigCoordinator::EditResult RigCoordinator::SubmitCandidate(const io::RigFile& candidateRig, EditReceiptPtr* receipt)
{
	if (receipt) receipt->reset();
	bool enabled = true;
	if (!_editsEnabled.compare_exchange_strong(enabled, false, std::memory_order_acq_rel)) return EditResult::EditsDisabled;
	const auto revision = _AllocateRevision();
	if (_shuttingDown.load(std::memory_order_acquire))
	{
		_editsEnabled.store(false, std::memory_order_release);
		return EditResult::TransitionRejected;
	}
	const auto accepted = Accepted();
	// Device setup and defaults belong to the scene lifecycle. A routing edit
	// cannot claim these settings were applied to the running scene.
	if (!accepted || !io::RigFile::Validate(candidateRig))
	{
		_editsEnabled.store(true, std::memory_order_release);
		return EditResult::ValidationFailed;
	}
	auto lifecycleSettings = candidateRig.User;
	lifecycleSettings.Loop.MidiSeamMergeWindowMs = accepted->Rig.User.Loop.MidiSeamMergeWindowMs;
	if (lifecycleSettings != accepted->Rig.User)
	{
		_editsEnabled.store(true, std::memory_order_release);
		return EditResult::ReinitialisationRequired;
	}
	auto snapshot = _BuildSnapshot(revision, candidateRig, _stationDescriptors, _stations,
		accepted->AvailableAdcChannels, accepted->AvailableMidiDevices, _triggerParams, accepted);
	if (!snapshot) { _editsEnabled.store(true, std::memory_order_release); return EditResult::ValidationFailed; }
	if (receipt)
	{
		_editReceipt = std::make_shared<EditReceipt>(revision);
		*receipt = _editReceipt;
	}
	_staged.store(snapshot, std::memory_order_release);
	return EditResult::Pending;
}

RigCoordinator::EditResult RigCoordinator::SubmitStationRename(
	const std::shared_ptr<Station>& station, std::uint64_t expectedRevision, const std::string& name)
{
	const auto accepted = Accepted();
	const auto found = std::find(_stations.begin(), _stations.end(), station);
	if (!accepted || accepted->Revision != expectedRevision || !station || station->IsRemote() ||
		found == _stations.end() || name.empty() || name.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
		name.find_first_not_of(" \t") == std::string::npos)
		return EditResult::ValidationFailed;
	if (station->Name() == name) return EditResult::ValidationFailed;
	for (const auto& other : _stations)
		if (other != station && other->Name() == name) return EditResult::ValidationFailed;
	auto rig = accepted->Rig;
	for (size_t i = 0; i < rig.Triggers.size(); ++i)
	{
		if (accepted->Triggers[i].BoundReceiver == station) rig.Triggers[i].StationTarget = name;
		else if (!accepted->Triggers[i].BoundReceiver && rig.Triggers[i].StationTarget == name)
			return EditResult::ValidationFailed;
	}
	// Reuse rig validation for the exact UTF-8 name contract, even in an empty rig.
	auto validation = rig;
	io::RigFile::Trigger probe;
	probe.Id = "station-name-validation";
	probe.Name = name;
	validation.Triggers = { probe };
	if (!io::RigFile::Validate(validation)) return EditResult::ValidationFailed;
	bool enabled = true;
	if (!_editsEnabled.compare_exchange_strong(enabled, false, std::memory_order_acq_rel))
		return EditResult::EditsDisabled;
	if (_shuttingDown.load(std::memory_order_acquire)) return EditResult::TransitionRejected;
	auto descriptors = _stationDescriptors;
	descriptors[static_cast<size_t>(found - _stations.begin())].Name = name;
	auto snapshot = _BuildSnapshot(_AllocateRevision(), rig, descriptors, _stations,
		accepted->AvailableAdcChannels, accepted->AvailableMidiDevices, _triggerParams, accepted);
	if (!snapshot) { _editsEnabled.store(true, std::memory_order_release); return EditResult::ValidationFailed; }
	for (size_t i = 0; i < snapshot->Triggers.size(); ++i)
		if (snapshot->Triggers[i].BoundReceiver != accepted->Triggers[i].BoundReceiver)
		{ _editsEnabled.store(true, std::memory_order_release); return EditResult::ValidationFailed; }
	auto prepared = std::const_pointer_cast<RigSnapshot>(snapshot);
	prepared->RenamedStation = station;
	prepared->StationName = name;
	prepared->StationDescriptors = std::move(descriptors);
	_staged.store(std::move(snapshot), std::memory_order_release);
	return EditResult::Pending;
}

RigCoordinator::EditResult RigCoordinator::SubmitInventory(
	const std::vector<std::shared_ptr<Station>>& stations, std::shared_ptr<Station> removed)
{
	bool enabled = true;
	if (!_editsEnabled.compare_exchange_strong(enabled, false, std::memory_order_acq_rel)) return EditResult::EditsDisabled;
	const auto accepted = Accepted();
	if (!accepted || _shuttingDown.load(std::memory_order_acquire))
	{ _editsEnabled.store(false, std::memory_order_release); return EditResult::TransitionRejected; }
	auto rig = accepted->Rig;
	std::vector<io::JamFile::Station> descriptors;
	for (const auto& station : stations) { io::JamFile::Station descriptor; descriptor.Name = station->Name(); descriptors.push_back(std::move(descriptor)); }
	for (size_t i = 0; i < rig.Triggers.size(); ++i)
	{
		const auto& route = accepted->Triggers[i];
		const auto receiver = route.BoundReceiver;
		const auto survivor = std::find(stations.begin(), stations.end(), receiver);
		// Preserve the accepted Station identity through membership/name edits.
		if (receiver)
			rig.Triggers[i].StationTarget = survivor == stations.end() ? std::nullopt : std::optional((*survivor)->Name());
		else if (!rig.Triggers[i].StationTarget || std::count_if(stations.begin(), stations.end(),
			[&](const auto& station) { return station->Name() == *rig.Triggers[i].StationTarget; }) == 1)
			rig.Triggers[i].StationTarget.reset();
	}
	auto snapshot = _BuildSnapshot(_AllocateRevision(), rig, descriptors, stations,
		accepted->AvailableAdcChannels, accepted->AvailableMidiDevices, _triggerParams, accepted);
	if (!snapshot) { _editsEnabled.store(true, std::memory_order_release); return EditResult::ValidationFailed; }
	for (size_t i = 0u; i < snapshot->Triggers.size(); ++i)
	{
		const auto receiver = accepted->Triggers[i].BoundReceiver;
		const auto expected = std::find(stations.begin(), stations.end(), receiver) == stations.end() ? nullptr : receiver;
		if (snapshot->Triggers[i].BoundReceiver != expected)
		{ _editsEnabled.store(true, std::memory_order_release); return EditResult::ValidationFailed; }
	}
	auto prepared = std::const_pointer_cast<RigSnapshot>(snapshot);
	prepared->MembershipChange = true;
	prepared->TopologyRevision = snapshot->Revision;
	prepared->StationDescriptors = std::move(descriptors);
	prepared->RemovedStation = removed.get();
	_staged.store(std::move(snapshot), std::memory_order_release);
	return EditResult::Pending;
}

RigCoordinator::EditResult RigCoordinator::CompleteTransition(std::uint64_t revision,
	bool acceptedAtAudioBoundary,
	const PersistRig& persistRig)
{
	auto candidate = _staged.load(std::memory_order_acquire);
	if (!candidate || candidate->Revision != revision)
		return EditResult::TransitionRejected;
	if (!acceptedAtAudioBoundary || _shuttingDown.load(std::memory_order_acquire))
	{
		_CompleteEditReceipt(revision, EditOutcome::Rejected);
		_staged.store({}, std::memory_order_release);
		_editsEnabled.store(!_shuttingDown.load(std::memory_order_acquire), std::memory_order_release);
		return EditResult::TransitionRejected;
	}
	// Performance edits are committed in memory; only explicit saving writes a rig.
	(void)persistRig;
	// The old trigger is idle at the accepted audio boundary. Restore its latest
	// published history before the replacement can be exposed to audio or input.
	for (const auto& replacement : candidate->TriggerReplacementChecks)
		if (replacement.ReplacementInstance)
		{
			auto history = replacement.AcceptedInstance->GetTakes();
			std::vector<std::pair<std::shared_ptr<base::TriggerPunchTarget>,
				std::shared_ptr<base::TriggerPunchTarget>>> pins;
			for (const auto& entry : history)
			{
				const auto station = std::dynamic_pointer_cast<Station>(entry.Receiver);
				const auto targets = station ? station->GetLoopTakeSnapshot() :
					std::vector<std::shared_ptr<LoopTake>>{};
				const auto resolve = [&targets](const std::string& id) -> std::shared_ptr<base::TriggerPunchTarget> {
					const auto found = std::find_if(targets.begin(), targets.end(),
						[&id](const auto& take) { return take && take->Id() == id; });
					return found == targets.end() ? nullptr : *found;
				};
				pins.emplace_back(resolve(entry.SourceTakeId), resolve(entry.TargetTakeId));
			}
			replacement.ReplacementInstance->RestoreTakes(std::move(history), std::move(pins));
		}
	// Job/UI-only immutable presentation update; callback never loads Name().
	for (const auto& runtime : candidate->Triggers)
		if (runtime.Instance && runtime.RigTriggerIndex < candidate->Rig.Triggers.size())
			runtime.Instance->SetName(runtime.DisplayName);
	_pending.store(candidate, std::memory_order_release);
	_staged.store({}, std::memory_order_release);
	return EditResult::Pending;
}

RigCoordinator::SnapshotPtr RigCoordinator::Accepted() const noexcept { return _accepted.load(std::memory_order_acquire); }
RigCoordinator::SnapshotPtr RigCoordinator::Staged() const noexcept { return _staged.load(std::memory_order_acquire); }
RigCoordinator::SnapshotPtr RigCoordinator::Pending() const noexcept { return _pending.load(std::memory_order_acquire); }
bool RigCoordinator::EditsEnabled() const noexcept { return _editsEnabled.load(std::memory_order_acquire); }
std::uint64_t RigCoordinator::AudioAcknowledgement() const noexcept { return _audioAcknowledgement.load(std::memory_order_acquire); }
std::uint64_t RigCoordinator::InputAcknowledgement() const noexcept { return _inputAcknowledgement.load(std::memory_order_acquire); }

RigCoordinator::SnapshotPtr RigCoordinator::ApplyPendingAtAudioBoundary() noexcept
{
	auto pending = _pending.load(std::memory_order_acquire);
	if (!pending || AudioAcknowledgement() == pending->Revision) return pending;
	_audioAcknowledgement.store(pending->Revision, std::memory_order_release);
	return pending;
}

bool RigCoordinator::ObserveAudioAcknowledgement(std::uint64_t revision) noexcept
{
	auto pending = _pending.load(std::memory_order_acquire);
	if (!pending || pending->Revision != revision)
		return false;
	_audioAcknowledgement.store(revision, std::memory_order_release);
	return true;
}

bool RigCoordinator::AcknowledgeInput(std::uint64_t revision) noexcept
{
	auto pending = _pending.load(std::memory_order_acquire);
	if (!pending || pending->Revision != revision || AudioAcknowledgement() != revision) return false;
	_inputAcknowledgement.store(revision, std::memory_order_release);
	return true;
}

bool RigCoordinator::PromoteAcknowledged()
{
	auto pending = _pending.load(std::memory_order_acquire);
	if (!pending || AudioAcknowledgement() != pending->Revision || InputAcknowledgement() != pending->Revision) return false;
	if (pending->MembershipChange)
	{
		_stations = *pending->Stations;
		_stationDescriptors = pending->StationDescriptors;
	}
	if (pending->RenamedStation)
	{
		pending->RenamedStation->SetName(pending->StationName);
		_stationDescriptors = pending->StationDescriptors;
	}
	auto old = _accepted.exchange(pending, std::memory_order_acq_rel);
	_pending.store({}, std::memory_order_release);
	_CompleteEditReceipt(pending->Revision, EditOutcome::Accepted);
	if (old)
	{
		std::scoped_lock lock(_retiringMutex);
		_retiring.push_back(std::move(old));
	}
	_editsEnabled.store(!_shuttingDown.load(std::memory_order_acquire), std::memory_order_release);
	return true;
}

void RigCoordinator::ReleaseRetired()
{
	std::scoped_lock lock(_retiringMutex);
	_retiring.clear();
}

void RigCoordinator::Shutdown() noexcept
{
	_shuttingDown.store(true, std::memory_order_release);
	_editsEnabled.store(false, std::memory_order_release);
}

void RigCoordinator::ReleaseAfterReadersStopped()
{
	if (_editReceipt) _CompleteEditReceipt(_editReceipt->Revision, EditOutcome::Rejected);
	_pending.store({}, std::memory_order_release);
	_staged.store({}, std::memory_order_release);
	_accepted.store({}, std::memory_order_release);
	std::scoped_lock lock(_retiringMutex);
	_retiring.clear();
	_stations.clear();
	_stationDescriptors.clear();
}

void RigCoordinator::_CompleteEditReceipt(std::uint64_t revision, EditOutcome outcome) noexcept
{
	if (!_editReceipt || _editReceipt->Revision != revision) return;
	_editReceipt->Outcome.store(outcome, std::memory_order_release);
	_editReceipt.reset();
}

bool RigCoordinator::MergeHardwarePreferences(const std::optional<io::UserConfig::AudioSettings>& audio,
	const std::optional<std::vector<io::UserConfig::MidiSettings>>& midi)
{
	// Caller excludes job and callback writers; merging the latest accepted value
	// preserves unrelated rig edits and does not replace Trigger identities.
	if (!EditsEnabled() || _shuttingDown.load(std::memory_order_acquire)) return false;
	const auto accepted = Accepted();
	if (!accepted) return false;
	auto rig = accepted->Rig;
	if (audio) rig.User.Audio = *audio;
	if (midi) rig.User.Midi.Devices = *midi;
	// Rebuild from immutable descriptors; never copy mutable route vectors that
	// a callback may still be swapping while consuming an older publication.
	auto updated = _BuildSnapshot(accepted->Revision, rig, _stationDescriptors, _stations,
		accepted->AvailableAdcChannels, accepted->AvailableMidiDevices, _triggerParams, accepted);
	if (!updated) return false;
	_accepted.store(updated, std::memory_order_release);
	std::scoped_lock lock(_retiringMutex);
	_retiring.push_back(accepted);
	return true;
}
