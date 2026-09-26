#include "RigCoordinator.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace engine;

bool RigCoordinator::_Equivalent(const io::RigFile::TriggerPair& lhs,
	const io::RigFile::TriggerPair& rhs) noexcept
{
	return lhs.ActivateDown == rhs.ActivateDown && lhs.ActivateUp == rhs.ActivateUp &&
		lhs.DitchDown == rhs.DitchDown && lhs.DitchUp == rhs.DitchUp &&
		lhs.Source == rhs.Source && lhs.Device == rhs.Device;
}

bool RigCoordinator::_Equivalent(const io::RigFile::Trigger::MidiTriggerBindingSpec& lhs,
	const io::RigFile::Trigger::MidiTriggerBindingSpec& rhs) noexcept
{
	return lhs.Kind == rhs.Kind && lhs.Channel == rhs.Channel && lhs.Id == rhs.Id &&
		lhs.State == rhs.State && lhs.MatchAnyChannel == rhs.MatchAnyChannel;
}

bool RigCoordinator::_Equivalent(const io::RigFile::Trigger::MidiTriggerBinding& lhs,
	const io::RigFile::Trigger::MidiTriggerBinding& rhs) noexcept
{
	return lhs.Device == rhs.Device && _Equivalent(lhs.Activate, rhs.Activate) &&
		_Equivalent(lhs.Ditch, rhs.Ditch);
}

bool RigCoordinator::_Equivalent(const io::RigFile::Trigger& lhs,
	const io::RigFile::Trigger& rhs) noexcept
{
	return lhs.Name == rhs.Name && lhs.StationType == rhs.StationType &&
		lhs.InputChannels == rhs.InputChannels && lhs.MidiInputDevices == rhs.MidiInputDevices &&
		lhs.StationTarget == rhs.StationTarget && lhs.MidiInputs == rhs.MidiInputs &&
		lhs.MidiTrigger.has_value() == rhs.MidiTrigger.has_value() &&
		(!lhs.MidiTrigger.has_value() || _Equivalent(lhs.MidiTrigger.value(), rhs.MidiTrigger.value())) &&
		lhs.TriggerPairs.size() == rhs.TriggerPairs.size() &&
		std::equal(lhs.TriggerPairs.begin(), lhs.TriggerPairs.end(), rhs.TriggerPairs.begin(),
			[](const auto& left, const auto& right) { return _Equivalent(left, right); });
}

bool RigCoordinator::_EquivalentActivation(const io::RigFile::Trigger& lhs,
	const io::RigFile::Trigger& rhs) noexcept
{
	return lhs.Name == rhs.Name && lhs.StationType == rhs.StationType &&
		lhs.MidiTrigger.has_value() == rhs.MidiTrigger.has_value() &&
		(!lhs.MidiTrigger.has_value() || _Equivalent(lhs.MidiTrigger.value(), rhs.MidiTrigger.value())) &&
		lhs.TriggerPairs.size() == rhs.TriggerPairs.size() &&
		std::equal(lhs.TriggerPairs.begin(), lhs.TriggerPairs.end(), rhs.TriggerPairs.begin(),
			[](const auto& left, const auto& right) { return _Equivalent(left, right); });
}

bool RigCoordinator::_EquivalentCaptureRouting(const io::RigFile::Trigger& lhs,
	const io::RigFile::Trigger& rhs) noexcept
{
	return lhs.InputChannels == rhs.InputChannels && lhs.MidiInputDevices == rhs.MidiInputDevices &&
		lhs.MidiInputs == rhs.MidiInputs;
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
	const SnapshotPtr& acceptedSnapshot)
{
	auto resolution = io::RigFileRouting::Resolve(rig, stationDescriptors,
		availableAdcChannels, availableMidiDevices);
	if (!resolution.IsValid) return {};
	const auto& normalizedRig = resolution.CandidateRig;
	auto snapshot = std::make_shared<RigSnapshot>();
	snapshot->Revision = revision;
	snapshot->Rig = normalizedRig;
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
				stationIndex != acceptedSnapshot->Triggers[acceptedIndex].StationIndex)
				snapshot->RetainedTriggerRouteChanges.push_back({ instance, triggerIndex });
		}
		else
		{
			auto trigger = Trigger::FromFile(triggerParams, fileTrigger);
			if (!trigger.has_value()) return {};
			instance = std::move(trigger.value());
			if (hasAcceptedIdentity)
			{
				retainedAcceptedIds.insert(fileTrigger.Id);
				snapshot->RetiredTriggerChecks.push_back(
					{ acceptedSnapshot->Triggers[acceptedIndex].Instance });
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
		if (stationIndex.has_value() && fileTrigger.MidiTrigger.has_value())
		{
			const auto& name = fileTrigger.MidiTrigger->Device;
			snapshot->InputDispatch.MidiTriggers.push_back({ name.empty() ? "default" : name, instance });
		}
		auto overdubMixer = std::make_shared<audio::AudioMixer>(
			Trigger::GetOverdubMixerParams(fileTrigger.InputChannels));
		auto overdubWriter = Trigger::CreateBounceWriter(overdubMixer);
		snapshot->Triggers.push_back({ fileTrigger.Id, triggerIndex, instance, stationIndex, std::move(receiver),
			fileTrigger.InputChannels, fileTrigger.MidiInputDevices, fileTrigger.MidiInputs,
			std::move(overdubMixer), std::move(overdubWriter) });
	}
	if (acceptedSnapshot)
	{
		for (const auto& acceptedTrigger : acceptedSnapshot->Triggers)
		{
			if (!retainedAcceptedIds.contains(acceptedTrigger.Id))
				snapshot->RetiredTriggerChecks.push_back({ acceptedTrigger.Instance });
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
						trigger.MidiInputMode == io::RigFile::Trigger::MidiInputMode::Selected &&
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
	_availableAdcChannels = availableAdcChannels;
	_availableMidiDevices = availableMidiDevices;
	_triggerParams = triggerParams;
	auto resolution = io::RigFileRouting::Resolve(rig, stationDescriptors, availableAdcChannels, availableMidiDevices);
	if (!resolution.IsValid) return false;
	if (resolution.RequiresSave && persistMigration)
		persistMigration(resolution.CandidateRig);
	auto snapshot = _BuildSnapshot(_AllocateRevision(), rig, stationDescriptors, stations,
		availableAdcChannels, availableMidiDevices, triggerParams, {});
	if (!snapshot) return false;
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
	const auto accepted = Accepted();
	auto snapshot = _BuildSnapshot(revision, candidateRig, _stationDescriptors, _stations,
		_availableAdcChannels, _availableMidiDevices, _triggerParams, accepted);
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
