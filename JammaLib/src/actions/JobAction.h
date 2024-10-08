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
			JOB_ENDRECORDING
		};

		JobType JobActionType;
		std::string SourceId;
		std::weak_ptr<base::ActionReceiver> Receiver;
	};
}
