#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include "../actions/ActionResult.h"
#include "../audio/AudioDevice.h"
#include "../io/JamFile.h"
#include "../io/UserConfig.h"
#include "../ninjam/NinjamController.h"
#include "../timing/TimingQuantiser.h"
#include "../engine/Station.h"

namespace io
{
	class IoSessionExporter
	{
	public:
		static actions::ActionResult ExportSession(const std::vector<std::shared_ptr<engine::Station>>& stations,
			const timing::TimingQuantiser& quantisation,
			io::JamFile::GlobalMidiQuantState globalMidiQuantState,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& streamParams,
			audio::AudioDevice* device,
			std::mutex& sceneMutex,
			const std::shared_ptr<ninjam::NinjamController>& ninjamController);
	};
}
