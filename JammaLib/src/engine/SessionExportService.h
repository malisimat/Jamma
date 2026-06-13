#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include "../actions/ActionResult.h"
#include "../audio/AudioDevice.h"
#include "../io/UserConfig.h"
#include "../ninjam/NinjamController.h"
#include "Quantisation.h"
#include "Station.h"

namespace engine
{
	class SessionExportService
	{
	public:
		static actions::ActionResult ExportSession(const std::vector<std::shared_ptr<Station>>& stations,
			const Quantisation& quantisation,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& streamParams,
			audio::AudioDevice* device,
			std::mutex& sceneMutex,
			const std::shared_ptr<ninjam::NinjamController>& ninjamController);
	};
}
