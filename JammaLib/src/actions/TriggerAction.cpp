#include "TriggerAction.h"

using namespace actions;

TriggerAction::TriggerAction() :
	ActionType(TriggerActionType::TRIGGER_REC_START),
	TargetId(""),
	SourceId(""),
	SampleCount(0),
	InputChannels({}),
	ApplyToTargetTake(true),
	ApplyToSourceTake(true)
{
}

TriggerAction::~TriggerAction()
{
}
