#pragma once

#include <vector>
#include "Action.h"
#include "CommonTypes.h"

namespace actions
{
	class TriggerAction :
		public base::Action
	{
	public:
		TriggerAction();
		~TriggerAction();

	public:
		enum TriggerActionType
		{
			TRIGGER_REC_START,
			TRIGGER_REC_END,
			TRIGGER_DITCH,
			TRIGGER_OVERDUB_START,
			TRIGGER_OVERDUB_END,
			TRIGGER_OVERDUB_DITCH,
			TRIGGER_PUNCHIN_START,
			TRIGGER_PUNCHIN_END
		};

	public:
		TriggerActionType ActionType;
		std::string TargetId;
		std::string SourceId;
		unsigned long SampleCount;
		std::vector<unsigned int> InputChannels;
	};
}
