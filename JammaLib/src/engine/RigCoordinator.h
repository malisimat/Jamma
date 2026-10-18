#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
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
		enum class EditResult { Pending, EditsDisabled, AudioCallbackInactive, TriggerBusy, TransitionRejected, ValidationFailed, PersistenceFailed, ReinitialisationRequired };

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
		bool MergeHardwarePreferences(const std::optional<io::UserConfig::AudioSettings>& audio,
			const std::optional<std::vector<io::UserConfig::MidiSettings>>& midi);
		EditResult SubmitCandidate(const io::RigFile& candidateRig);
		EditResult SubmitStationRename(const std::shared_ptr<Station>& station,
			std::uint64_t expectedRevision, const std::string& name);
		// Serialized Scene owner, off callback. Normalize against the old inventory.
		EditResult SubmitInventory(const std::vector<std::shared_ptr<Station>>& stations,
			std::shared_ptr<Station> removed = {});
		// Refresh source availability after the audio stream and MIDI inputs actually open.
		SnapshotPtr RefreshRuntimeAvailability(unsigned int availableAdcChannels,
			std::vector<std::string> availableMidiDevices,
			std::optional<std::vector<std::string>> availableSerialDevices = std::nullopt,
			std::optional<io::UserConfig::AudioSettings> confirmedGeneratedAudio = std::nullopt);
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
			const SnapshotPtr& acceptedSnapshot,
			const std::vector<std::string>* availableSerialDevices = nullptr);
		std::uint64_t _AllocateRevision() noexcept;

		// Retire snapshots only after audio and input readers stop, to keep teardown off their threads.
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
		TriggerParams _triggerParams;
	};
}
