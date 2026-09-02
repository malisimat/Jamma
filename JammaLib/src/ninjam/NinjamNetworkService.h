#pragma once

#include <memory>
#include <mutex>
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
		NinjamTempoJoinOptions TempoJoinOptions() const noexcept;

		NinjamTimingUpdate PrepareTempoSyncOnConnect(
			const std::optional<engine::QuantisationTiming>& localTiming);

		NinjamTimingUpdate ResetTempoSyncOnDisconnect();
		NinjamTimingUpdate ObserveSessionStatus(const NinjamSessionTimingStatus& status,
			const std::optional<engine::QuantisationTiming>& localTiming);

		bool UpdateRemoteStationsFromSnapshot(const NinjamRemoteSnapshot& snapshot,
			std::vector<std::shared_ptr<engine::Station>>& stations);

		NinjamTimingUpdate ObserveTiming(const NinjamTiming& timing,
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent,
			const io::UserConfig& userConfig,
			utils::Timer& clock,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		NinjamTimingUpdate TickTiming(
			const std::optional<engine::QuantisationTiming>& localTiming,
			bool hasLocalContent,
			utils::Timer& clock,
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		NinjamTimingUpdate ResolveRemoteTempoPromptDecision(bool accept,
			const std::optional<engine::QuantisationTiming>& localTiming,
			utils::Timer& clock);
		std::optional<NinjamTempoChange> PendingRemoteTempoPrompt() const;
		void SendTempoRequest(const NinjamTempoRequest& request);
		bool HasConnectedTiming() const noexcept;
		TempoRequestState TempoJoinRequestState() const noexcept;
		NinjamTimingDiagnostics TimingDiagnostics() const noexcept;

	private:
		std::shared_ptr<ninjam::NinjamController> _ninjamController;
		NinjamTempoJoinOptions _tempoJoinOptions{};
		NinjamTimingCoordinator _timingCoordinator;
		mutable std::mutex _timingMutex;
	};
}
