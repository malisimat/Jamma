#pragma once

#include <memory>
#include <vector>
#include <string>
#include "../ninjam/NinjamController.h"
#include "Station.h"
#include "StationRemote.h"
#include "Quantisation.h"
#include "../io/UserConfig.h"

namespace engine
{
	class NetworkService
	{
	public:
		NetworkService();
		~NetworkService() = default;

		std::shared_ptr<ninjam::NinjamController> GetController() const { return _ninjamController; }

		void SendChat(const std::string& msg);
		void Connect(const std::string& host);
		void Disconnect();

		bool UpdateRemoteStationsFromSnapshot(const ninjam::NinjamRemoteSnapshot& snapshot,
			std::vector<std::shared_ptr<Station>>& stations);

		void ApplyRemoteTempoToClock(const ninjam::NinjamRemoteSnapshot& snapshot,
			Quantisation& quantisation,
			const std::vector<std::shared_ptr<Station>>& stations,
			const io::UserConfig& userConfig);

		void QueueLocalTempoFromClock(Quantisation& quantisation,
			const io::UserConfig& userConfig,
			unsigned int currentSampleRate);

		void SendQueuedTempoAtIntervalWrap(const ninjam::NinjamRemoteSnapshot& snapshot,
			Quantisation& quantisation,
			unsigned int currentSampleRate);

	private:
		std::shared_ptr<ninjam::NinjamController> _ninjamController;
	};
}
