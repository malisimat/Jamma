#pragma once

#include <memory>
#include <vector>
#include <string>
#include "../ninjam/NinjamController.h"
#include "../engine/Station.h"
#include "../engine/StationRemote.h"
#include "../timing/TimingQuantiser.h"
#include "../io/UserConfig.h"

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

	private:
		std::shared_ptr<ninjam::NinjamController> _ninjamController;
	};
}
