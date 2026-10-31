#pragma once

#include "Action.h"
#include "CommonTypes.h"
#include <memory>
#include <vector>
#include <cstdint>

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
			// VST jobs include stable slot identity as well as path/index.
			// Distinct staged unload requests need distinct completions even when
			// they target the same slot; the later removal safely becomes a no-op.
			if (JobActionType == JOB_LOADVST)
				return other.VstPath == VstPath && other.VstIdentity == VstIdentity &&
					other.VstManaged == VstManaged && other.VstDiscardOnFailure == VstDiscardOnFailure;
			if (JobActionType == JOB_UNLOADVST)
				return other.VstIndex == VstIndex && other.VstIdentity == VstIdentity && other.VstRequestId == VstRequestId;
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
		std::uint64_t CaptureGeneration = 0u;
		bool StartupDiagnostic = false; // Origin metadata; excluded from action identity.
		std::string SourceId;
		std::weak_ptr<base::ActionReceiver> Receiver;

		// Payload for JOB_LOADVST: path to the plugin.
		std::wstring VstPath;
		std::string VstIdentity; // Captured slot identity; stale jobs must never remove a replacement.
		std::string VstRequestId; // Distinct staged completions must not be lost to queue deduplication.
		bool VstManaged = false;
		bool VstDiscardOnFailure = false; // Fresh UI append; saved/restored slots remain recoverable.

		// Optional: plugin pre-initialised on the main thread before the job is
		// dispatched to the job thread.  If non-null, the engine OnAction uses
		// this instance directly instead of creating a fresh one.
		// Can be Vst3Plugin (VST3) or Vst2Plugin (VST2) — determined by extension.
		std::shared_ptr<vst::IVstPlugin> PreInitPlugin;

		// Optional: VST2 state blob to restore immediately after a successful load.
		// Populated from JamFile::VstEntry::DecodeState() on file load.
		// Empty means no state to restore (normal interactive loads).
		std::vector<std::uint8_t> VstInitialState;

		// Payload for JOB_UNLOADVST / JOB_LOADVST: 0-based index in the chain.
		size_t VstIndex = 0;

		// Payload for JOB_UPDATEMIDIQUANTISATION: stable loop snapshot captured on
		// the UI thread before background processing begins.
		std::vector<std::shared_ptr<midi::MidiLoop>> MidiLoops;
	};
}
