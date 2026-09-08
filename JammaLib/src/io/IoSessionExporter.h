#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include "../actions/ActionResult.h"
#include "../audio/AudioDevice.h"
#include "../io/JamFile.h"
#include "../io/UserConfig.h"
#include "../ninjam/NinjamController.h"
#include "../engine/Quantiser.h"
#include "../engine/Station.h"

namespace io
{
	class IoSessionExporter
	{
	public:
		static actions::ActionResult ExportSession(const std::vector<std::shared_ptr<engine::Station>>& stations,
			const engine::Quantiser& quantisation,
			io::JamFile::GlobalMidiQuantState globalMidiQuantState,
			double transportOffsetLoopFrac,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& streamParams,
			audio::AudioDevice* device,
			std::mutex& sceneMutex,
			const std::shared_ptr<ninjam::NinjamController>& ninjamController);

		// Non-UI entry point for the normal paused, scene-locked save workflow.
		// The manifest is published only after every sidecar has been staged.
		// Returns true only when session.jam has been published successfully.
		static bool ExportSessionToDirectory(const std::vector<std::shared_ptr<engine::Station>>& stations,
			const engine::Quantiser& quantisation,
			io::JamFile::GlobalMidiQuantState globalMidiQuantState,
			double transportOffsetLoopFrac,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& streamParams,
			audio::AudioDevice* device,
			std::mutex& sceneMutex,
			const std::shared_ptr<ninjam::NinjamController>& ninjamController,
			const std::wstring& exportDir);
	};
}
