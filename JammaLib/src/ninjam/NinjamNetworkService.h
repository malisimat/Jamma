#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <string>
#include "../ninjam/NinjamController.h"
#include "../engine/Station.h"
#include "../engine/StationRemote.h"
#include "../timing/TimingQuantiser.h"
#include "../timing/ExternalTransport.h"
#include "../io/UserConfig.h"

namespace ninjam
{
	struct NinjamTempoJoinOptions
	{
		bool PushLocalTempoOnJoin = false;
		bool PromptBeforeApplyingRemoteTempo = true;
	};

	class NinjamNetworkService
	{
	public:
		NinjamNetworkService();
		~NinjamNetworkService() = default;

		std::shared_ptr<ninjam::NinjamController> GetController() const { return _ninjamController; }

		void SendChat(const std::string& msg);
		void Connect(const std::string& host);
		void Disconnect();

		void SetTempoJoinOptions(const NinjamTempoJoinOptions& options);
		const NinjamTempoJoinOptions& TempoJoinOptions() const noexcept { return _tempoJoinOptions; }

		void PrepareTempoSyncOnConnect(timing::TimingQuantiser& quantisation,
			unsigned int currentSampleRate);

		void ResetTempoSyncOnDisconnect(timing::TimingQuantiser& quantisation);

		bool UpdateRemoteStationsFromSnapshot(const NinjamRemoteSnapshot& snapshot,
			std::vector<std::shared_ptr<engine::Station>>& stations);

		void ApplyRemoteTempoToClock(const NinjamRemoteSnapshot& snapshot,
			timing::TimingQuantiser& quantisation,
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			const io::UserConfig& userConfig);

		void QueueLocalTempoFromClock(timing::TimingQuantiser& quantisation,
			const io::UserConfig& userConfig,
			unsigned int currentSampleRate);

		void SendQueuedTempoAtIntervalWrap(const NinjamRemoteSnapshot& snapshot,
			timing::TimingQuantiser& quantisation,
			unsigned int currentSampleRate);

		void HandleRemoteTempoSnapshot(const NinjamRemoteSnapshot& snapshot,
			timing::TimingQuantiser& quantisation,
			const std::vector<std::shared_ptr<engine::Station>>& stations,
			const io::UserConfig& userConfig);

		std::optional<timing::PendingRemoteTempoChange> PendingRemoteTempoPrompt() const
		{
			return _pendingRemoteTempoPrompt;
		}

		void ResolveRemoteTempoPromptDecision(bool accept,
			timing::TimingQuantiser& quantisation,
			const std::vector<std::shared_ptr<engine::Station>>& stations);

		// Lock-free read of the authoritative connected-sync transport state.
		std::shared_ptr<const timing::ExternalTransportState> PublishedTransportState() const noexcept
		{
			return _externalTransport.Published();
		}

		void SetExternalTransportDiagnostics(bool enabled) noexcept
		{
			_externalTransport.SetDiagnosticsEnabled(enabled);
		}

	private:
		static bool IsSameRemoteTempoChange(const timing::PendingRemoteTempoChange& lhs,
			const timing::PendingRemoteTempoChange& rhs) noexcept;

		// Ingests the current snapshot into the external transport and applies
		// wrap-gated phase discipline to the master clock while connected.
		void _FeedExternalTransport(const NinjamRemoteSnapshot& snapshot,
			timing::TimingQuantiser& quantisation);

		std::shared_ptr<ninjam::NinjamController> _ninjamController;
		NinjamTempoJoinOptions _tempoJoinOptions{};
		bool _joinPushAwaitingOutcome = false;
		std::optional<timing::PendingRemoteTempoChange> _pendingRemoteTempoPrompt;
		std::optional<timing::PendingRemoteTempoChange> _ignoredRemoteTempoPrompt;

		// Runtime-only continuous NINJAM transport sync (never persisted).
		timing::ExternalTransport _externalTransport;
		bool _externalJoinAligned = false;
	};
}
