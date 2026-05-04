#pragma once

#include "Action.h"
#include "CommonTypes.h"

namespace base { class ActionReceiver; }

namespace actions
{
	class JobAction :
		public base::Action
	{
	public:
		JobAction();
		~JobAction();

		bool operator==(const JobAction& other) const {
			if (other.JobActionType == JobActionType)
				return other.SourceId == SourceId;

			return false;
		}

	public:
		enum JobType
		{
			JOB_UPDATELOOPS,
			JOB_ENDRECORDING,
			JOB_LOADVST,
			JOB_UNLOADVST
		};

		JobType JobActionType;
		std::string SourceId;
		std::weak_ptr<base::ActionReceiver> Receiver;

		// Payload for JOB_LOADVST: path to the .vst3 plugin.
		std::wstring VstPath;

		// Payload for JOB_UNLOADVST / JOB_LOADVST: 0-based index in the chain.
		size_t VstIndex = 0;
	};
}
