#include "RigCoordinator.h"

#include <algorithm>

using namespace engine;

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
	const TriggerParams& triggerParams)
{
	auto resolution = io::RigFileRouting::Resolve(rig, stationDescriptors,
		availableAdcChannels, availableMidiDevices);
	auto snapshot = std::make_shared<RigSnapshot>();
	snapshot->Revision = revision;
	snapshot->Rig = rig;
	snapshot->Graph = { revision, std::move(resolution.Triggers) };
	snapshot->InputDispatch.Revision = revision;
	snapshot->StationMemberships.reserve(stations.size());
	std::vector<std::shared_ptr<Station::TriggerMembership>> memberships;
	memberships.reserve(stations.size());
	for (const auto& station : stations)
	{
		auto membership = std::make_shared<Station::TriggerMembership>();
		memberships.push_back(membership);
		snapshot->StationMemberships.push_back({ station, membership });
	}

	for (size_t triggerIndex = 0u; triggerIndex < rig.Triggers.size(); ++triggerIndex)
	{
		auto trigger = Trigger::FromFile(triggerParams, rig.Triggers[triggerIndex]);
		if (!trigger.has_value()) return {};
		std::optional<size_t> stationIndex;
		if (triggerIndex < snapshot->Graph.Triggers.size())
			stationIndex = snapshot->Graph.Triggers[triggerIndex].StationIndex;
		if (stationIndex.has_value() && stationIndex.value() >= stations.size()) return {};

		auto instance = std::move(trigger.value());
		if (stationIndex.has_value())
		{
			const auto& station = stations[stationIndex.value()];
			if (!station) return {};
			instance->SetReceiver(station);
			memberships[stationIndex.value()]->push_back(instance);
			snapshot->InputDispatch.SerialTriggers.push_back(instance);
			snapshot->InputDispatch.KeyboardTriggers.push_back(instance);
		}
		const auto& fileTrigger = rig.Triggers[triggerIndex];
		if (stationIndex.has_value() && fileTrigger.MidiTrigger.has_value())
		{
			const auto& name = fileTrigger.MidiTrigger->Device;
			snapshot->InputDispatch.MidiTriggers.push_back({ name.empty() ? "default" : name, instance });
		}
		snapshot->Triggers.push_back({ triggerIndex, instance, stationIndex });
	}

	for (const auto& device : availableMidiDevices)
	{
		LiveMidiDispatch route{ device, {} };
		for (size_t stationIndex = 0u; stationIndex < stations.size(); ++stationIndex)
		{
			const auto& station = stations[stationIndex];
			if (!station || station->IsRemote()) continue;
			const auto accepts = std::any_of(memberships[stationIndex]->begin(), memberships[stationIndex]->end(),
				[&device](const std::shared_ptr<Trigger>& trigger) {
					if (!trigger) return false;
					const auto mode = trigger->MidiInputMode();
					if (mode != io::RigFile::Trigger::MidiInputMode::Selected) return false;
					const auto& devices = trigger->MidiInputDevices();
					return std::find(devices.begin(), devices.end(), device) != devices.end();
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
	_availableAdcChannels = availableAdcChannels;
	_availableMidiDevices = availableMidiDevices;
	_triggerParams = triggerParams;
	auto resolution = io::RigFileRouting::Resolve(rig, stationDescriptors, availableAdcChannels, availableMidiDevices);
	const auto savedMigration = resolution.RequiresSave && persistMigration && persistMigration(resolution.CandidateRig);
	const auto& acceptedRig = savedMigration ? resolution.CandidateRig : rig;
	auto snapshot = _BuildSnapshot(_AllocateRevision(), acceptedRig, stationDescriptors, stations,
		availableAdcChannels, availableMidiDevices, triggerParams);
	if (!snapshot) return false;
	for (const auto& membership : snapshot->StationMemberships)
		if (membership.Station) membership.Station->PublishTriggerMembership(membership.Triggers);
	_accepted.store(snapshot, std::memory_order_release);
	_audioAcknowledgement.store(snapshot->Revision, std::memory_order_release);
	_inputAcknowledgement.store(snapshot->Revision, std::memory_order_release);
	_editsEnabled.store(true, std::memory_order_release);
	return true;
}

RigCoordinator::EditResult RigCoordinator::SubmitCandidate(const io::RigFile& candidateRig)
{
	bool enabled = true;
	if (!_editsEnabled.compare_exchange_strong(enabled, false, std::memory_order_acq_rel)) return EditResult::EditsDisabled;
	const auto revision = _AllocateRevision();
	if (_shuttingDown.load(std::memory_order_acquire))
	{
		_editsEnabled.store(false, std::memory_order_release);
		return EditResult::QuiescenceRejected;
	}
	auto snapshot = _BuildSnapshot(revision, candidateRig, _stationDescriptors, _stations,
		_availableAdcChannels, _availableMidiDevices, _triggerParams);
	if (!snapshot) { _editsEnabled.store(true, std::memory_order_release); return EditResult::ValidationFailed; }
	_quiescing.store(snapshot, std::memory_order_release);
	return EditResult::Pending;
}

RigCoordinator::EditResult RigCoordinator::CompleteQuiescence(std::uint64_t revision,
	bool acceptedAtAudioBoundary,
	const PersistRig& persistRig)
{
	auto candidate = _quiescing.load(std::memory_order_acquire);
	if (!candidate || candidate->Revision != revision)
		return EditResult::QuiescenceRejected;
	if (!acceptedAtAudioBoundary || _shuttingDown.load(std::memory_order_acquire))
	{
		_quiescing.store({}, std::memory_order_release);
		_editsEnabled.store(!_shuttingDown.load(std::memory_order_acquire), std::memory_order_release);
		return EditResult::QuiescenceRejected;
	}
	// Persistence remains the final fallible operation before pending publication.
	if (!persistRig || !persistRig(candidate->Rig))
	{
		_quiescing.store({}, std::memory_order_release);
		_editsEnabled.store(!_shuttingDown.load(std::memory_order_acquire), std::memory_order_release);
		return EditResult::PersistenceFailed;
	}
	_pending.store(candidate, std::memory_order_release);
	_quiescing.store({}, std::memory_order_release);
	return EditResult::Pending;
}

RigCoordinator::SnapshotPtr RigCoordinator::Accepted() const noexcept { return _accepted.load(std::memory_order_acquire); }
RigCoordinator::SnapshotPtr RigCoordinator::Quiescing() const noexcept { return _quiescing.load(std::memory_order_acquire); }
RigCoordinator::SnapshotPtr RigCoordinator::Pending() const noexcept { return _pending.load(std::memory_order_acquire); }
bool RigCoordinator::EditsEnabled() const noexcept { return _editsEnabled.load(std::memory_order_acquire); }
std::uint64_t RigCoordinator::AudioAcknowledgement() const noexcept { return _audioAcknowledgement.load(std::memory_order_acquire); }
std::uint64_t RigCoordinator::InputAcknowledgement() const noexcept { return _inputAcknowledgement.load(std::memory_order_acquire); }

RigCoordinator::SnapshotPtr RigCoordinator::ApplyPendingAtAudioBoundary() noexcept
{
	auto pending = _pending.load(std::memory_order_acquire);
	if (!pending || AudioAcknowledgement() == pending->Revision) return pending;
	for (const auto& membership : pending->StationMemberships)
		if (membership.Station) membership.Station->PublishTriggerMembership(membership.Triggers);
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
	auto old = _accepted.exchange(pending, std::memory_order_acq_rel);
	_pending.store({}, std::memory_order_release);
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
	_pending.store({}, std::memory_order_release);
	_quiescing.store({}, std::memory_order_release);
	_accepted.store({}, std::memory_order_release);
	std::scoped_lock lock(_retiringMutex);
	_retiring.clear();
	_stations.clear();
	_stationDescriptors.clear();
	_availableMidiDevices.clear();
}
