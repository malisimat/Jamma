#include "TriggerAction.h"

using namespace actions;

TriggerAction::TriggerAction() :
	ActionType(TriggerActionType::TRIGGER_REC_START),
	TargetId(""),
	SourceId(""),
	SampleCount(0),
	InputChannels({}),
	MidiInputChannels({}),
	MidiInputDevices({}),
	ApplyToTargetTake(true),
	ApplyToSourceTake(true),
	ApplyToTargetAudio(true),
	ApplyToTargetMidi(true)
{
}

TriggerAction::~TriggerAction()
{
}
