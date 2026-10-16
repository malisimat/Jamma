#pragma once

#include "JamFile.h"
#include "RigFile.h"
#include "../engine/RigSnapshot.h"

namespace io
{
	// Persistence reconstruction only; Scene owns rig acceptance and ordered installation.
	class IoSessionLoader
	{
	public:
		struct MasterPosition { unsigned long LengthSamps; std::uint64_t AbsoluteSamplePos; };
		struct ReconstructedSession
		{
			std::vector<std::shared_ptr<engine::Station>> Stations;
			std::vector<JamFile::Station> StationDescriptors;
			unsigned int HudAudioInputCount = 0u;
			std::vector<std::string> HudMidiInputs;
			engine::TriggerParams TriggerParameters;
			// Present only for initialized current-schema transport with nonzero length.
			std::optional<MasterPosition> SavedMaster;
		};

		static ReconstructedSession Reconstruct(const JamFile& jam, const RigFile& rig, const std::wstring& directory);
		// Exactly once on the loading thread, on a fresh rig before input/audio publication.
		static void RestoreTriggerHistories(const std::vector<JamFile::TriggerHistory>& histories,
			const engine::RigSnapshot& initialRig,
			const std::vector<std::shared_ptr<engine::Station>>& stations);
	};
}
