#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <string>
#include "../ninjam/NinjamController.h"
#include "../engine/Station.h"
#include "../engine/StationRemote.h"
#include "../engine/Quantiser.h"
#include "../io/UserConfig.h"
#include "NinjamTimingCoordinator.h"

namespace ninjam
{
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

		void PrepareTempoSyncOnConnect(const std::optional<engine::QuantisationTiming>& localTiming);

		void ResetTempoSyncOnDisconnect();
		NinjamTimingUpdate ObserveSessionStatus(const NinjamSessionTimingStatus& status,
			const std::optional<engine::QuantisationTiming>& localTiming);

		bool UpdateRemoteStationsFromSnapshot(const NinjamRemoteSnapshot& snapshot,
			std::vector<std::shared_ptr<engine::Station>>& stations);

		NinjamTimingUpdate ObserveTiming(const NinjamTiming& timing,
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent,
			const io::UserConfig& userConfig,
			utils::Timer& clock);
		NinjamTimingUpdate TickTiming(
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent,
			utils::Timer& clock,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		NinjamTimingUpdate ResolveRemoteTempoPromptDecision(bool accept,
			const std::optional<engine::QuantisationTiming>& localTiming,
			utils::Timer& clock);
		std::optional<NinjamTempoChange> PendingRemoteTempoPrompt() const
		{
			return _timingCoordinator.PendingTempoChange();
		}
		void SendTempoRequest(const NinjamTempoRequest& request);
		bool HasConnectedTiming() const noexcept { return _timingCoordinator.IsConnected(); }
		TempoRequestState TempoJoinRequestState() const noexcept { return _timingCoordinator.RequestState(); }
		NinjamTimingDiagnostics TimingDiagnostics() const noexcept { return _timingCoordinator.Diagnostics(); }

	private:
		std::shared_ptr<ninjam::NinjamController> _ninjamController;
		NinjamTempoJoinOptions _tempoJoinOptions{};
		NinjamTimingCoordinator _timingCoordinator;
	};
}
