#pragma once

#include <memory>
#include <functional>
#include <mutex>
#include <vector>
#include "../actions/ActionResult.h"
#include "../audio/AudioHost.h"
#include "../io/JamFile.h"
#include "../io/UserConfig.h"
#include "../ninjam/NinjamController.h"
#include "../engine/Quantiser.h"
#include "../engine/RigSnapshot.h"
#include "../engine/Station.h"

namespace io
{
	class IoSessionExporter
	{
	public:
        struct ExportResult
        {
            bool Success = false;
            bool Cancelled = false;
            bool AudioResumed = true;
            std::wstring Path;
            std::string Error;
        };

        // Control owner excludes job/UI/trigger writers before calling.
        // beforeCapture runs quiescent, outside Scene mutex, to exclude MIDI
        // writers and recheck capture admission. afterCapture releases caller
        // exclusions after audio restoration, before disk work owns only copies.
        // Caller must also release exclusions on early preflight/pause failure.
        static ExportResult ExportSessionVersionToDirectory(const std::vector<std::shared_ptr<engine::Station>>& stations,
            const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot,
            const engine::Quantiser& quantisation,
            io::JamFile::GlobalMidiQuantState globalMidiQuantState,
            double transportOffsetLoopFrac,
            const io::UserConfig& userConfig,
            const audio::AudioStreamParams& streamParams,
            audio::AudioHost* host,
            std::mutex& sceneMutex,
            const std::shared_ptr<ninjam::NinjamController>& ninjamController,
            const std::wstring& exportDir,
            const std::function<bool(std::string&)>& afterCapture = {},
            const std::function<bool(std::string&)>& beforeCapture = {}, const engine::PerformanceGroups* groups = nullptr);

		static actions::ActionResult ExportSession(const std::vector<std::shared_ptr<engine::Station>>& stations,
			const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot,
			const engine::Quantiser& quantisation,
			io::JamFile::GlobalMidiQuantState globalMidiQuantState,
			double transportOffsetLoopFrac,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& streamParams,
			audio::AudioDevice* device,
			std::mutex& sceneMutex,
			const std::shared_ptr<ninjam::NinjamController>& ninjamController,
			const engine::PerformanceGroups* groups = nullptr);

		// Legacy offline/test entry point; rejects a non-null AudioDevice because
        // device pause alone does not prove callback quiescence.
		// The manifest is published only after every sidecar has been staged.
		// Returns true only when session.jam has been published successfully.
        // Repeated offline calls do not overwrite a prior manifest.
		static bool ExportSessionToDirectory(const std::vector<std::shared_ptr<engine::Station>>& stations,
			const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot,
			const engine::Quantiser& quantisation,
			io::JamFile::GlobalMidiQuantState globalMidiQuantState,
			double transportOffsetLoopFrac,
			const io::UserConfig& userConfig,
			const audio::AudioStreamParams& streamParams,
			audio::AudioDevice* device,
			std::mutex& sceneMutex,
			const std::shared_ptr<ninjam::NinjamController>& ninjamController,
			const std::wstring& exportDir, const engine::PerformanceGroups* groups = nullptr);
    private:
        static ExportResult ExportToDirectory(const std::vector<std::shared_ptr<engine::Station>>& stations,
            const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot,
            const engine::Quantiser& quantisation,
            io::JamFile::GlobalMidiQuantState globalMidiQuantState,
            double transportOffsetLoopFrac,
            const io::UserConfig& userConfig,
            const audio::AudioStreamParams& streamParams,
            audio::AudioHost* host,
            std::mutex& sceneMutex,
            const std::shared_ptr<ninjam::NinjamController>& ninjamController,
            const std::wstring& exportDir, bool numbered,
            const std::function<bool(std::string&)>& afterCapture,
            const std::function<bool(std::string&)>& beforeCapture, const engine::PerformanceGroups* groups);
	};
}
