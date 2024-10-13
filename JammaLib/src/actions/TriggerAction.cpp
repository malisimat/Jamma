#include "TriggerAction.h"

using namespace actions;

TriggerAction::TriggerAction() :
	ActionType(TriggerActionType::TRIGGER_REC_START),
	TargetId(""),
	OverdubTargetId(""),
	SampleCount(0),
	InputChannels({})
{
}

TriggerAction::~TriggerAction()
{
}
