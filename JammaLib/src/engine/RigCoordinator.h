#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "RigSnapshot.h"

namespace engine
{
	class Station;

	class RigCoordinator
	{
	public:
		using SnapshotPtr = std::shared_ptr<const RigSnapshot>;
		using PersistRig = std::function<bool(const io::RigFile&)>;
		enum class EditResult { Pending, EditsDisabled, AudioCallbackInactive, TriggerBusy, TransitionRejected, ValidationFailed, PersistenceFailed };

		RigCoordinator() = default;
		RigCoordinator(const RigCoordinator&) = delete;
		RigCoordinator& operator=(const RigCoordinator&) = delete;

		bool BuildInitial(const io::RigFile& rig,
			const std::vector<io::JamFile::Station>& stationDescriptors,
			const std::vector<std::shared_ptr<Station>>& stations,
			unsigned int availableAdcChannels,
			const std::vector<std::string>& availableMidiDevices,
			const TriggerParams& triggerParams,
			const PersistRig& persistMigration);
		EditResult SubmitCandidate(const io::RigFile& candidateRig);
		EditResult CompleteTransition(std::uint64_t revision,
			bool acceptedAtAudioBoundary,
			const PersistRig& persistRig);

		SnapshotPtr Accepted() const noexcept;
		SnapshotPtr Staged() const noexcept;
		SnapshotPtr Pending() const noexcept;
		bool EditsEnabled() const noexcept;
		std::uint64_t AudioAcknowledgement() const noexcept;
		std::uint64_t InputAcknowledgement() const noexcept;
		SnapshotPtr ApplyPendingAtAudioBoundary() noexcept;
		bool ObserveAudioAcknowledgement(std::uint64_t revision) noexcept;
		bool AcknowledgeInput(std::uint64_t revision) noexcept;
		bool PromoteAcknowledged();
		void ReleaseRetired();
		void Shutdown() noexcept;
		void ReleaseAfterReadersStopped();

	private:
		static bool _Equivalent(const io::RigFile::TriggerPair& lhs,
			const io::RigFile::TriggerPair& rhs) noexcept;
		static bool _Equivalent(const io::RigFile::Trigger::MidiTriggerBindingSpec& lhs,
			const io::RigFile::Trigger::MidiTriggerBindingSpec& rhs) noexcept;
		static bool _Equivalent(const io::RigFile::Trigger::MidiTriggerBinding& lhs,
			const io::RigFile::Trigger::MidiTriggerBinding& rhs) noexcept;
		static bool _Equivalent(const io::RigFile::Trigger& lhs,
			const io::RigFile::Trigger& rhs) noexcept;
		static bool _EquivalentActivation(const io::RigFile::Trigger& lhs,
			const io::RigFile::Trigger& rhs) noexcept;
		static bool _EquivalentCaptureRouting(const io::RigFile::Trigger& lhs,
			const io::RigFile::Trigger& rhs) noexcept;
		static SnapshotPtr _BuildSnapshot(std::uint64_t revision,
			const io::RigFile& rig,
			const std::vector<io::JamFile::Station>& stationDescriptors,
			const std::vector<std::shared_ptr<Station>>& stations,
			unsigned int availableAdcChannels,
			const std::vector<std::string>& availableMidiDevices,
			const TriggerParams& triggerParams,
			const SnapshotPtr& acceptedSnapshot);
		std::uint64_t _AllocateRevision() noexcept;

		// Coordinator/job side is the sole writer. Audio and input readers acquire
		// immutable snapshots atomically; ReleaseAfterReadersStopped performs final
		// teardown after both reader domains have stopped.
		std::atomic<std::uint64_t> _nextRevision{ 1u };
		std::atomic<SnapshotPtr> _accepted;
		std::atomic<SnapshotPtr> _staged;
		std::atomic<SnapshotPtr> _pending;
		std::atomic<std::uint64_t> _audioAcknowledgement{ 0u };
		std::atomic<std::uint64_t> _inputAcknowledgement{ 0u };
		std::atomic<bool> _editsEnabled{ false };
		std::atomic<bool> _shuttingDown{ false };
		std::mutex _retiringMutex;
		std::vector<SnapshotPtr> _retiring;
		std::vector<io::JamFile::Station> _stationDescriptors;
		std::vector<std::shared_ptr<Station>> _stations;
		unsigned int _availableAdcChannels = 0u;
		std::vector<std::string> _availableMidiDevices;
		TriggerParams _triggerParams;
	};
}
