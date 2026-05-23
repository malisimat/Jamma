#include "JobAction.h"
#include "../vst/VstPlugin.h"

using namespace actions;

JobAction::JobAction() :
	JobActionType(JOB_UPDATELOOPS),
	SourceId(""),
	Receiver(std::shared_ptr<base::ActionReceiver>())
{
}

JobAction::~JobAction()
{
}
