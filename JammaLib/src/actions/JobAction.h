#pragma once

#include "Action.h"
#include "CommonTypes.h"
#include <memory>
#include <vector>

namespace base { class ActionReceiver; }
namespace midi { class MidiLoop; }
namespace vst { class IVstPlugin; }

namespace actions
{
	class JobAction :
		public base::Action
	{
	public:
		JobAction();
		~JobAction();

		bool operator==(const JobAction& other) const {
			if (other.JobActionType != JobActionType)
				return false;
			if (other.SourceId != SourceId)
				return false;
			// VST jobs carry path/index payloads — two jobs for the same source
			// but different plugins must not be treated as duplicates.
			// LOADVST is keyed on path; UNLOADVST is keyed on slot index.
			if (JobActionType == JOB_LOADVST)
				return other.VstPath == VstPath;
			if (JobActionType == JOB_UNLOADVST)
				return other.VstIndex == VstIndex;
			return true;
		}

	public:
		enum JobType
		{
			JOB_UPDATELOOPS,
			JOB_ENDRECORDING,
			JOB_UPDATEMIDIQUANTISATION,
			JOB_LOADVST,
			JOB_UNLOADVST
		};

		JobType JobActionType;
		std::string SourceId;
		std::weak_ptr<base::ActionReceiver> Receiver;

		// Payload for JOB_LOADVST: path to the .vst3 plugin.
		std::wstring VstPath;

		// Optional: plugin pre-initialised on the main thread before the job is
		// dispatched to the job thread.  If non-null, the engine OnAction uses
		// this instance directly instead of creating a fresh one.
		// Can be Vst3Plugin (VST3) or Vst2Plugin (VST2) — determined by extension.
		std::shared_ptr<vst::IVstPlugin> PreInitPlugin;

		// Payload for JOB_UNLOADVST / JOB_LOADVST: 0-based index in the chain.
		size_t VstIndex = 0;

		// Payload for JOB_UPDATEMIDIQUANTISATION: stable loop snapshot captured on
		// the UI thread before background processing begins.
		std::vector<std::shared_ptr<midi::MidiLoop>> MidiLoops;
	};
}
